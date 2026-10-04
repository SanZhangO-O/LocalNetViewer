// 图片解码/编码/缩放实现（对应 Python util/imaging.py；stb 实现）
// 注意：STB_*_IMPLEMENTATION 宏全工程只能在此处出现一次。
#include "lnv/util/imaging.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE2_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"
#include "stb_image_resize2.h"
#include <miniz.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#ifdef _WIN32
// WIC：stb 不支持 WebP/TIFF/AVIF，而 Python(Pillow) 支持；用系统解码器兜底
#include <wincodec.h>
#include <objbase.h>
#include <windows.h>
#endif

namespace lnv {

namespace {

// ---------------------------------------------------------------- 小端/大端读取
unsigned rd16le(const std::string& d, size_t o) {
    if (o + 2 > d.size()) return 0;
    return unsigned(unsigned char(d[o])) | (unsigned(unsigned char(d[o + 1])) << 8);
}
unsigned rd32le(const std::string& d, size_t o) {
    if (o + 4 > d.size()) return 0;
    return unsigned(unsigned char(d[o])) | (unsigned(unsigned char(d[o + 1])) << 8) |
           (unsigned(unsigned char(d[o + 2])) << 16) | (unsigned(unsigned char(d[o + 3])) << 24);
}
unsigned rd16be(const std::string& d, size_t o) {
    if (o + 2 > d.size()) return 0;
    return (unsigned(unsigned char(d[o])) << 8) | unsigned(unsigned char(d[o + 1]));
}
unsigned rd32be(const std::string& d, size_t o) {
    if (o + 4 > d.size()) return 0;
    return (unsigned(unsigned char(d[o])) << 24) | (unsigned(unsigned char(d[o + 1])) << 16) |
           (unsigned(unsigned char(d[o + 2])) << 8) | unsigned(unsigned char(d[o + 3]));
}
bool starts_with(const std::string& d, size_t o, const char* s, size_t n) {
    return o + n <= d.size() && memcmp(d.data() + o, s, n) == 0;
}

// ---------------------------------------------------------------- EXIF Orientation 解析
constexpr unsigned EXIF_ORIENTATION_TAG = 0x0112;
constexpr unsigned TIFF_TAG_IMAGE_WIDTH = 256;
constexpr unsigned TIFF_TAG_IMAGE_LENGTH = 257;

// 在一段 TIFF 数据（IFD0）里查标签值（假定值为内联 SHORT/LONG）
bool tiff_find_tag(const std::string& d, size_t base, size_t len, unsigned tag, unsigned* value) {
    if (base + 8 > d.size()) return false;
    bool le = d[base] == 'I' && d[base + 1] == 'I';  // II=小端, MM=大端
    unsigned (*rd16)(const std::string&, size_t) = le ? rd16le : rd16be;
    unsigned (*rd32)(const std::string&, size_t) = le ? rd32le : rd32be;
    if (rd16(d, base + 2) != 42) return false;
    unsigned ifd = rd32(d, base + 4);
    if (ifd == 0 || base + ifd + 2 > d.size()) return false;
    unsigned n = rd16(d, base + ifd);
    for (unsigned i = 0; i < n; i++) {
        size_t e = base + ifd + 2 + size_t(i) * 12;
        if (e + 12 > d.size() || e + 12 > base + len) return false;
        if (rd16(d, e) != tag) continue;
        unsigned type = rd16(d, e + 2);
        if (type == 3) *value = rd16(d, e + 8);        // SHORT
        else if (type == 4) *value = rd32(d, e + 8);   // LONG
        else return false;
        return true;
    }
    return false;
}

// 解析 EXIF Orientation（1..8，未知返回 1）；来源：JPEG APP1 Exif 或 TIFF 头
int parse_orientation(const std::string& d) {
    // TIFF 本体：直接读 IFD0
    if (d.size() >= 8 && (starts_with(d, 0, "II*\0", 4) || starts_with(d, 0, "MM\0*", 4))) {
        unsigned v = 1;
        if (tiff_find_tag(d, 0, d.size(), EXIF_ORIENTATION_TAG, &v) && v >= 1 && v <= 8) return int(v);
        return 1;
    }
    // JPEG：扫描 marker 段找 APP1 "Exif\0\0"
    if (d.size() < 4 || unsigned char(d[0]) != 0xFF || unsigned char(d[1]) != 0xD8) return 1;
    size_t pos = 2;
    while (pos + 4 <= d.size()) {
        if (unsigned char(d[pos]) != 0xFF) return 1;
        unsigned char m = unsigned char(d[pos + 1]);
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { pos += 2; continue; }  // 无长度段
        if (m == 0xDA || m == 0xD9) return 1;  // 进入扫描数据，后面没有头部信息了
        unsigned seglen = rd16be(d, pos + 2);
        if (seglen < 2) return 1;
        if (m == 0xE1 && starts_with(d, pos + 4, "Exif\0\0", 6)) {
            unsigned v = 1;
            size_t tiff_off = pos + 10, tiff_len = size_t(seglen) - 8;
            if (tiff_off < d.size() &&
                tiff_find_tag(d, tiff_off, tiff_len, EXIF_ORIENTATION_TAG, &v) && v >= 1 && v <= 8)
                return int(v);
            return 1;
        }
        pos += 2 + seglen;
    }
    return 1;
}

// ---------------------------------------------------------------- 各格式头部尺寸探测
// JPEG：跳过 marker 找 SOF（C0-CF，排除 C4/C8/CC）
bool probe_jpeg(const std::string& d, int* w, int* h) {
    size_t pos = 2;
    while (pos + 4 <= d.size()) {
        if (unsigned char(d[pos]) != 0xFF) return false;
        unsigned char m = unsigned char(d[pos + 1]);
        if (m == 0xFF) { pos++; continue; }  // 填充字节
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { pos += 2; continue; }
        if (m == 0xDA || m == 0xD9) return false;  // SOF 之前未找到
        unsigned seglen = rd16be(d, pos + 2);
        if (seglen < 2) return false;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (pos + 9 > d.size()) return false;
            *h = int(rd16be(d, pos + 5));  // JPEG SOF 里高度在前
            *w = int(rd16be(d, pos + 7));
            return *w > 0 && *h > 0;
        }
        pos += 2 + seglen;
    }
    return false;
}
bool probe_png(const std::string& d, int* w, int* h) {
    if (d.size() < 24) return false;
    *w = int(rd32be(d, 16));
    *h = int(rd32be(d, 20));
    return *w > 0 && *h > 0;
}
bool probe_gif(const std::string& d, int* w, int* h) {
    if (d.size() < 10) return false;
    *w = int(rd16le(d, 6));
    *h = int(rd16le(d, 8));
    return *w > 0 && *h > 0;
}
bool probe_bmp(const std::string& d, int* w, int* h) {
    if (d.size() < 26) return false;
    unsigned hsz = rd32le(d, 14);
    if (hsz == 12) {  // BITMAPCOREHEADER
        *w = int(rd16le(d, 18));
        *h = int(rd16le(d, 20));
    } else {
        *w = int(rd32le(d, 18));
        *h = int(rd32le(d, 22));  // 顶朝下 BMP 为负
        if (*h < 0) *h = -*h;
    }
    return *w > 0 && *h > 0;
}
bool probe_webp(const std::string& d, int* w, int* h) {
    if (d.size() < 30) return false;
    std::string fourcc = d.substr(12, 4);
    if (fourcc == "VP8 ") {  // 有损：帧头 sync 0x9d 0x01 0x2a 之后 14bit 尺寸
        if (unsigned char(d[23]) != 0x9d || unsigned char(d[24]) != 0x01 ||
            unsigned char(d[25]) != 0x2a)
            return false;
        *w = int(rd16le(d, 26) & 0x3FFF);
        *h = int(rd16le(d, 28) & 0x3FFF);
    } else if (fourcc == "VP8L") {  // 无损：签名 0x2F 后 14+14bit（LSB 优先）
        if (unsigned char(d[20]) != 0x2F) return false;
        unsigned bits = rd32le(d, 21);
        *w = int((bits & 0x3FFF) + 1);
        *h = int(((bits >> 14) & 0x3FFF) + 1);
    } else if (fourcc == "VP8X") {  // 扩展：画布尺寸-1，各 24bit LE
        *w = int((unsigned(d[24]) | (unsigned(d[25]) << 8) | (unsigned(d[26]) << 16)) + 1);
        *h = int((unsigned(d[27]) | (unsigned(d[28]) << 8) | (unsigned(d[29]) << 16)) + 1);
    } else {
        return false;
    }
    return *w > 0 && *h > 0;
}
bool probe_tiff(const std::string& d, int* w, int* h) {
    unsigned iw = 0, ih = 0;
    if (!tiff_find_tag(d, 0, d.size(), TIFF_TAG_IMAGE_WIDTH, &iw)) return false;
    if (!tiff_find_tag(d, 0, d.size(), TIFF_TAG_IMAGE_LENGTH, &ih)) return false;
    *w = int(iw);
    *h = int(ih);
    return *w > 0 && *h > 0;
}

// ---------------------------------------------------------------- EXIF 方向变换（RGBA）
// o = Orientation 值；2/4 镜像，3/6/8 旋转，5/7 镜像+旋转（宽高互换）
ImageBuf apply_orientation(const ImageBuf& src, int o) {
    if (o <= 1 || o > 8 || src.w <= 0 || src.h <= 0) return src;
    // 目标尺寸：5/6/7/8 旋转 90°，宽高互换
    int w = (o >= 5) ? src.h : src.w;
    int h = (o >= 5) ? src.w : src.h;
    ImageBuf out;
    out.w = w;
    out.h = h;
    out.has_alpha = src.has_alpha;
    out.rgba.resize(size_t(w) * h * 4);
    auto px = [&](int x, int y) -> const unsigned char* {
        return &src.rgba[(size_t(y) * src.w + x) * 4];
    };
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx = 0, sy = 0;
            switch (o) {
                case 2: sx = src.w - 1 - x; sy = y; break;                          // 左右镜像
                case 3: sx = src.w - 1 - x; sy = src.h - 1 - y; break;              // 旋转 180°
                case 4: sx = x; sy = src.h - 1 - y; break;                          // 上下镜像
                case 5: sx = y; sy = x; break;                                      // 转置
                case 6: sx = y; sy = src.h - 1 - x; break;                          // 旋转 90° 顺时针
                case 7: sx = src.w - 1 - y; sy = src.h - 1 - x; break;              // 反转置
                case 8: sx = src.w - 1 - y; sy = x; break;                          // 旋转 90° 逆时针
            }
            memcpy(&out.rgba[(size_t(y) * w + x) * 4], px(sx, sy), 4);
        }
    }
    return out;
}

