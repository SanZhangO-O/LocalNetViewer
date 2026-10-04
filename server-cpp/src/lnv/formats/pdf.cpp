// PDF 适配器实现：动态加载 pdfium.dll，FPDF_* 原型手写 typedef
// 语义对齐 Python formats/pdf.py（pypdfium2 直渲、fixed、恒 JPEG）
#include "lnv/formats/pdf.h"
#include "lnv/util/imaging.h"
#include "lnv/util/paths.h"

#include <mutex>
#include <cmath>
#include <cstring>

#include <windows.h>

namespace lnv {

namespace {

// ---------------------------------------------------------- pdfium 动态加载

// PDFium C API 原型（手写 typedef，避免引入 pdfium 头）
using FPDF_DOCUMENT = void*;
using FPDF_PAGE = void*;
using FPDF_BITMAP = void*;
using FPDF_BYTESTRING = const char*;
using FPDF_WCHAR = unsigned short;
using FPDF_STRLEN = unsigned long;

using Fn_InitLibrary = void (*)();
using Fn_LoadDocument = FPDF_DOCUMENT (*)(FPDF_BYTESTRING file_path, FPDF_BYTESTRING password);
using Fn_CloseDocument = void (*)(FPDF_DOCUMENT);
using Fn_GetPageCount = int (*)(FPDF_DOCUMENT);
using Fn_LoadPage = FPDF_PAGE (*)(FPDF_DOCUMENT, int);
using Fn_ClosePage = void (*)(FPDF_PAGE);
using Fn_GetPageWidthF = float (*)(FPDF_PAGE);
using Fn_GetPageHeightF = float (*)(FPDF_PAGE);
using Fn_RenderPageBitmap = void (*)(FPDF_BITMAP, FPDF_PAGE, int start_x, int start_y,
                                     int size_x, int size_y, int rotate, int flags);
using Fn_Bitmap_Create = FPDF_BITMAP (*)(int width, int height, int alpha);
using Fn_Bitmap_Destroy = void (*)(FPDF_BITMAP);
using Fn_Bitmap_FillRect = void (*)(FPDF_BITMAP, int left, int top, int width, int height,
                                    unsigned long color);
using Fn_Bitmap_GetBuffer = void* (*)(FPDF_BITMAP);
using Fn_Bitmap_GetStride = int (*)(FPDF_BITMAP);
using Fn_GetMetaText = FPDF_STRLEN (*)(FPDF_DOCUMENT, FPDF_BYTESTRING tag,
                                       void* buffer, FPDF_STRLEN buflen);
using Fn_GetLastError = unsigned long (*)();

struct PdfiumApi {
    bool ok = false;
    HMODULE mod = nullptr;
    Fn_InitLibrary InitLibrary = nullptr;
    Fn_LoadDocument LoadDocument = nullptr;
    Fn_CloseDocument CloseDocument = nullptr;
    Fn_GetPageCount GetPageCount = nullptr;
    Fn_LoadPage LoadPage = nullptr;
    Fn_ClosePage ClosePage = nullptr;
    Fn_GetPageWidthF GetPageWidthF = nullptr;
    Fn_GetPageHeightF GetPageHeightF = nullptr;
    Fn_RenderPageBitmap RenderPageBitmap = nullptr;
    Fn_Bitmap_Create Bitmap_Create = nullptr;
    Fn_Bitmap_Destroy Bitmap_Destroy = nullptr;
    Fn_Bitmap_FillRect Bitmap_FillRect = nullptr;
    Fn_Bitmap_GetBuffer Bitmap_GetBuffer = nullptr;
    Fn_Bitmap_GetStride Bitmap_GetStride = nullptr;
    Fn_GetMetaText GetMetaText = nullptr;
    Fn_GetLastError GetLastError = nullptr;
};

template <typename T>
static T get_fn(HMODULE m, const char* name) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

const PdfiumApi& pdfium_api() {
    static PdfiumApi api = [] {
        PdfiumApi a;
        // LoadLibrary 搜索序：exe 目录 → 系统目录 → PATH；再显式补试 exe 目录
        HMODULE m = LoadLibraryW(L"pdfium.dll");
        if (!m) {
            wchar_t exe[MAX_PATH];
            if (GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0) {
                std::filesystem::path p(exe);
                SetLastError(0);
                m = LoadLibraryW((p.parent_path() / L"pdfium.dll").c_str());
            }
        }
        if (!m) return a;
        a.mod = m;
        a.InitLibrary = get_fn<Fn_InitLibrary>(m, "FPDF_InitLibrary");
        a.LoadDocument = get_fn<Fn_LoadDocument>(m, "FPDF_LoadDocument");
        a.CloseDocument = get_fn<Fn_CloseDocument>(m, "FPDF_CloseDocument");
        a.GetPageCount = get_fn<Fn_GetPageCount>(m, "FPDF_GetPageCount");
        a.LoadPage = get_fn<Fn_LoadPage>(m, "FPDF_LoadPage");
        a.ClosePage = get_fn<Fn_ClosePage>(m, "FPDF_ClosePage");
        a.GetPageWidthF = get_fn<Fn_GetPageWidthF>(m, "FPDF_GetPageWidthF");
        a.GetPageHeightF = get_fn<Fn_GetPageHeightF>(m, "FPDF_GetPageHeightF");
        a.RenderPageBitmap = get_fn<Fn_RenderPageBitmap>(m, "FPDF_RenderPageBitmap");
        a.Bitmap_Create = get_fn<Fn_Bitmap_Create>(m, "FPDFBitmap_Create");
        a.Bitmap_Destroy = get_fn<Fn_Bitmap_Destroy>(m, "FPDFBitmap_Destroy");
        a.Bitmap_FillRect = get_fn<Fn_Bitmap_FillRect>(m, "FPDFBitmap_FillRect");
        a.Bitmap_GetBuffer = get_fn<Fn_Bitmap_GetBuffer>(m, "FPDFBitmap_GetBuffer");
        a.Bitmap_GetStride = get_fn<Fn_Bitmap_GetStride>(m, "FPDFBitmap_GetStride");
        a.GetMetaText = get_fn<Fn_GetMetaText>(m, "FPDF_GetMetaText");
        a.GetLastError = get_fn<Fn_GetLastError>(m, "FPDF_GetLastError");
        a.ok = a.LoadDocument && a.CloseDocument && a.GetPageCount && a.LoadPage &&
               a.GetPageWidthF && a.GetPageHeightF && a.RenderPageBitmap &&
               a.Bitmap_Create && a.Bitmap_Destroy && a.Bitmap_FillRect &&
               a.Bitmap_GetBuffer && a.Bitmap_GetStride;
        if (a.ok && a.InitLibrary) a.InitLibrary();
        return a;
    }();
    return api;
}

[[noreturn]] void throw_no_pdfium() {
    throw UnsupportedFormatError("缺少 PDF 后端", "需放置 pdfium.dll");
}

void require_pdfium() {
    if (!pdfium_api().ok) throw_no_pdfium();
}

// FPDF_ERR_* → pypdfium2 的 ErrorToStr 文案（Python 侧 PdfiumError 消息逐字来源）
std::string pdfium_error_str(unsigned long err) {
    switch (err) {
        case 0: return "Success";
        case 2: return "File access error";
        case 3: return "Data format error";
        case 4: return "Incorrect password error";  // 口令 PDF 的区分点
        case 5: return "Unsupported security scheme error";
        case 6: return "Page not found or content error";
        default: return "Unknown error";
    }
}

// FPDF_PAGE / FPDF_BITMAP 的 RAII 守卫：encode_jpeg 抛出时也保证释放
struct PageGuard {
    const PdfiumApi& api;
    FPDF_PAGE pg;
    ~PageGuard() { if (pg && api.ClosePage) api.ClosePage(pg); }
};
struct BitmapGuard {
    const PdfiumApi& api;
    FPDF_BITMAP bmp;
    ~BitmapGuard() { if (bmp && api.Bitmap_Destroy) api.Bitmap_Destroy(bmp); }
};

}  // namespace

// 供 /files 的 openable 探测用（Python registry.openable_reason）
bool pdfium_available() { return pdfium_api().ok; }

namespace {

// UTF-16LE → UTF-8（含代理对组合）
std::string utf16_to_utf8(const std::u16string& s) {
    U32Text t;
    for (size_t i = 0; i < s.size(); ++i) {
        char32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() &&
            s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            continue;  // 孤立代理：丢弃
        }
        t.push_back(c);
    }
    return u32_to_utf8(t);
}

}  // namespace

