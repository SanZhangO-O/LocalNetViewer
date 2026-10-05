#pragma once
// MOBI / AZW3 适配器（对应 Python formats/mobi.py）
// 依赖 Calibre 的 ebook-convert 转成 EPUB 后走 EpubAdapter。
#include <memory>

#include "lnv/config.h"
#include "lnv/formats/adapter.h"
#include "lnv/formats/epub.h"

namespace lnv {

// Calibre 的 ebook-convert 是否在 PATH 上（/files 的 openable 探测用）
bool calibre_available();

class MobiAdapter : public BookAdapter {
public:
    // ebook-convert 缺失 → UnsupportedFormatError("MOBI/AZW3 需要 Calibre","需安装 Calibre")
    // 转换失败 → ParseError（stderr 尾 500 字节入 message）
    // env 透传给内层 EPUB 适配器（字体管理器，坏书封面用）
    MobiAdapter(const AppConfig& cfg, const BookRef& ref, const AdapterEnv& env = {});

    // 委托内层 EPUB 适配器，format 字段改回 mobi/azw3
    AdapterMeta meta() override;
    DocumentModel document() override;
    std::optional<ImageBytes> cover(int width = 0) override;
    ImageBytes asset(const std::string& path) override;

    // 漫画分流（open_mobi_adapter 用）：检测后按固定版式打开，取走内层 EPUB
    EpubAdapter& inner() { return *inner_; }
    std::unique_ptr<EpubAdapter> take_inner() { return std::move(inner_); }

private:
    // 转换产物路径：cache/converted/{book_id}.{rev}.epub
    std::filesystem::path converted_path() const;
    void convert(const BookRef& ref, const std::filesystem::path& target);

    AppConfig cfg_;
    BookRef ref_;
    AdapterEnv env_;                       // 透传给内层 EpubAdapter（仅存指针）
    std::unique_ptr<EpubAdapter> inner_;   // 转换产物 EPUB（检测后可能被 take_inner 取走）
};

// MOBI/AZW3 工厂：先转换，再按"是否漫画"分流到固定版式或流式适配器
// （对应 Python formats/mobi.open_mobi_adapter）
std::unique_ptr<BookAdapter> open_mobi_adapter(const AppConfig& cfg, const BookRef& ref,
                                               const AdapterEnv& env = {});

}  // namespace lnv