// 扫描 alpha：存在非 255 的 alpha 通道才视为带透明
bool scan_alpha(const std::vector<unsigned char>& rgba) {
    for (size_t i = 3; i < rgba.size(); i += 4)
        if (rgba[i] != 255) return true;
    return false;
}

// ---------------------------------------------------------------- 编码回调与辅助
void stb_write_cb(void* ctx, void* data, int size) {
    static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), size_t(size));
}

// RGBA → RGB（Python to_rgb：透明区域合成到白底）
std::vector<unsigned char> to_rgb_buffer(const ImageBuf& img) {
    std::vector<unsigned char> out(size_t(img.w) * img.h * 3);
    for (size_t i = 0, j = 0; i < out.size(); i += 3, j += 4) {
        unsigned a = img.rgba[j + 3];
        for (int c = 0; c < 3; c++) {
            // a=255 直通；否则按 alpha 合成到 255 白底
            unsigned v = img.rgba[j + c];
            out[i + c] = unsigned char(a == 255 ? v : (v * a + 255 * (255 - a) + 127) / 255);
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------- sniff_mime / mime_from_ext
std::string mime_from_ext(const std::string& ext) {
    std::string e = ext;
    for (auto& c : e) c = char(tolower(unsigned char(c)));
    // Python mime_for_ext 表
    if (e == ".jpg" || e == ".jpeg" || e == ".jfif") return "image/jpeg";
    if (e == ".png") return "image/png";
    if (e == ".gif") return "image/gif";
    if (e == ".webp") return "image/webp";
    if (e == ".bmp") return "image/bmp";
    if (e == ".avif") return "image/avif";
    if (e == ".tif" || e == ".tiff") return "image/tiff";
    return "application/octet-stream";
}

std::string sniff_mime(const std::string& bytes, const std::string& fallback_ext) {
    // 魔数表（Python sniff_mime）；注意 Python 要求 len >= 12 才查表
    if (bytes.size() >= 12) {
        if (starts_with(bytes, 0, "\xFF\xD8\xFF", 3)) return "image/jpeg";
        if (starts_with(bytes, 0, "\x89PNG\r\n\x1a\n", 8)) return "image/png";
        if (starts_with(bytes, 0, "GIF87a", 6) || starts_with(bytes, 0, "GIF89a", 6))
            return "image/gif";
        if (starts_with(bytes, 0, "RIFF", 4) && starts_with(bytes, 8, "WEBP", 4))
            return "image/webp";
        if (starts_with(bytes, 0, "BM", 2)) return "image/bmp";
        if (starts_with(bytes, 0, "II*\0", 4) || starts_with(bytes, 0, "MM\0*", 4))
            return "image/tiff";
    }
    // 魔数认不出来 → 退回扩展名表（Python：mime_for_ext(fallback_ext)）。
    // 少了这一步，CBZ 里的 .avif/.tif/.jfif 页会以 application/octet-stream 下发，
    // 客户端按 Content-Type 分派时就显示不出来。
    if (!fallback_ext.empty()) return mime_from_ext(fallback_ext);
    return "application/octet-stream";
}

// ---------------------------------------------------------------- probe_size
std::pair<int, int> probe_size(const std::string& bytes) {
    int w = 0, h = 0;
    bool ok = false;
    if (bytes.size() >= 12) {
        if (starts_with(bytes, 0, "\xFF\xD8\xFF", 3)) ok = probe_jpeg(bytes, &w, &h);
        else if (starts_with(bytes, 0, "\x89PNG\r\n\x1a\n", 8)) ok = probe_png(bytes, &w, &h);
        else if (starts_with(bytes, 0, "GIF87a", 6) || starts_with(bytes, 0, "GIF89a", 6))
            ok = probe_gif(bytes, &w, &h);
        else if (starts_with(bytes, 0, "RIFF", 4) && starts_with(bytes, 8, "WEBP", 4))
            ok = probe_webp(bytes, &w, &h);
        else if (starts_with(bytes, 0, "BM", 2)) ok = probe_bmp(bytes, &w, &h);
        else if (starts_with(bytes, 0, "II*\0", 4) || starts_with(bytes, 0, "MM\0*", 4))
            ok = probe_tiff(bytes, &w, &h);
    }
    if (!ok) return {0, 0};
    // EXIF Orientation 5/6/7/8 = 旋转 90°，宽高互换（与 decode 校正后的几何一致）
    int o = parse_orientation(bytes);
    if (o >= 5 && o <= 8) std::swap(w, h);
    return {w, h};
}

// ---------------------------------------------------------------- decode_image
#ifdef _WIN32
namespace {

// 极简 COM 指针守卫
template <typename T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { if (p) p->Release(); }
    T** put() { return &p; }
    T* get() const { return p; }
    T* operator->() const { return p; }
};

// 每线程 COM 初始化（httplib 工作线程池；RPC_E_CHANGED_MODE 说明本线程已是 STA，
// 仍可正常使用 WIC，因此也视为成功）
bool com_ready() {
    static thread_local bool ready = [] {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }();
    return ready;
}

// 用 Windows Imaging Component 解码（stb 不支持的 WebP/TIFF/AVIF 等）。
// 失败返回 false（可能是系统没装对应解码器 → 调用方报"图片解码失败"）。
// max_source_pixels：AVIF 等 WIC-only 格式绕过了 probe_size/stbi_info 的头部预检，
// 必须在分配像素缓冲前用帧元数据尺寸做同样的 2× 上限检查——几 KB 的 AVIF 可以
// 声明 30000×30000，若等到 CopyPixels 才分配 ~3.6GB（bad_alloc → 500），
// 就不是 Python/Pillow 的 DecompressionBomb → 422 语义了。
bool wic_decode(const std::string& bytes, long long max_source_pixels, int* out_w, int* out_h,
                std::vector<unsigned char>* out_rgba) {
    if (bytes.empty() || bytes.size() > 0xFFFFFFFFull) return false;
    if (!com_ready()) return false;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.put()))))
        return false;
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.put()))) return false;
    if (FAILED(stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())),
            static_cast<DWORD>(bytes.size()))))
        return false;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr,
                                                WICDecodeMetadataCacheOnDemand,
                                                decoder.put())))
        return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) return false;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return false;
    // 与 stb/头部预检相同的 2×max_source_pixels 语义（检查在分配 out_rgba 之前）
    if (max_source_pixels > 0 &&
        static_cast<double>(w) * static_cast<double>(h) >
            2.0 * static_cast<double>(max_source_pixels))
        throw ImageTooLargeError();
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(conv.put()))) return false;
    if (FAILED(conv->Initialize(frame.get(), GUID_WICPixelFormat32bppRGBA,
                                WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    const size_t stride = static_cast<size_t>(w) * 4;
    out_rgba->resize(stride * h);
    if (FAILED(conv->CopyPixels(nullptr, static_cast<UINT>(stride),
                                static_cast<UINT>(out_rgba->size()), out_rgba->data())))
        return false;
    *out_w = static_cast<int>(w);
    *out_h = static_cast<int>(h);
    return true;
}

}  // namespace
#endif

ImageBuf decode_image(const std::string& bytes, long long max_source_pixels) {
    // 解码前用头部探测拦截图片炸弹。Pillow 的语义是 MAX_IMAGE_PIXELS 只发警告，
    // 超过 **2 倍** 才抛 DecompressionBombError（imaging.py 只把这个异常转成
    // ImageTooLargeError）——按 1 倍拦截会把 Python 能正常渲染的 30~60MP 扫描页
    // 变成"图片无法显示"。
    //
    // 探测失败时（TGA/PSD/PNM/HDR 等魔数表外的格式）必须再用 stb 的头部解析兜一层：
    // 只按魔数表判断会让这些格式完全绕过像素上限（审查 H7）。
    //
    // stb 的长度参数是 int：>2GB 的输入 static_cast<int> 会截断成负数，stb 可能
    // 越界读。这种输入直接跳过 stb，Windows 上交给 WIC（长度参数是 DWORD，上限 4GB），
    // 其余平台按解码失败处理。
    const bool stb_ok =
        bytes.size() <= static_cast<size_t>(std::numeric_limits<int>::max());
    if (max_source_pixels > 0) {
        auto [pw, ph] = probe_size(bytes);
        if (!(pw > 0 && ph > 0) && stb_ok) {
            int iw = 0, ih = 0, ic = 0;
            if (stbi_info_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()),
                                      int(bytes.size()), &iw, &ih, &ic)) {
                pw = iw;
                ph = ih;
            }
        }
        if (pw > 0 && ph > 0 && static_cast<double>(pw) * ph > 2.0 * max_source_pixels)
            throw ImageTooLargeError();
    }
    int w = 0, h = 0, n = 0;
    unsigned char* data = nullptr;
    if (stb_ok)
        data = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()),
                                     int(bytes.size()), &w, &h, &n, 4);
    std::vector<unsigned char> rgba;
    if (data) {
        rgba.assign(data, data + size_t(w) * h * 4);
        stbi_image_free(data);
    } else {
#ifdef _WIN32
        // stb 解不了 → 试系统解码器（WebP/TIFF/AVIF 等）
        if (!wic_decode(bytes, max_source_pixels, &w, &h, &rgba)) throw ImageDecodeError();
#else
        throw ImageDecodeError();
#endif
    }
    // 兜底复核：头部探测与真实解码尺寸可能不一致（畸形文件），此处只做拦截
    if (max_source_pixels > 0 &&
        static_cast<double>(w) * h > 2.0 * max_source_pixels)
        throw ImageTooLargeError();
    ImageBuf img;
    img.w = w;
    img.h = h;
    img.rgba = std::move(rgba);
    // EXIF 方向校正（等价 ImageOps.exif_transpose）
    img = apply_orientation(img, parse_orientation(bytes));
    img.has_alpha = scan_alpha(img.rgba);
    return img;
}

