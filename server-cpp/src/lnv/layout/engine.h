#pragma once
// 分页引擎与锚点（对应 Python layout/engine.py、layout/anchors.py）
#include "lnv/formats/adapter.h"
#include "lnv/layout/fonts.h"

namespace lnv {

constexpr const char* LAYOUT_ENGINE_VERSION = "1";

struct LayoutItem {
    enum class Kind { TextLine, ChapterMark, Image, Separator };
    Kind kind = Kind::TextLine;
    double height = 0;          // 文本行高 / 图片显示高 / separator 高
    double space_after = 0;
    int chapter = 0, block = 0; // 锚点
    int level = 1;              // heading
    bool bold = false, italic = false;
    double font_size = 0;       // 行字号（标题缩放后）
    U32Text line_text;          // 行文本（含 \u3000 缩进前缀）
    int char_offset = 0;        // 行首在块内的字符偏移
    double y = 0;               // 页内纵向位置（对应 Python item.y）
    // image
    const Block* image_block = nullptr;
    double display_w = 0, display_h = 0;
};

// flow 分页引擎（每次 build 独立实例）
class FlowLayoutEngine {
public:
    struct Deps {
        const DocumentModel* doc = nullptr;
        FontManager* fonts = nullptr;
        const LayoutProfile* profile = nullptr;
        std::string font_id;   // fonts.resolve_id(profile.font.family)
    };

    explicit FlowLayoutEngine(Deps d);
    ~FlowLayoutEngine();
    FlowLayoutEngine(const FlowLayoutEngine&) = delete;
    FlowLayoutEngine& operator=(const FlowLayoutEngine&) = delete;

    // 从 start 锚点排一页；无更多内容返回 nullopt。
    // record 的 anchor = 页首条目锚点；next_anchor = 断点（下页起点），末尾 nullopt。
    struct PageResult {
        PageRecord record;
        std::vector<LayoutItem> items;   // 渲染 JSON 模式需要
        std::optional<std::array<int, 3>> next_anchor;
    };
    std::optional<PageResult> layout_page(const int anchor[3]);

    // 全量构建（pagemap，含 chapters 起始页）；条目不保留。
    // 页数 > 500000 → throw ParseError。
    PageMap build();

    // 渲染 JSON 模式用：给定 pagemap + 页号重建条目（从页锚点 layout_page）。
    std::optional<PageResult> items_for_page(const PageMap& pm, int page);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 服务层 layout_key = "L"+sha1(sorted json {engine,font,geometry,parser})[:12]
std::string compute_layout_key(const LayoutProfile& profile,
                               const std::string& resolved_font_id);

// -------------------------------------------------------------- 锚点定位
struct LocateResult {
    int page = 1;
    int total_pages = 1;
    double ratio = 0.0;
    int chapter_index = 0;
    U32Text chapter_title;
    std::optional<int> anchor_chapter_index;
    bool clamped = false;
};

// pagemap 内锚点二分查找（§4.5）
LocateResult locate(const PageMap& pm, const int target[3]);

}  // namespace lnv