// ---------------------------------------------------------- 构造 / 生命周期

static constexpr double kRenderPtToPx = 2.0;  // fit=original 基准倍率（约 144dpi）

PdfAdapter::PdfAdapter(const AppConfig& cfg, const BookRef& ref)
    : cfg_(cfg), ref_(ref) {
    format = "pdf";
    mode = "fixed";
    max_pixels_ = cfg.render.max_page_pixels;
    require_pdfium();
    const auto& api = pdfium_api();
    // FPDF_LoadDocument 与 pypdfium2 一样要求 UTF-8 路径（path::string() 非 ACP 时给错编码）
    std::string path_u8 = path_to_utf8(ref.path);
    doc_ = api.LoadDocument(path_u8.c_str(), nullptr);
    page_count_ = doc_ ? api.GetPageCount(doc_) : 0;
    if (!doc_ || page_count_ < 1) {
        // pypdfium2 的等义行为：加载失败/空文档 → PdfiumError("Failed to load document
        // (PDFium: <原因>).")；口令 PDF 报 FPDF_ERR_PASSWORD(4) → "Incorrect password
        // error"，Python 的 422 消息就是据此区分口令的
        unsigned long err = api.GetLastError ? api.GetLastError() : 1;
        if (doc_) { api.CloseDocument(doc_); doc_ = nullptr; }
        throw ParseError("PDF 无法打开: Failed to load document (PDFium: " +
                         pdfium_error_str(err) + ").");
    }
}