// ---------------------------------------------------------------- 缩放
// stb resize（LANCZOS3 近似 LANCZOS）；返回新缓冲，失败抛 ImageDecodeError
static ImageBuf stb_resize(const ImageBuf& src, int dw, int dh) {
    ImageBuf out;
    out.w = dw;
    out.h = dh;
    out.has_alpha = src.has_alpha;
    out.rgba.resize(size_t(dw) * dh * 4);
    if (!stbir_resize(static_cast<const void*>(src.rgba.data()), src.w, src.h, 0,
                      static_cast<void*>(out.rgba.data()), dw, dh, 0, STBIR_RGBA,
                      STBIR_TYPE_UINT8, STBIR_EDGE_CLAMP, STBIR_FILTER_MITCHELL))
        throw ImageDecodeError();
    return out;
}

ImageBuf resize_image(const ImageBuf& src, int max_w, int max_h) {
    // 只缩不放大（Python fit_box allow_upscale=false）
    if (src.w <= 0 || src.h <= 0) return src;
    double ratio = 1.0;
    if (max_w > 0) ratio = std::min(ratio, double(max_w) / src.w);
    if (max_h > 0) ratio = std::min(ratio, double(max_h) / src.h);
    if (ratio >= 1.0) return src;
    int w = std::max(1, int(lround(src.w * ratio)));
    int h = std::max(1, int(lround(src.h * ratio)));
    if (w == src.w && h == src.h) return src;
    return stb_resize(src, w, h);
}

