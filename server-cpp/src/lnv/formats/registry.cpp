// 格式注册表实现（对应 Python formats/registry.py create_adapter）
#include "lnv/formats/registry.h"

#include "lnv/formats/comics.h"
#include "lnv/formats/container.h"
#include "lnv/formats/epub.h"
#include "lnv/formats/fb2.h"
#include "lnv/formats/txt.h"
#include "lnv/formats/pdf.h"
#include "lnv/formats/mobi.h"

// ---- 依赖说明（由另一代理实现，见任务分工）----
// - 漫画/图集：lnv/formats/comics.h 中的 create_comic_adapter(cfg, ref)
//   （cbz/cbz3/zip、cbr/rar、cb7/7z、cbt/tar 以及 gallery 虚拟书）；
// - PDF：create_pdf_adapter(cfg, ref)——预期在 "lnv/formats/pdf.h"（同一代理实现，
//   若该头不存在则与 comics.h 同文件，届时把下面的声明换成对应 include）；
// - MOBI/AZW3：create_mobi_adapter(cfg, ref)——预期在 "lnv/formats/mobi.h"（同一代理）。
// 三个工厂函数目前均未在本仓库头文件中声明，这里给出前向声明以保持本文件自洽：
namespace lnv {
std::unique_ptr<BookAdapter> create_comic_adapter(const AppConfig& cfg, const BookRef& ref) {
    return std::make_unique<ComicAdapter>(cfg, ref);
}
std::unique_ptr<BookAdapter> create_pdf_adapter(const AppConfig& cfg, const BookRef& ref) {
    return std::make_unique<PdfAdapter>(cfg, ref);
}
std::unique_ptr<BookAdapter> create_mobi_adapter(const AppConfig& cfg, const BookRef& ref) {
    return std::make_unique<MobiAdapter>(cfg, ref);
}
}  // namespace lnv

namespace lnv {

std::unique_ptr<BookAdapter> create_adapter(const AppConfig& cfg, const BookRef& ref) {
    // 虚拟书：文件夹图集
    if (ref.virtual_book && ref.format == "gallery") {
        return create_comic_adapter(cfg, ref);
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
    if (fmt == "epub") return std::make_unique<EpubAdapter>(cfg, ref);
    if (fmt == "fb2") return std::make_unique<Fb2Adapter>(cfg, ref);
    if (fmt == "pdf") return create_pdf_adapter(cfg, ref);
    if (fmt == "mobi" || fmt == "azw3") return create_mobi_adapter(cfg, ref);
    // cbz/cbz3/zip、cbr/rar、cb7/7z、cbt/tar → 漫画容器适配器
    return create_comic_adapter(cfg, ref);
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
