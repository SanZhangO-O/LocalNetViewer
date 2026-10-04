#include "lnv/model.h"
#include <cmath>
#include <cstdio>
#include <chrono>
#include <ctime>
#include "lnv/http/validate.h"
#include "lnv/util/sha1.h"

namespace lnv {

namespace {
// pydantic 端只有下界校验（Margin ge=0 / Viewport ge=1），Python int 无上界；
// long long → int 的窄化在 >INT_MAX 时是实现定义行为（MSVC 回绕成负数，会让
// avail_*、geometry_json/layout_id 变成垃圾值）。这里饱和到 INT_MAX，与
// model.h::avail_* 的钳制策略一致（>INT_MAX 的几何值仍无法与 Python 逐位一致，
// 属已知边界差异，但绝不再产生负值）。
int saturating_int(long long v) {
    if (v > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
    if (v < 0) return 0;
    return static_cast<int>(v);
}

// 共享缓存（parsed/layouts）可能被外部写坏：JSON 里的 double → int 在
// NaN/Inf/超出 int 范围时是 UB（不只是实现定义），必须在 from_dict 层拦截。
// 抛异常 → 上层 cache.get 按"损坏"删除文件，而不是拿垃圾值继续渲染。
int cache_int(const nlohmann::json& v, int fallback) {
    double d = static_cast<double>(fallback);
    if (v.is_number()) {
        d = v.get<double>();
    } else if (v.is_boolean()) {
        d = v.get<bool>() ? 1.0 : 0.0;
    } else if (!v.is_null()) {
        throw nlohmann::json::type_error::create(302,
            "cache integer field is not a number", &v);
    }
    if (!std::isfinite(d) || d < static_cast<double>(std::numeric_limits<int>::min()) ||
        d > static_cast<double>(std::numeric_limits<int>::max())) {
        throw nlohmann::json::out_of_range::create(406, "cache integer field out of range", &v);
    }
    // C++20 起 static_cast<double>→int 截断语义确定；先显式 std::trunc 保持旧标准一致
    return static_cast<int>(std::trunc(d));
}

// 对象字段版的 cache_int：缺失/null → fallback
int cache_int_of(const nlohmann::json& obj, const char* key, int fallback) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return fallback;
    return cache_int(*it, fallback);
}
}  // namespace

// ---------------------------------------------------------------- JSON 工具
nlohmann::json py_num(double v, int ndigits) {
    double m = std::pow(10.0, ndigits);
    // Python round() 用的是"就近取偶"（且作用于 double 的精确值）；
    // std::round 是"远离零"，在 .5 这类精确平局上会给出不同的几何 JSON →
    // 不同的 layout_id（例如 round(0.625, 2)：Python 0.62，std::round 0.63）。
    double r = std::nearbyint(v * m) / m;
    if (r == std::floor(r) && std::abs(r) < 1e15) {
        // Python round 返回 float，json.dumps 输出 20.0；保持 float 类型
        return nlohmann::json(r);
    }
    return nlohmann::json(r);
}

std::string py_sorted_dumps(const nlohmann::json& j) {
    // nlohmann 默认 json 对象即 std::map（按键排序），dump() 无空白
    return j.dump();
}

std::string iso_time_utc(double epoch_s) {
    time_t t = (time_t)epoch_s;
    struct tm tmv;
    gmtime_s(&tmv, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return buf;
}

// Python datetime.fromtimestamp(ts, tz=utc).isoformat().replace("+00:00","Z")：
// 微秒非 0 时带 ".ffffff"（/files 的 mtime 用这个；progress.updated_at 用秒级格式）
std::string iso_time_utc_micro(double epoch_s) {
    time_t t = static_cast<time_t>(std::floor(epoch_s));
    double frac = epoch_s - static_cast<double>(t);
    long long micros = static_cast<long long>(std::llround(frac * 1e6));
    if (micros >= 1000000) {  // 四舍五入进位
        micros -= 1000000;
        t += 1;
    }
    struct tm tmv;
    gmtime_s(&tmv, &t);
    char buf[48];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
    std::string out = buf;
    if (micros != 0) {
        char us[16];
        std::snprintf(us, sizeof(us), ".%06lld", micros);
        out += us;
    }
    return out + "Z";
}

double now_epoch() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(system_clock::now().time_since_epoch()).count();
}

// ------------------------------------------------------------ LayoutProfile
nlohmann::json LayoutProfile::geometry_json() const {
    nlohmann::json j;
    j["chapter_new_page"] = chapter_new_page;
    j["font"] = {font.family, font.size_px, py_num(font.line_height, 4),
                 py_num(font.letter_spacing_px, 4)};
    j["image"] = {py_num(image.max_width_ratio, 4), py_num(image.max_height_ratio, 4)};
    j["margin"] = {margin.left, margin.right, margin.top, margin.bottom};
    j["mode"] = mode;
    j["paragraph"] = {py_num(paragraph.spacing_px, 4), paragraph.first_line_indent_chars};
    j["strip_overlap_px"] = strip_overlap_px;
    j["viewport"] = {viewport.w, viewport.h};
    return j;
}

std::string LayoutProfile::layout_id() const {
    std::string payload = geometry_json().dump();
    Sha1 sha;
    sha.update(payload.data(), payload.size());
    return "L" + sha.hexdigest().substr(0, 12);
}

LayoutProfile LayoutProfile::from_json(const nlohmann::json& j) {
    // pydantic 等价校验（schemas.py LayoutProfile）。错误必须带 detail.errors：
    // 客户端按 detail.errors[].loc 定位到具体字段，早期实现只回一句
    // "请求参数缺失或非法" 且把 null/小数静默当默认值，会让"参数写错"变成
    // "服务器按别的几何排版了"（layout_id 与分页都对不上）。
    Validator v;
    if (!j.is_object()) {
        v.add({"body"}, "Input should be a valid dictionary or object to extract fields from",
              "model_attributes_type");
        v.done();
    }
    LayoutProfile p;
    if (const nlohmann::json* vp = jv_object_required(v, j, "viewport", {"body", "viewport"})) {
        p.viewport.w =
            saturating_int(jv_int(v, *vp, "w", {"body", "viewport", "w"}, 0, int_ge(1),
                                   /*required=*/true));
        p.viewport.h =
            saturating_int(jv_int(v, *vp, "h", {"body", "viewport", "h"}, 0, int_ge(1),
                                   /*required=*/true));
    }
    if (const nlohmann::json* mg = jv_object(v, j, "margin", {"body", "margin"})) {
        p.margin.left =
            saturating_int(jv_int(v, *mg, "left", {"body", "margin", "left"}, 0, int_ge(0)));
        p.margin.right =
            saturating_int(jv_int(v, *mg, "right", {"body", "margin", "right"}, 0, int_ge(0)));
        p.margin.top =
            saturating_int(jv_int(v, *mg, "top", {"body", "margin", "top"}, 0, int_ge(0)));
        p.margin.bottom =
            saturating_int(jv_int(v, *mg, "bottom", {"body", "margin", "bottom"}, 0, int_ge(0)));
    }
    if (const nlohmann::json* fs = jv_object(v, j, "font", {"body", "font"})) {
        p.font.family = jv_str(v, *fs, "family", {"body", "font", "family"}, "auto");
        p.font.size_px = static_cast<int>(jv_int(v, *fs, "size_px", {"body", "font", "size_px"},
                                                  52, int_range(8, 400)));
        p.font.line_height =
            jv_num(v, *fs, "line_height", {"body", "font", "line_height"}, 1.6,
                   num_range(1.0, 4.0));
        p.font.letter_spacing_px =
            jv_num(v, *fs, "letter_spacing_px", {"body", "font", "letter_spacing_px"}, 0.0,
                   num_range(-10.0, 40.0));
    }
    if (const nlohmann::json* ps = jv_object(v, j, "paragraph", {"body", "paragraph"})) {
        p.paragraph.spacing_px =
            jv_num(v, *ps, "spacing_px", {"body", "paragraph", "spacing_px"}, 20.0,
                   num_range(0.0, 400.0));
        p.paragraph.first_line_indent_chars = static_cast<int>(
            jv_int(v, *ps, "first_line_indent_chars",
                   {"body", "paragraph", "first_line_indent_chars"}, 2, int_range(0, 8)));
    }
    if (const nlohmann::json* is = jv_object(v, j, "image", {"body", "image"})) {
        p.image.max_width_ratio =
            jv_num(v, *is, "max_width_ratio", {"body", "image", "max_width_ratio"}, 1.0,
                   NumBound{0.0, 2.0, /*lo_strict=*/true, false});
        p.image.max_height_ratio =
            jv_num(v, *is, "max_height_ratio", {"body", "image", "max_height_ratio"}, 1.0,
                   NumBound{0.0, 2.0, /*lo_strict=*/true, false});
    }
    p.chapter_new_page = jv_bool(v, j, "chapter_new_page", {"body", "chapter_new_page"}, true);
    p.mode = jv_mode(v, j, "mode", {"body", "mode"}, "flow");
    p.strip_overlap_px = static_cast<int>(
        jv_int(v, j, "strip_overlap_px", {"body", "strip_overlap_px"}, 0, int_range(0, 2000)));
    v.done();
    return p;
}

nlohmann::json LayoutProfile::to_json() const {
    nlohmann::json j;
    j["viewport"] = {{"w", viewport.w}, {"h", viewport.h}};
    j["margin"] = {{"left", margin.left}, {"right", margin.right},
                   {"top", margin.top}, {"bottom", margin.bottom}};
    j["font"] = {{"family", font.family}, {"size_px", font.size_px},
                 {"line_height", font.line_height}, {"letter_spacing_px", font.letter_spacing_px}};
    j["paragraph"] = {{"spacing_px", paragraph.spacing_px},
                      {"first_line_indent_chars", paragraph.first_line_indent_chars}};
    j["image"] = {{"max_width_ratio", image.max_width_ratio},
                  {"max_height_ratio", image.max_height_ratio}};
    j["chapter_new_page"] = chapter_new_page;
    j["mode"] = mode;
    j["strip_overlap_px"] = strip_overlap_px;
    return j;
}

LayoutProfile LayoutProfile::from_dict(const nlohmann::json& j) {
    // 落盘格式即 to_json 的输出；不重新校验。整数统一走 cache_int（防坏缓存的
    // double → int 窄化 UB；超出 int 范围抛异常 → 上层按损坏删除）。
    LayoutProfile p;
    const auto& vp = j["viewport"];
    p.viewport.w = cache_int_of(vp, "w", 0);
    p.viewport.h = cache_int_of(vp, "h", 0);
    const auto& mg = j["margin"];
    p.margin = {cache_int_of(mg, "left", 0), cache_int_of(mg, "right", 0),
                cache_int_of(mg, "top", 0), cache_int_of(mg, "bottom", 0)};
    const auto& fs = j["font"];
    p.font.family = fs.value("family", "auto");
    p.font.size_px = cache_int_of(fs, "size_px", 52);
    p.font.line_height = fs.value("line_height", 1.6);
    p.font.letter_spacing_px = fs.value("letter_spacing_px", 0.0);
    const auto& ps = j["paragraph"];
    p.paragraph.spacing_px = ps.value("spacing_px", 20.0);
    p.paragraph.first_line_indent_chars = cache_int_of(ps, "first_line_indent_chars", 2);
    const auto& is = j["image"];
    p.image.max_width_ratio = is.value("max_width_ratio", 1.0);
    p.image.max_height_ratio = is.value("max_height_ratio", 1.0);
    p.chapter_new_page = j.value("chapter_new_page", true);
    p.mode = j.value("mode", "flow");
    p.strip_overlap_px = cache_int_of(j, "strip_overlap_px", 0);
    return p;
}

// ------------------------------------------------------------ DocumentModel
U32Text Block::text() const {
    U32Text out;
    for (const auto& s : spans) out += s.text;
    return out;
}

size_t Chapter::total_chars() const {
    size_t n = 0;
    for (const auto& b : blocks)
        if (b.type == BlockType::Heading || b.type == BlockType::Paragraph)
            n += b.text().size();
    return n;
}

static const char* span_style_str(SpanStyle s) {
    switch (s) {
        case SpanStyle::Bold: return "bold";
        case SpanStyle::Italic: return "italic";
        case SpanStyle::BoldItalic: return "bold_italic";
        default: return "none";
    }
}
static SpanStyle span_style_of(const std::string& s) {
    if (s == "bold") return SpanStyle::Bold;
    if (s == "italic") return SpanStyle::Italic;
    if (s == "bold_italic") return SpanStyle::BoldItalic;
    return SpanStyle::None;
}

static nlohmann::json span_to_dict(const Span& s) {
    nlohmann::json d;
    d["text"] = u32_to_utf8(s.text);
    if (s.style != SpanStyle::None) d["style"] = span_style_str(s.style);
    return d;
}

static nlohmann::json block_to_dict(const Block& b) {
    nlohmann::json d;
    switch (b.type) {
        case BlockType::Heading:
            d["type"] = "heading";
            d["level"] = b.level;
            {
                nlohmann::json arr = nlohmann::json::array();
                for (auto& s : b.spans) arr.push_back(span_to_dict(s));
                d["spans"] = arr;
            }
            break;
        case BlockType::Paragraph:
            d["type"] = "paragraph";
            {
                nlohmann::json arr = nlohmann::json::array();
                for (auto& s : b.spans) arr.push_back(span_to_dict(s));
                d["spans"] = arr;
            }
            break;
        case BlockType::Image:
            d["type"] = "image";
            d["asset"] = b.asset;
            d["w"] = b.w;
            d["h"] = b.h;
            break;
        case BlockType::Separator:
            d["type"] = "separator";
            break;
    }
    return d;
}

static Block block_from_dict(const nlohmann::json& d) {
    Block b;
    std::string t = d.value("type", "paragraph");
    if (t == "heading") b.type = BlockType::Heading;
    else if (t == "image") b.type = BlockType::Image;
    else if (t == "separator") b.type = BlockType::Separator;
    else b.type = BlockType::Paragraph;
    if (d.contains("spans") && d["spans"].is_array()) {
        for (const auto& s : d["spans"]) {
            Span sp;
            sp.text = utf8_to_u32(s.value("text", ""));
            sp.style = span_style_of(s.value("style", "none"));
            b.spans.push_back(std::move(sp));
        }
    }
    if (b.spans.empty()) {
        auto it = d.find("text");
        if (it != d.end() && it->is_string() && !it->get<std::string>().empty())
            b.spans.push_back({utf8_to_u32(it->get<std::string>()), SpanStyle::None});
    }
    b.level = cache_int_of(d, "level", 1);
    b.asset = d.value("asset", "");
    b.w = cache_int_of(d, "w", 0);
    b.h = cache_int_of(d, "h", 0);
    return b;
}

nlohmann::json DocumentModel::to_dict() const {
    nlohmann::json d;
    d["book_id"] = book_id;
    d["format"] = format;
    d["meta"] = meta;
    nlohmann::json chs = nlohmann::json::array();
    for (const auto& c : chapters) {
        nlohmann::json cj;
        cj["index"] = c.index;
        cj["title"] = u32_to_utf8(c.title);
        nlohmann::json blocks = nlohmann::json::array();
        for (const auto& b : c.blocks) blocks.push_back(block_to_dict(b));
        cj["blocks"] = blocks;
        chs.push_back(cj);
    }
    d["chapters"] = chs;
    return d;
}

DocumentModel DocumentModel::from_dict(const nlohmann::json& d) {
    DocumentModel m;
    m.book_id = d.value("book_id", "");
    m.format = d.value("format", "");
    if (d.contains("meta") && d["meta"].is_object())
        m.meta = d["meta"].get<std::map<std::string, nlohmann::json>>();
    if (d.contains("chapters") && d["chapters"].is_array())
        for (const auto& c : d["chapters"]) {
            Chapter ch;
            ch.index = cache_int_of(c, "index", 0);
            ch.title = utf8_to_u32(c.value("title", ""));
            if (c.contains("blocks") && c["blocks"].is_array())
                for (const auto& b : c["blocks"]) ch.blocks.push_back(block_from_dict(b));
            m.chapters.push_back(std::move(ch));
        }
    return m;
}

// ---------------------------------------------------------------- PageMap
nlohmann::json PageRecord::to_dict() const {
    nlohmann::json d;
    d["page"] = page;
    d["anchor"] = {anchor[0], anchor[1], anchor[2]};
    d["chapter_index"] = chapter_index;
    d["chapter_title"] = u32_to_utf8(chapter_title);
    d["first_line"] = u32_to_utf8(first_line);
    d["image_index"] = image_index;
    d["y0"] = y0;
    d["height"] = height;
    return d;
}

PageRecord PageRecord::from_dict(const nlohmann::json& d) {
    PageRecord r;
    r.page = cache_int_of(d, "page", 0);
    if (d.contains("anchor") && d["anchor"].is_array() && d["anchor"].size() >= 3) {
        r.anchor[0] = cache_int(d["anchor"][0], 0);
        r.anchor[1] = cache_int(d["anchor"][1], 0);
        r.anchor[2] = cache_int(d["anchor"][2], 0);
    }
    r.chapter_index = cache_int_of(d, "chapter_index", 0);
    r.chapter_title = utf8_to_u32(d.value("chapter_title", ""));
    r.first_line = utf8_to_u32(d.value("first_line", ""));
    r.image_index = cache_int_of(d, "image_index", -1);
    r.y0 = d.value("y0", 0.0);
    r.height = d.value("height", 0.0);
    return r;
}

const PageRecord* PageMap::record(int page) const {
    if (pages.empty()) return nullptr;
    // total_pages 与 pages 长度可能不一致（损坏/半截的磁盘缓存）→ 一律以 pages 为准，
    // 否则 pages[page-1] 会越界读（审查 M5）。Python 的 record() 返回 None → 404。
    if (page < 1 || page > static_cast<int>(pages.size())) return nullptr;
    return &pages[page - 1];
}

nlohmann::json PageMap::to_dict() const {
    nlohmann::json d;
    d["book_id"] = book_id;
    d["layout_id"] = layout_id;
    d["mode"] = mode;
    d["total_pages"] = total_pages;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : pages) arr.push_back(p.to_dict());
    d["pages"] = arr;
    nlohmann::json chs = nlohmann::json::array();
    for (const auto& c : chapters) {
        nlohmann::json cj;
        cj["index"] = c.index;
        cj["title"] = u32_to_utf8(c.title);
        cj["total_chars"] = c.total_chars ? nlohmann::json(*c.total_chars) : nlohmann::json(nullptr);
        cj["start_page"] = c.start_page ? nlohmann::json(*c.start_page) : nlohmann::json(nullptr);
        chs.push_back(cj);
    }
    d["chapters"] = chs;
    d["end_anchor"] = {end_anchor[0], end_anchor[1], end_anchor[2]};
    d["created_at"] = created_at;
    return d;
}

