#pragma once
// 漫画 / 文件夹图集适配器（对应 Python formats/comics.py ComicAdapter）
#include "lnv/config.h"
#include "lnv/formats/adapter.h"
#include "lnv/formats/container.h"

namespace lnv {

class ComicAdapter : public BookAdapter {
public:
    // 目录 / 单图文件由调用方（GalleryService）决定；此处按扩展名选容器
    ComicAdapter(const AppConfig& cfg, const BookRef& ref);
    // 外部容器（图片型 EPUB 分流用；见 formats/imagebook.cpp）：容器接管所有权，
    // title 显式给出（EPUB 模型标题），format/size 仍取自 ref
    ComicAdapter(const AppConfig& cfg, const BookRef& ref, std::unique_ptr<Container> container,
                 std::string title);

    AdapterMeta meta() override;
    // 对应 Python isinstance(adapter, ComicAdapter)：只有漫画/图集能做条漫切片
    bool supports_strip() const override { return true; }
    int page_count() override;
    std::optional<ImageBytes> cover(int width = 0) override;
    std::pair<int, int> page_native_size(int page) override;
    ImageBytes page_bytes(int page, const std::string& fit = "viewport", int quality = 85,
                          int viewport_w = 1080, double render_scale = 1.0) override;
    ImageBytes pair_bytes(int page, int quality = 85, int viewport_w = 1080,
                          double render_scale = 1.0, bool rtl = false) override;
    std::vector<StripPage> strip_pages(int viewport_h, int overlap = 0) override;
    ImageBytes strip_slice_bytes(const StripPage& sp, int quality = 85, int viewport_w = 1080,
                                 double render_scale = 1.0,
                                 const std::string& fit = "viewport") override;

    const Container& container() const { return *container_; }

private:
    void init_common();                                     // 两个构造器共用的初始化
    const std::string& entry(int page) const;               // 1 基页号 → 条目名
    std::pair<std::string, std::string> raw_bytes(int page);  // (bytes, mime)
    std::optional<std::pair<int, int>> native_size_for_name(const std::string& name);
    long long entry_limit() const;  // max_archive_entry_mb（字节；0 = 不限）
    void build_names();             // 过滤 + 自然排序 + 总量上限
    // 外部容器（图片型 EPUB 分流）：页序必须保持调用方给出的 spine 顺序，
    // 不得按条目名自然排序（与 Python EpubImageContainer 同契约，PORT_SPEC §4.3）
    bool preserve_order_ = false;
    static std::string path_u8_impl(const std::filesystem::path& p);  // path → UTF-8

    AppConfig cfg_;
    BookRef ref_;
    std::unique_ptr<Container> container_;
    std::vector<std::string> names_;
    std::map<std::string, std::string> mime_cache_;
    std::string title_;
    std::string single_;  // 单图图集：仅此条目
    long long max_page_bytes_ = 0;
    long long max_source_pixels_ = 0;
    int jpeg_quality_ = 85;
    double strip_min_aspect_ = 1.5;
};

}  // namespace lnv