ImageBuf resize_to(const ImageBuf& src, int w, int h) {
    // 强制尺寸
    if (w <= 0 || h <= 0 || (w == src.w && h == src.h)) return src;
    return stb_resize(src, w, h);
}

// ---------------------------------------------------------------- 编码
namespace {

// PNG 块：长度 + 类型 + 数据 + CRC32
void png_chunk(std::string& out, const char* type, const std::string& data) {
    auto be32 = [&](unsigned int v) {
        out.push_back(char((v >> 24) & 0xFF));
        out.push_back(char((v >> 16) & 0xFF));
        out.push_back(char((v >> 8) & 0xFF));
        out.push_back(char(v & 0xFF));
    };
    be32(static_cast<unsigned int>(data.size()));
    std::string body(type, 4);
    body += data;
    out += body;
    be32(static_cast<unsigned int>(mz_crc32(MZ_CRC32_INIT,
                                            reinterpret_cast<const mz_uint8*>(body.data()),
                                            body.size())));
}

// 单行滤波（PNG 规范 5 种），out 长度 = stride
void png_filter_row(const unsigned char* cur, const unsigned char* prev, int stride, int bpp,
                    int filter, std::string& out) {
    out.resize(static_cast<size_t>(stride));
    for (int i = 0; i < stride; ++i) {
        int a = i >= bpp ? cur[i - bpp] : 0;
        int b = prev ? prev[i] : 0;
        int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
        int x = cur[i];
        int v = 0;
        switch (filter) {
            case 0: v = x; break;
            case 1: v = x - a; break;
            case 2: v = x - b; break;
            case 3: v = x - ((a + b) >> 1); break;
            default: {
                int p = a + b - c;
                int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                int pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                v = x - pred;
                break;
            }
        }
        out[static_cast<size_t>(i)] = static_cast<unsigned char>(v & 0xFF);
    }
}

}  // namespace

