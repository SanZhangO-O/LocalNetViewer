// 漫画 / 图集适配器实现（语义对齐 Python formats/comics.py ComicAdapter）
#include "lnv/formats/comics.h"
#include "lnv/util/imaging.h"

#include <algorithm>
#include <cstring>

namespace lnv {

// ---------------------------------------------------------- 本地图片工具

// 只缩不放大到目标宽（对应 imaging.fit_width）
static ImageBuf fit_width(const ImageBuf& src, int target_w) {
    if (src.w <= target_w || target_w <= 0) return src;
    return resize_image(src, target_w, 0);
}

// 横向拼接（白底、各图垂直居中），对应 imaging.hstack
static ImageBuf hstack_imgs(const std::vector<ImageBuf>& imgs) {
    ImageBuf out;
    long long w = 0;
    int h = 0;
    for (const auto& im : imgs) { w += im.w; h = std::max(h, im.h); }
    out.w = (int)w;
    out.h = h;
    out.rgba.assign((size_t)w * h * 4, 255);  // 白底
    out.has_alpha = false;
    long long xoff = 0;
    for (const auto& im : imgs) {
        int yoff = (h - im.h) / 2;
        if (yoff < 0) yoff = 0;
        for (int y = 0; y < im.h; ++y) {
            auto* dst = out.rgba.data() + ((size_t)(y + yoff) * w + xoff) * 4;
            const auto* src = im.rgba.data() + (size_t)y * im.w * 4;
            std::memcpy(dst, src, (size_t)im.w * 4);
        }
        xoff += im.w;
    }
    return out;
}

// 裁剪纵向切片 [y0, y0+height)，对应 imaging.crop_slice：top 先夹到 h-1、
// bottom 至少 top+1。probe 声明高度与实际解码高度不一致（截断图）时也保证
// 非空切片，不会产出 0 高图片（Python crop_slice 同样夹取，审查 M8）。
static ImageBuf crop_slice(const ImageBuf& src, long long y0, long long height) {
    long long h = src.h;
    long long top = std::max<long long>(0, std::min<long long>(y0, std::max<long long>(0, h - 1)));
    long long bottom = std::min<long long>(h, top + std::max<long long>(1, height));
    ImageBuf out;
    out.w = src.w;
    out.h = (int)std::max<long long>(0, bottom - top);
    out.has_alpha = src.has_alpha;
    out.rgba.resize((size_t)out.w * out.h * 4);
    for (int y = 0; y < out.h; ++y) {
        const auto* s = src.rgba.data() + ((size_t)(top + y) * src.w) * 4;
        auto* d = out.rgba.data() + (size_t)y * out.w * 4;
        std::memcpy(d, s, (size_t)out.w * 4);
    }
    return out;
}

// ---------------------------------------------------------- 构造 / 页集

ComicAdapter::ComicAdapter(const AppConfig& cfg, const BookRef& ref)
    : cfg_(cfg), ref_(ref) {
    title_ = stem_of(ref.name());

    std::error_code ec;
    std::string ext = ext_of(ref.name());
    // 图集递归标志来自 BookRef.extra（services 在注册/索引时写入，Python 同名字段）
    bool recursive = false;
    if (auto it = ref.extra.find("recursive"); it != ref.extra.end() && it->second.is_boolean())
        recursive = it->second.get<bool>();
    if (std::filesystem::is_directory(ref.path, ec)) {
        // 目录容器：recursive 必须透传，否则 page_count（按 recursive 统计）与
        // 实际可取的页数不一致 —— 客户端拿到的 total_pages 会小于声称的页数
        container_ = open_dir(ref.path, recursive, cfg_.comic.skip_entries);
    } else if (std::filesystem::is_regular_file(ref.path, ec) && is_image_ext(ext)) {
        // 单张图片 = 单页图集：以父目录为根，仅此文件
        container_ = open_dir(ref.path.parent_path(), false, cfg_.comic.skip_entries);
        single_ = path_u8_impl(ref.path.filename());
    } else if (ext == ".cbz" || ext == ".zip") {
        container_ = open_zip(ref.path);
    } else if (ext == ".cbr" || ext == ".rar") {
        container_ = open_rar(ref.path);
    } else if (ext == ".cb7" || ext == ".7z") {
        container_ = open_7z(ref.path);
    } else if (ext == ".cbt" || ext == ".tar") {
        container_ = open_tar(ref.path);
    } else {
        throw UnsupportedFormatError("不支持的漫画容器: " + (ext.empty() ? ref.name() : ext),
                                     "未知容器类型");
    }
    init_common();
}

// 外部容器（图片型 EPUB 分流）：容器接管所有权，title 由调用方给出
ComicAdapter::ComicAdapter(const AppConfig& cfg, const BookRef& ref,
                           std::unique_ptr<Container> container, std::string title)
    : cfg_(cfg), ref_(ref) {
    title_ = std::move(title);
    container_ = std::move(container);
    preserve_order_ = true;  // 页序 = spine 顺序（见 comics.h preserve_order_）
    init_common();
}

void ComicAdapter::init_common() {
    // 真实格式：ref.format 优先，否则扩展名，兜底 gallery
    format = ref_.format.empty()
                 ? (ext_of(ref_.name()).size() > 1 ? ext_of(ref_.name()).substr(1) : "gallery")
                 : ref_.format;
    for (auto& c : format) c = (char)std::tolower((unsigned char)c);
    mode = "fixed";
    max_page_bytes_ = cfg_.limits.max_page_bytes;      // 0 = 不限
    max_source_pixels_ = cfg_.limits.max_source_pixels;
    jpeg_quality_ = cfg_.comic.jpeg_quality;
    strip_min_aspect_ = cfg_.comic.strip_min_aspect;

    // RAR/7z 按需单条目解压：声明尺寸可能不可信，把总量上限交给容器读取路径
    // 累计校验（超过即杀子进程并抛错，审查 M3）
    container_->set_total_limit(cfg_.limits.max_archive_total_mb * 1024 * 1024);
    build_names();
}

// fs::path → UTF-8（避免头文件里再引 windows.h）
std::string ComicAdapter::path_u8_impl(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

void ComicAdapter::build_names() {
    const auto& entries = container_->entries();
    long long total = 0;
    long long max_total = cfg_.limits.max_archive_total_mb * 1024 * 1024;  // 0 = 不限
    std::vector<std::string> out;
    for (const auto& e : entries) {
        const std::string& name = e.name;  // 容器层已规范化（normpath；ZIP 额外去重）
        if (should_skip_entry(name, cfg_.comic.skip_entries)) continue;
        if (!is_image_ext(ext_of(name))) continue;
        if (!single_.empty() && name != single_) continue;
        if (is_unsafe_entry_name(name)) continue;
        out.push_back(name);
        total += (long long)e.size;
    }
    if (max_total > 0 && total > max_total)
        throw ParseError(archive_total_limit_error(total));
    // 目录容器的条目已自然排序（list_dir_image_names），避免每请求对上万张图重排
    if (!preserve_order_ && !container_->entries_presorted()) {
        std::sort(out.begin(), out.end(), natural_key_less);  // 自然排序页序（§4.9）
    }
    names_ = std::move(out);
}

long long ComicAdapter::entry_limit() const {
    return cfg_.limits.max_archive_entry_mb * 1024 * 1024;  // 0 = 不限
}

const std::string& ComicAdapter::entry(int page) const {
    if (page < 1 || page > (int)names_.size())
        throw UnsupportedFormatError("页码越界: " + std::to_string(page));
    return names_[page - 1];
}

std::pair<std::string, std::string> ComicAdapter::raw_bytes(int page) {
    const std::string& name = entry(page);
    std::string data = container_->read(name, entry_limit());
    auto it = mime_cache_.find(name);
    // 魔数认不出来时退回条目扩展名（Python comics.py:685 sniff_mime(data, ext_of(name))）
    std::string mime = it != mime_cache_.end() ? it->second : sniff_mime(data, ext_of(name));
    mime_cache_[name] = mime;
    return {std::move(data), mime};
}

int ComicAdapter::page_count() { return (int)names_.size(); }

AdapterMeta ComicAdapter::meta() {
    AdapterMeta m;
    m.title = title_;
    m.format = format;
    m.mode = "fixed";
    m.size = ref_.size;
    m.page_count = page_count();
    m.supports_json = false;
    return m;
}

std::optional<std::pair<int, int>> ComicAdapter::native_size_for_name(const std::string& name) {
    try {
        std::string data = container_->read(name, entry_limit());
        auto sz = probe_size(data);
        if (sz.first <= 0) return std::nullopt;
        return sz;
    } catch (...) {
        return std::nullopt;
    }
}

std::pair<int, int> ComicAdapter::page_native_size(int page) {
    const std::string& name = entry(page);
    std::string data = container_->read(name, entry_limit());
    auto sz = probe_size(data);
    if (sz.first <= 0) throw ParseError("图片无法识别: " + name);
    return sz;
}

// ---------------------------------------------------------- 取页

ImageBytes ComicAdapter::page_bytes(int page, const std::string& fit, int quality,
                                    int viewport_w, double render_scale) {
    const std::string& name = entry(page);
    auto [data, mime] = raw_bytes(page);

    if (fit == "original") {
        if (max_page_bytes_ <= 0 || (long long)data.size() <= max_page_bytes_)
            return {std::move(data), mime};  // 原图直通
        quality = std::max(quality, jpeg_quality_);  // 超限强制缩放，保底质量
    }

    // fit=viewport / 强制缩放共用：目标宽 = 视口 × clamp(0.5..2.0, scale)
    double sc = std::max(0.5, std::min(2.0, render_scale));
    int target_w = std::max(1, (int)std::llround(viewport_w * sc));
    if (fit == "original") {
        // 原图模式保持原图分辨率优先，只做编码转 JPEG
        auto native = probe_size(data);
        if (native.first > target_w) target_w = native.first;
    }

    ImageBuf img;
    try {
        img = decode_image(data, max_source_pixels_);
    } catch (const ParseError& e) {
        throw ParseError(std::string(e.what()) + ": " + name);
    }
    if (img.w > target_w) img = fit_width(img, target_w);  // 只缩小不放大

    // 原 mime 非 PNG 或无透明 → JPEG，否则 PNG（保住透明）
    bool prefer_jpeg = mime != "image/png" || !img.has_alpha;
    EncodeOpts opts;
    opts.prefer_jpeg = prefer_jpeg;
    opts.jpeg_quality = quality > 0 ? quality : jpeg_quality_;
    return encode_image(img, opts);
}

std::optional<ImageBytes> ComicAdapter::cover(int width) {
    if (page_count() < 1) return std::nullopt;
    int target = width > 0 ? width : 320;
    target = std::max(80, std::min(1024, target));
    try {
        // Python comics.py:719：封面质量固定 80（不是配置里的 jpeg_quality）
        return page_bytes(1, "viewport", 80, target, 1.0);
    } catch (...) {
        return std::nullopt;  // 首页读不出 → 无封面，由上层生成文字封面
    }
}

ImageBytes ComicAdapter::pair_bytes(int page, int quality, int viewport_w,
                                    double render_scale, bool rtl) {
    int n = page_count();
    if (n < 1) throw ParseError("图集为空");
    int first = std::max(1, std::min(page, n));
    int target_w = std::max(1, (int)std::llround(viewport_w * render_scale));
    std::vector<ImageBuf> imgs;
    for (int p : {first, first + 1}) {
        if (p > n) continue;  // 末页无右页 → 单页返回
        auto [data, mime] = raw_bytes(p);
        (void)mime;
        ImageBuf img;
        try {
            img = decode_image(data, max_source_pixels_);
        } catch (const ParseError& e) {
            throw ParseError(std::string(e.what()));
        }
        if (img.w > target_w) img = fit_width(img, target_w);
        imgs.push_back(std::move(img));
    }
    if (rtl) std::reverse(imgs.begin(), imgs.end());
    ImageBuf merged = hstack_imgs(imgs);
    EncodeOpts opts;
    opts.prefer_jpeg = true;
    opts.jpeg_quality = quality > 0 ? quality : jpeg_quality_;
    return encode_image(merged, opts);
}

// ---------------------------------------------------------- 条漫切片

std::vector<StripPage> ComicAdapter::strip_pages(int viewport_h, int overlap) {
    int vh = std::max(64, viewport_h);
    // 重叠不能逼近视口高度，否则长图会切出海量 1px 页
    int overlap_eff = std::max(0, std::min(overlap, vh / 2));
    int step = std::max(1, vh - overlap_eff);
    std::vector<StripPage> slices;
    for (int idx = 0; idx < (int)names_.size(); ++idx) {
        auto size = native_size_for_name(names_[idx]);
        if (!size) {
            // 无法读取尺寸的图片按单页处理
            slices.push_back({idx, 0, vh, true, true});
            continue;
        }
        long long h = size->second;
        long long y = 0;
        while (y < h) {
            long long height = std::min<long long>(vh, h - y);
            slices.push_back({idx, y, height, y == 0, y + height >= h});
            if (y + height >= h) break;
            y += step;
        }
    }
    return slices;
}

ImageBytes ComicAdapter::strip_slice_bytes(const StripPage& sp, int quality, int /*viewport_w*/,
                                           double /*render_scale*/, const std::string& /*fit*/) {
    if (sp.image_index < 0 || sp.image_index >= (int)names_.size())
        throw UnsupportedFormatError("切片越界: image_index=" + std::to_string(sp.image_index));
    const std::string& name = names_[sp.image_index];
    std::string data = container_->read(name, entry_limit());
    auto it = mime_cache_.find(name);
    std::string mime = it != mime_cache_.end() ? it->second : sniff_mime(data, ext_of(name));
    mime_cache_[name] = mime;
    ImageBuf img;
    try {
        img = decode_image(data, max_source_pixels_);  // 整图解码，不缩放（缩放 0 语义）
    } catch (const ParseError&) {
        throw;
    }
    // 解码结果为空图（畸形头）时直接按解码失败处理；否则 crop_slice 之后可能
    // 拿到 0 高图片编出空体（200）。Python 侧这里走 ImageDecodeError → ParseError
    if (img.w <= 0 || img.h <= 0) throw ParseError("长图解码失败: " + name);
    ImageBuf piece = crop_slice(img, sp.y0, sp.height);
    // PNG/GIF 源保持无损 PNG，其余 JPEG
    if (mime == "image/png" || mime == "image/gif") return {encode_png(piece), "image/png"};
    EncodeOpts opts;
    opts.prefer_jpeg = true;
    opts.jpeg_quality = quality > 0 ? quality : jpeg_quality_;
    return encode_image(piece, opts);
}

}  // namespace lnv