PdfAdapter::~PdfAdapter() {
    if (!doc_) return;
    const auto& api = pdfium_api();
    if (api.CloseDocument) api.CloseDocument(doc_);
    doc_ = nullptr;
}

PdfAdapter::PtSize PdfAdapter::page_pt(int page) {
    if (page < 1 || page > page_count_)
        throw UnsupportedFormatError("页码越界: " + std::to_string(page));
    const auto& api = pdfium_api();
    FPDF_PAGE pg = api.LoadPage(doc_, page - 1);
    if (!pg) throw ParseError("PDF 第 " + std::to_string(page) + " 页无法读取");
    PageGuard guard{api, pg};
    PtSize sz;
    sz.w = api.GetPageWidthF(pg);
    sz.h = api.GetPageHeightF(pg);
    if (sz.w <= 0) sz.w = 1.0;
    if (sz.h <= 0) sz.h = 1.0;
    return sz;
}

int PdfAdapter::page_count() { return page_count_; }

std::pair<int, int> PdfAdapter::page_native_size(int page) {
    auto it = size_cache_.find(page);
    if (it != size_cache_.end()) return it->second;
    PtSize pt = page_pt(page);  // 越界在此抛错
    auto sz = std::make_pair(std::max(1, (int)std::lround(pt.w * kRenderPtToPx)),
                             std::max(1, (int)std::lround(pt.h * kRenderPtToPx)));
    size_cache_[page] = sz;
    return sz;
}

