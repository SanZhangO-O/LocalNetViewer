#pragma once
// MOBI / AZW3 适配器（对应 Python formats/mobi.py）
// 依赖 Calibre 的 ebook-convert 转成 EPUB 后走 EpubAdapter。
#include "lnv/config.h"
#include "lnv/formats/adapter.h"

namespace lnv {

// Calibre 的 ebook-convert 是否在 PATH 上（/files 的 openable 探测用）
bool calibre_available();

class MobiAdapter : public BookAdapter {
public:
    // ebook-convert 缺失 → UnsupportedFormatError("MOBI/AZW3 需要 Calibre","需安装 Calibre")
    // 转换失败 → ParseError（stderr 尾 500 字节入 message）
    MobiAdapter(const AppConfig& cfg, const BookRef& ref);

    // 委托内层 EPUB 适配器，format 字段改回 mobi/azw3
    AdapterMeta meta() override;
    DocumentModel document() override;
    std::optional<ImageBytes> cover(int width = 0) override;
    ImageBytes asset(const std::string& path) override;

private:
    // 转换产物路径：cache/converted/{book_id}.epub
    std::filesystem::path converted_path() const;
    void convert(const BookRef& ref, const std::filesystem::path& target);

    AppConfig cfg_;
    BookRef ref_;
    std::unique_ptr<BookAdapter> inner_;  // EpubAdapter（由 create_epub_adapter 创建）
};

}  // namespace lnv
