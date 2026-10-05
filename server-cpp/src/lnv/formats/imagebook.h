#pragma once
// 图片型书籍（漫画）检测与固定版式适配（对应 Python formats/imagebook.py）。
//
// EPUB 与 MOBI 转换产物共用：spine 图片页 ≥8 且平均每页正文 ≤64 字符 → 按固定
// 版式漫画打开（每图一页，复用漫画管线的 fit/双页/条漫/封面），否则维持流式排版。
// 检测结果进程内缓存（路径 + mtime/size，LRU 32 条），内容变化自然失效。
#include <memory>
#include <string>
#include <vector>

#include "lnv/config.h"
#include "lnv/formats/adapter.h"

namespace lnv {

class EpubAdapter;

// 判定阈值（与 Python formats/imagebook.py 一致）
inline constexpr int kImageBookMinPages = 8;
inline constexpr int kImageBookMaxTextPerPage = 64;
// 廉价预筛：每个图片条目允许的正文 XHTML 字节上限。C++ 的 spine 文档在 meta
// 阶段已整体读入内存，按内存字节数预筛即可（无需再查 zip 元数据）。
inline constexpr long long kImageBookXhtmlBytesPerImage = 8192;

struct ImageBookInfo {
    bool manga = false;
    std::vector<std::string> pages;  // spine 顺序的容器内图片路径
    std::string title;
    long long text_chars = 0;
};

// 带缓存的检测（键 = 文件路径 + mtime/size）
ImageBookInfo image_book_info(EpubAdapter& epub);

// 组装固定版式漫画适配器：接管 epub（含已打开的 zip），ref 为对用户展示的原书
// （EPUB 本体或 MOBI 原文件，meta.format/size 取自它）
std::unique_ptr<BookAdapter> make_epub_comic_adapter(const AppConfig& cfg, const BookRef& ref,
                                                     std::unique_ptr<EpubAdapter> epub,
                                                     const ImageBookInfo& info);

}  // namespace lnv