PageMap PageMap::from_dict(const nlohmann::json& d) {
    PageMap m;
    m.book_id = d.value("book_id", "");
    m.layout_id = d.value("layout_id", "");
    m.mode = d.value("mode", "flow");
    if (d.contains("chapters") && d["chapters"].is_array())
        for (const auto& c : d["chapters"]) {
            PageMapChapter ch;
            ch.index = cache_int_of(c, "index", 0);
            ch.title = utf8_to_u32(c.value("title", ""));
            auto tc = c.find("total_chars");
            if (tc != c.end() && !tc->is_null()) ch.total_chars = cache_int(*tc, 0);
            auto sp = c.find("start_page");
            if (sp != c.end() && !sp->is_null()) ch.start_page = cache_int(*sp, 0);
            m.chapters.push_back(std::move(ch));
        }
    if (d.contains("pages") && d["pages"].is_array())
        for (const auto& p : d["pages"]) m.pages.push_back(PageRecord::from_dict(p));
    m.total_pages = d.contains("total_pages") && !d["total_pages"].is_null()
                        ? cache_int(d["total_pages"], static_cast<int>(m.pages.size()))
                        : static_cast<int>(m.pages.size());
    // 一致性兜底：total_pages 与 pages 长度不符（损坏/半截缓存）时以 pages 为准。
    // 否则 check_page 会放行 pages[page-1] 越界的页码（审查 M5）。
    if (m.total_pages != static_cast<int>(m.pages.size()))
        m.total_pages = static_cast<int>(m.pages.size());
    if (d.contains("end_anchor") && d["end_anchor"].is_array() && d["end_anchor"].size() >= 3) {
        m.end_anchor[0] = cache_int(d["end_anchor"][0], 0);
        m.end_anchor[1] = cache_int(d["end_anchor"][1], 0);
        m.end_anchor[2] = cache_int(d["end_anchor"][2], 0);
    } else if (!m.pages.empty()) {
        const auto& a = m.pages.back().anchor;
        m.end_anchor[0] = a[0];
        m.end_anchor[1] = a[1] + 1;
        m.end_anchor[2] = 0;
    }
    m.created_at = d.value("created_at", 0.0);
    return m;
}

}  // namespace lnv
