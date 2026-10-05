#pragma once
// EPUB 适配器（对应 Python formats/epub.py，zip + 手写 OPF/spine 路径）。
// - miniz 直接开 zip；
// - META-INF/container.xml → rootfile full-path → OPF（pugixml，禁 DTD）；
// - manifest + spine（仅 application/xhtml*）；章标题 EPUB3 nav → NCX → 无；
// - document()：手写轻量容错 HTML 解析 → 块模型；
// - meta() 只做容器级解析，不解析正文 HTML。
#include "lnv/config.h"
#include "lnv/formats/adapter.h"
#include "lnv/formats/imagebook.h"
#include "lnv/util/textutil.h"

namespace lnv {

class EpubAdapter : public BookAdapter {
public:
    // env.fonts 提供 AppContext 的字体管理器（坏书文字封面用）；不拥有
    EpubAdapter(const AppConfig& cfg, const BookRef& ref, const AdapterEnv& env = {});
    ~EpubAdapter() override;

    AdapterMeta meta() override;
    DocumentModel document() override;
    std::optional<ImageBytes> cover(int width = 0) override;
    ImageBytes asset(const std::string& path) override;

    // 供 epub.cpp 内的自由辅助函数使用
    std::string resolve_href_pub(const std::string& base_item, const std::string& href);
    std::pair<int, int> image_size_pub(const std::string& name);
    // 容器路径 → OPF 相对路径（用于图片块的 asset 字段）
    std::string opf_relative(const std::string& container_path) const;

    // ---- 图片型检测（formats/imagebook.py 对应）----
    // 扫描 spine 块模型（图片页 ≥8 且平均每页正文 ≤64 字符 → manga），
    // pages = spine 顺序的容器内图片路径；结果由 imagebook.cpp 缓存。
    ImageBookInfo scan_image_book();
    // 容器内条目读取（漫画分流容器用；守卫与 asset 一致）
    std::string read_entry_pub(const std::string& name);
    const BookRef& book_ref() const { return ref_; }

private:
    struct Doc {
        std::string name;   // 容器内规范化路径
        std::string title;  // toc 标题（可空）
        std::string html;   // XHTML 原始字节
    };

    // 模型（对应 Python _EpubModel）
    struct Model {
        std::string title;
        std::optional<std::string> author;
        std::optional<std::string> language;
        std::vector<Doc> docs;
        std::vector<std::string> image_names;  // sorted()
        std::optional<std::string> cover_name;
    };

    // ---- 容器读取 ----
    // 依次尝试：原名 / unquote / 去前导 / / normpath / 小写全名 / basename 唯一匹配
    std::string read_entry(const std::string& name);  // 失败抛 AssetNotFoundError
    std::string read_guarded(int index, const std::string& entry);  // 条目上限守卫
    // 相对 href → 容器内路径（去 fragment、unquote、规范化 ../）
    std::string resolve_href(const std::string& base_item, const std::string& href) const;

    const Model& model();  // 懒加载
    void load_model();

    // HTML → 块（手写容错解析，见 epub.cpp）。
    // image_paths 非空时按出现顺序收集图片的**容器内路径**（图片型检测用）。
    std::vector<Block> html_to_blocks(const std::string& html, const std::string& item_name,
                                      std::vector<std::string>* image_paths = nullptr);
    // 图片资源尺寸（probe 失败 600×800，带缓存）
    std::pair<int, int> image_size(const std::string& name);

    const AppConfig& cfg_;
    BookRef ref_;
    FontManager* fonts_ = nullptr;      // AppContext 的字体管理器（不拥有）

    void* zip_ = nullptr;               // mz_zip_archive*（避免在头里引 miniz）
    std::vector<std::string> names_;    // 容器条目名（zip 名解码后）
    std::map<std::string, int> by_name_;
    std::map<std::string, std::vector<int>> by_lower_;  // 小写全名 → 索引们
    std::map<std::string, std::vector<int>> by_base_;   // 小写 basename → 索引们

    std::optional<Model> model_;
    std::map<std::string, std::pair<int, int>> size_cache_;
    std::string opf_dir_;   // OPF 所在目录（容器路径），用于 opf_relative
};

// 工厂（mobi.cpp 转换产物委托用）
std::unique_ptr<BookAdapter> create_epub_adapter(const AppConfig& cfg, const BookRef& ref,
                                                 const AdapterEnv& env = {});

}  // namespace lnv