double PdfAdapter::page_aspect(int page) {
    try {
        PtSize pt = page_pt(page);
        return pt.w > 0 ? pt.h / pt.w : 0.0;
    } catch (...) {
        return 0.0;
    }
}

// ---------------------------------------------------------- 渲染

double PdfAdapter::clamp_scale(double w_pt, double h_pt, double scale) {
    if (max_pixels_ <= 0 || w_pt <= 0 || h_pt <= 0) return scale;
    double pixels = (w_pt * scale) * (h_pt * scale);
    if (pixels <= (double)max_pixels_) return scale;
    return scale * std::sqrt((double)max_pixels_ / pixels);
}

ImageBytes PdfAdapter::render_loaded_page(FPDF_PAGE pg, double w_pt, double h_pt,
                                          double scale, int quality) {
    const auto& api = pdfium_api();
    int w = std::max(1, (int)std::lround(w_pt * scale));
    int h = std::max(1, (int)std::lround(h_pt * scale));
    FPDF_BITMAP bmp = api.Bitmap_Create(w, h, 0);
    if (!bmp) throw ParseError("PDF 渲染位图创建失败");
    BitmapGuard guard{api, bmp};  // encode_jpeg 抛出时也释放位图
    api.Bitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFF);  // 白底（PDFium 渲染自带底色，防御）
    api.RenderPageBitmap(bmp, pg, 0, 0, w, h, 0, 0);
    ImageBuf img;
    img.w = w;
    img.h = h;
    img.rgba.resize((size_t)w * h * 4);
    const uint8_t* src = (const uint8_t*)api.Bitmap_GetBuffer(bmp);
    int stride = api.Bitmap_GetStride(bmp);
    if (src) {
        // BGRA → RGBA
        for (int y = 0; y < h; ++y) {
            const uint8_t* row = src + (size_t)y * stride;
            auto* dst = img.rgba.data() + (size_t)y * w * 4;
            for (int x = 0; x < w; ++x) {
                dst[x * 4 + 0] = row[x * 4 + 2];
                dst[x * 4 + 1] = row[x * 4 + 1];
                dst[x * 4 + 2] = row[x * 4 + 0];
                dst[x * 4 + 3] = 255;
            }
        }
    }
    img.has_alpha = false;
    return {encode_jpeg(img, quality > 0 ? quality : 85), "image/jpeg"};
}

ImageBytes PdfAdapter::page_bytes(int page, const std::string& fit, int quality,
                                  int viewport_w, double render_scale) {
    require_pdfium();
    if (page < 1 || page > page_count_)
        throw UnsupportedFormatError("页码越界: " + std::to_string(page));
    const auto& api = pdfium_api();
    FPDF_PAGE pg = api.LoadPage(doc_, page - 1);
    if (!pg) throw ParseError("PDF 第 " + std::to_string(page) + " 页无法读取");
    PageGuard guard{api, pg};
    // 单次 LoadPage：原实现 page_pt + render 内各再 LoadPage 一次（共 3 次）
    double w_pt = api.GetPageWidthF(pg);
    double h_pt = api.GetPageHeightF(pg);
    if (w_pt <= 0) w_pt = 1.0;
    if (h_pt <= 0) h_pt = 1.0;
    double sc;
    if (fit == "original")
        sc = kRenderPtToPx * std::max(0.5, std::min(2.0, render_scale));
    else
        sc = (std::max(1, viewport_w) * std::max(0.5, std::min(2.0, render_scale))) / w_pt;
    sc = clamp_scale(w_pt, h_pt, std::max(0.05, sc));
    return render_loaded_page(pg, w_pt, h_pt, sc, quality);
}

std::optional<ImageBytes> PdfAdapter::cover(int width) {
    if (page_count_ < 1) return std::nullopt;
    try {
        return page_bytes(1, "viewport", 82, width > 0 ? width : 480, 1.0);
    } catch (...) {
        return std::nullopt;
    }
}