std::string encode_png(const ImageBuf& img) {
    // 自适应行滤波 + miniz deflate(level 9)。
    // 早期实现直接用 stb_image_write：它的 zlib 是最朴素的实现（无惰性匹配），
    // 同一页 PNG 体积可达 Pillow(optimize=True) 的 5~7 倍 —— 漫画/条漫每页都要
    // 走局域网传输，这个差距是实打实的等待时间。滤波启发式与 libpng 一致
    // （取"有符号字节绝对值之和"最小的滤波器）。
    const int bpp = img.has_alpha ? 4 : 3;
    const int w = img.w, h = img.h;
    if (w <= 0 || h <= 0) return std::string();

    std::vector<unsigned char> raw;
    raw.resize(static_cast<size_t>(w) * h * bpp);
    if (img.has_alpha) {
        std::memcpy(raw.data(), img.rgba.data(), raw.size());
    } else {
        for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
            raw[i * 3 + 0] = img.rgba[i * 4 + 0];
            raw[i * 3 + 1] = img.rgba[i * 4 + 1];
            raw[i * 3 + 2] = img.rgba[i * 4 + 2];
        }
    }

    const int stride = w * bpp;
    std::string filtered;
    filtered.reserve(static_cast<size_t>(h) * (stride + 1));
    std::string best, trial;
    for (int y = 0; y < h; ++y) {
        const unsigned char* cur = raw.data() + static_cast<size_t>(y) * stride;
        const unsigned char* prev = y > 0 ? cur - stride : nullptr;
        int best_filter = 0;
        long long best_score = -1;
        for (int f = 0; f < 5; ++f) {
            png_filter_row(cur, prev, stride, bpp, f, trial);
            long long score = 0;
            for (unsigned char c : trial) {
                int s = c < 128 ? c : 256 - static_cast<int>(c);
                score += s;
            }
            if (best_score < 0 || score < best_score) {
                best_score = score;
                best_filter = f;
                best = trial;
            }
        }
        filtered.push_back(static_cast<char>(best_filter));
        filtered += best;
    }

    mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(filtered.size()));
    std::string zlib;
    zlib.resize(bound);
    mz_ulong zlen = bound;
    if (mz_compress2(reinterpret_cast<unsigned char*>(&zlib[0]), &zlen,
                     reinterpret_cast<const unsigned char*>(filtered.data()),
                     static_cast<mz_ulong>(filtered.size()), 9) != MZ_OK) {
        // 压缩失败 → 退化为 stb（保证一定有可用字节）
        std::string fallback;
        if (img.has_alpha)
            stbi_write_png_to_func(stb_write_cb, &fallback, w, h, 4, img.rgba.data(), 0);
        else {
            auto rgb = to_rgb_buffer(img);
            stbi_write_png_to_func(stb_write_cb, &fallback, w, h, 3, rgb.data(), 0);
        }
        return fallback;
    }
    zlib.resize(zlen);

    std::string out;
    static const char kSig[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
    out.append(kSig, 8);
    std::string ihdr;
    auto be32 = [&](unsigned int v) {
        ihdr.push_back(char((v >> 24) & 0xFF));
        ihdr.push_back(char((v >> 16) & 0xFF));
        ihdr.push_back(char((v >> 8) & 0xFF));
        ihdr.push_back(char(v & 0xFF));
    };
    be32(static_cast<unsigned int>(w));
    be32(static_cast<unsigned int>(h));
    ihdr.push_back(8);                                    // bit depth
    ihdr.push_back(img.has_alpha ? 6 : 2);                // color type RGBA / RGB
    ihdr.push_back(0);                                    // compression
    ihdr.push_back(0);                                    // filter
    ihdr.push_back(0);                                    // interlace
    png_chunk(out, "IHDR", ihdr);
    png_chunk(out, "IDAT", zlib);
    png_chunk(out, "IEND", std::string());
    return out;
}

