#pragma once
// 数据模型：LayoutProfile / DocumentModel / PageMap 及其 JSON 序列化
// 对应 Python schemas.py / formats/base.py / layout/engine.py 的落盘契约。
// 序列化必须与 Python json.dumps(..., ensure_ascii=False, sort_keys=按需,
// separators=(",",":")) 逐字节兼容（缓存跨实现共用）。
#include <limits>
#include <optional>
#include <string>
#include <vector>
#include "lnv/types.h"
#include "lnv/util/textutil.h"

namespace lnv {

// ============================================================ LayoutProfile
struct Viewport { int w = 0, h = 0; };
struct Margin   { int left = 0, right = 0, top = 0, bottom = 0; };
struct FontSpec {
    std::string family = "auto";
    int size_px = 52;
    double line_height = 1.6;
    double letter_spacing_px = 0.0;
};
struct ParagraphSpec {
    double spacing_px = 20.0;
    int first_line_indent_chars = 2;
};
struct ImageSpec {
    double max_width_ratio = 1.0;
    double max_height_ratio = 1.0;
};

struct LayoutProfile {
    Viewport viewport;
    Margin margin;
    FontSpec font;
    ParagraphSpec paragraph;
    ImageSpec image;
    bool chapter_new_page = true;
    std::string mode = "flow";          // flow | fixed | strip
    int strip_overlap_px = 0;

    // Python 的 int 不会溢出：margin 只有 ge=0 校验（schemas.py），理论上可给
    // 任意非负值；直接 int 相减在大 margin（或 from_dict 的负 margin）下是 UB。
    // 用 64 位算差后钳到 [1, INT_MAX]，与 Python max(1, w-l-r) 语义一致。
    int avail_width() const {
        const long long v =
            static_cast<long long>(viewport.w) - margin.left - margin.right;
        if (v < 1) return 1;
        if (v > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
        return static_cast<int>(v);
    }
    int avail_height() const {
        const long long v =
            static_cast<long long>(viewport.h) - margin.top - margin.bottom;
        if (v < 1) return 1;
        if (v > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
        return static_cast<int>(v);
    }

    // geometry_dict 对应的 JSON（键排序、无空白、round(…,4)）
    nlohmann::json geometry_json() const;
    // "L" + sha1(geometry json utf8)[:12]
    std::string layout_id() const;

    // pydantic 校验等价实现；失败抛 AppError(BadRequest)
    static LayoutProfile from_json(const nlohmann::json& j);
    nlohmann::json to_json() const;
    static LayoutProfile from_dict(const nlohmann::json& j);  // 落盘格式（无校验）
};

// ============================================================ DocumentModel
// style ∈ none|bold|italic|bold_italic
enum class SpanStyle { None, Bold, Italic, BoldItalic };

struct Span {
    U32Text text;
    SpanStyle style = SpanStyle::None;
};

enum class BlockType { Heading, Paragraph, Image, Separator };

struct Block {
    BlockType type = BlockType::Paragraph;
    std::vector<Span> spans;    // 文本块
    int level = 1;              // heading
    std::string asset;          // image
    int w = 0, h = 0;           // image 原始像素
    // 块内纯文本（heading/paragraph）；缓存 total_chars 用
    U32Text text() const;
};

struct Chapter {
    int index = 0;
    U32Text title;
    std::vector<Block> blocks;
    // total_chars = Σ len(block.text)（仅 heading/paragraph）
    size_t total_chars() const;
};

struct DocumentModel {
    std::string book_id;
    std::string format;
    std::map<std::string, nlohmann::json> meta;  // dict（title/author/language/size/mode…）
    std::vector<Chapter> chapters;

    // 落盘契约：键序固定 book_id, format, meta, chapters；span none 省略 style
    nlohmann::json to_dict() const;
    static DocumentModel from_dict(const nlohmann::json& j);
};

// ============================================================ PageMap
struct PageRecord {
    int page = 0;               // 1 基
    int anchor[3] = {0, 0, 0};  // chapter, block, char
    int chapter_index = 0;
    U32Text chapter_title;
    U32Text first_line;
    int image_index = -1;       // strip：源图序号；fixed：-1
    double y0 = 0;              // strip：切片纵向起点
    double height = 0;          // strip：切片高

    nlohmann::json to_dict() const;
    static PageRecord from_dict(const nlohmann::json& j);
};

struct PageMapChapter {
    int index = 0;
    U32Text title;
    std::optional<int> total_chars;
    std::optional<int> start_page;
};

struct PageMap {
    std::string book_id;
    std::string layout_id;
    std::string mode;           // flow | fixed | strip
    int total_pages = 0;
    std::vector<PageRecord> pages;
    std::vector<PageMapChapter> chapters;
    int end_anchor[3] = {0, 0, 0};
    double created_at = 0;      // epoch 秒

    const PageRecord* record(int page) const;  // 1 基；越界取首/末页，null 仅当空

    nlohmann::json to_dict() const;
    static PageMap from_dict(const nlohmann::json& j);  // 兼容缺 end_anchor/total_pages
};

// ============================================================ 通用 JSON 工具
// Python round(x, n) 后 json.dumps 的数字输出
nlohmann::json py_num(double v, int ndigits);
// 排序键、无空白、ensure_ascii=false 的 dumps 等价
std::string py_sorted_dumps(const nlohmann::json& j);

// ISO8601 UTC "YYYY-MM-DDTHH:MM:SSZ"
std::string iso_time_utc(double epoch_s);
// 同上但带微秒（非 0 时）：Python datetime.isoformat() 等价，/files 的 mtime 用
std::string iso_time_utc_micro(double epoch_s);
// 现在（epoch 秒，浮点）
double now_epoch();

}  // namespace lnv