ImageBytes PdfAdapter::pair_bytes(int page, int quality, int viewport_w,
                                  double render_scale, bool rtl) {
    require_pdfium();
    if (page + 1 > page_count_)
        return page_bytes(page, "viewport", quality, viewport_w, render_scale);  // 末页落单
    std::vector<ImageBuf> imgs;
    const auto& api = pdfium_api();
    for (int p : {page, page + 1}) {
        FPDF_PAGE pg = api.LoadPage(doc_, p - 1);
        if (!pg) throw ParseError("PDF 第 " + std::to_string(p) + " 页无法读取");
        PageGuard guard{api, pg};
        double w_pt = api.GetPageWidthF(pg);
        double h_pt = api.GetPageHeightF(pg);
        if (w_pt <= 0) w_pt = 1.0;
        if (h_pt <= 0) h_pt = 1.0;
        // Python pair 不做 0.5..2 clamp，只做像素上限钳制
        double sc = clamp_scale(w_pt, h_pt, (viewport_w * render_scale) / w_pt);
        sc = std::max(0.05, sc);
        auto bytes = render_loaded_page(pg, w_pt, h_pt, sc, quality > 0 ? quality : 85);
        ImageBuf im = decode_image(bytes.bytes, 0);
        imgs.push_back(std::move(im));
    }
    if (rtl) std::reverse(imgs.begin(), imgs.end());
    // 横向拼接（白底垂直居中）
    long long w = 0;
    int h = 0;
    for (auto& im : imgs) { w += im.w; h = std::max(h, im.h); }
    ImageBuf out;
    out.w = (int)w;
    out.h = h;
    out.rgba.assign((size_t)w * h * 4, 255);
    long long xoff = 0;
    for (auto& im : imgs) {
        int yoff = std::max(0, (h - im.h) / 2);
        for (int y = 0; y < im.h; ++y)
            std::memcpy(out.rgba.data() + ((size_t)(y + yoff) * w + xoff) * 4,
                        im.rgba.data() + (size_t)y * im.w * 4, (size_t)im.w * 4);
        xoff += im.w;
    }
    return {encode_jpeg(out, quality > 0 ? quality : 85), "image/jpeg"};
}

// ---------------------------------------------------------- meta

std::string PdfAdapter::meta_text(const char* tag) {
    const auto& api = pdfium_api();
    if (!api.GetMetaText) return {};
    FPDF_STRLEN need = api.GetMetaText(doc_, tag, nullptr, 0);
    if (need < 2) return {};
    std::u16string buf((size_t)need / 2, u'\0');
    api.GetMetaText(doc_, tag, buf.data(), need);
    if (!buf.empty() && buf.back() == u'\0') buf.pop_back();
    return utf16_to_utf8(buf);
}

AdapterMeta PdfAdapter::meta() {
    require_pdfium();
    AdapterMeta m;
    std::string title = meta_text("Title");
    // 去 BOM + strip
    if (title.rfind("\xEF\xBB\xBF", 0) == 0) title.erase(0, 3);
    while (!title.empty() && (title.front() == ' ' || title.front() == '\t')) title.erase(title.begin());
    while (!title.empty() && (title.back() == ' ' || title.back() == '\t')) title.pop_back();
    m.title = !title.empty() ? title : stem_of(ref_.name());
    std::string author = meta_text("Author");
    if (author.rfind("\xEF\xBB\xBF", 0) == 0) author.erase(0, 3);
    while (!author.empty() && (author.front() == ' ' || author.front() == '\t')) author.erase(author.begin());
    while (!author.empty() && (author.back() == ' ' || author.back() == '\t')) author.pop_back();
    if (!author.empty()) m.author = author;
    m.format = format;
    m.mode = "fixed";
    m.size = ref_.size;
    m.page_count = page_count_;
    m.supports_json = false;
    return m;
}

}  // namespace lnv