std::string encode_jpeg(const ImageBuf& img, int quality) {
    // 与 encode_png 相同的守卫：w/h ≤0 时 to_rgb_buffer 的
    // size_t(w)*h*3 会变成天文数字（负 int → size_t 回绕）→ bad_alloc
    if (img.w <= 0 || img.h <= 0) return std::string();
    quality = std::clamp(quality, 1, 100);
    auto rgb = to_rgb_buffer(img);  // JPEG 无 alpha，透明合成到白底
    std::string out;
    stbi_write_jpg_to_func(stb_write_cb, &out, img.w, img.h, 3, rgb.data(), quality);
    return out;
}

ImageBytes encode_image(const ImageBuf& img, const EncodeOpts& opts) {
    // imaging.py:228 use_png = force_png or (has_alpha and not prefer_jpeg)。
    // 注意 prefer_jpeg=True 时**即使有 alpha 也走 JPEG** —— 早期实现写成
    // "has_alpha 就 PNG"，让带一像素半透明的照片页从 image/jpeg 变成 image/png，
    // Content-Type 与体积都与 Python 不同。
    const bool use_png = opts.force_png || (img.has_alpha && !opts.prefer_jpeg);
    ImageBytes res;
    if (use_png || !opts.prefer_jpeg) {
        res.bytes = encode_png(img);
        res.mime = "image/png";
    } else {
        res.bytes = encode_jpeg(img, opts.jpeg_quality);
        res.mime = "image/jpeg";
    }
    return res;
}

}  // namespace lnv
