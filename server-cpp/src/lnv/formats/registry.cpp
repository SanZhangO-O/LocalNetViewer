// 格式注册表实现（对应 Python formats/registry.py create_adapter）
#include "lnv/formats/registry.h"

#include "lnv/formats/comics.h"
#include "lnv/formats/container.h"
#include "lnv/formats/epub.h"
#include "lnv/formats/fb2.h"
#include "lnv/formats/txt.h"
#include "lnv/formats/pdf.h"
#include "lnv/formats/mobi.h"

// ---- 依赖说明 ----
// - 漫画/图集 / PDF：本文件就地组装（ComicAdapter / PdfAdapter）；
// - EPUB：create_epub_adapter（epub.h）——内含图片型（漫画）分流，固定版式铺满屏幕；
// - MOBI/AZW3：open_mobi_adapter（mobi.h）——先转换，再按是否漫画分流。
namespace lnv {
std::unique_ptr<BookAdapter> create_comic_adapter(const AppConfig& cfg, const BookRef& ref,
                                                  const AdapterEnv&) {
    return std::make_unique<ComicAdapter>(cfg, ref);
}
std::unique_ptr<BookAdapter> create_pdf_adapter(const AppConfig& cfg, const BookRef& ref,
                                                const AdapterEnv&) {
    return std::make_unique<PdfAdapter>(cfg, ref);
}
}  // namespace lnv

namespace lnv {

std::unique_ptr<BookAdapter> create_adapter(const AppConfig& cfg, const BookRef& ref,
                                            const AdapterEnv& env) {
    // 虚拟书：文件夹图集
    if (ref.virtual_book && ref.format == "gallery") {
        return create_comic_adapter(cfg, ref, env);
    }

    std::string fmt = ref.format;
    for (char& c : fmt)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');

    // 未知格式先拒绝
    bool known = fmt == "txt" || fmt == "epub" || fmt == "fb2" || fmt == "pdf" ||
                 fmt == "cbz" || fmt == "cbz3" || fmt == "zip" || fmt == "cbr" ||
                 fmt == "rar" || fmt == "cb7" || fmt == "7z" || fmt == "cbt" ||
                 fmt == "tar" || fmt == "mobi" || fmt == "azw3" || fmt == "azw";

    // 顺序与 Python registry.py 一致：**先查大小上限**再判格式。
    // 反过来的话，超限的未知扩展名文件 Python 报 422、C++ 报 415。
    long long max_mb = cfg.limits.max_book_mb;
    if (max_mb > 0 && ref.size > max_mb * 1024 * 1024) {
        // 文案逐字对齐 registry.py:183-186（含实际大小，便于用户判断）
        char buf[128];
        std::snprintf(buf, sizeof(buf), "文件超过单本上限 %lldMB（%.1fMB），已拒绝", max_mb,
                      double(ref.size) / 1048576.0);
        throw ParseError(buf);
    }

    if (!known) {
        std::string ext = ext_of(ref.name());
        // 文案与 reason 对齐 registry.py:175-177
        // （reason = f"不支持的文件类型 {ext or ''}".strip()）
        throw UnsupportedFormatError("不支持的格式: " + (ext.empty() ? ref.name() : ext),
                                     "不支持的文件类型" + (ext.empty() ? "" : " " + ext));
    }

    if (fmt == "txt") return std::make_unique<TxtAdapter>(cfg, ref);
    // EPUB：图片型（漫画）→ 固定版式，文本型 → 流式（create_epub_adapter 内部检测）
    if (fmt == "epub") return create_epub_adapter(cfg, ref, env);
    if (fmt == "fb2") return std::make_unique<Fb2Adapter>(cfg, ref);
    if (fmt == "pdf") return create_pdf_adapter(cfg, ref, env);
    // MOBI/AZW3：转换后按是否漫画分流（open_mobi_adapter）
    if (fmt == "mobi" || fmt == "azw3" || fmt == "azw") return open_mobi_adapter(cfg, ref, env);
    // cbz/cbz3/zip、cbr/rar、cb7/7z、cbt/tar → 漫画容器适配器
    return create_comic_adapter(cfg, ref, env);
}

std::optional<std::string> openable_reason(const std::string& fmt_in) {
    std::string fmt = fmt_in;
    for (char& c : fmt)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if ((fmt == "cbr" || fmt == "rar") && !rar_backend_available())
        return std::string("需安装 unrar");
    if ((fmt == "cb7" || fmt == "7z") && !seven_backend_available())
        return std::string("需安装 7-Zip");
    if ((fmt == "mobi" || fmt == "azw3" || fmt == "azw") && !calibre_available())
        return std::string("需安装 Calibre（MOBI/AZW3）");
    if (fmt == "pdf" && !pdfium_available()) return std::string("需放置 pdfium.dll");
    return std::nullopt;
}

}  // namespace lnv
