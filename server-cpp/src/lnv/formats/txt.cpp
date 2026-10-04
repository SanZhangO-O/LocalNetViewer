// TXT 适配器实现（照抄 server/app/formats/txt.py）
#include "lnv/formats/txt.h"

namespace lnv {

namespace {

// 与 txt.py 常量一致
constexpr size_t kSectionChars = 64 * 1024;          // 切节阈值（字符）
constexpr size_t kNoChapterSplitThreshold = 512 * 1024;  // UTF-8 字节阈值

// ---- U32 上的字符工具 -------------------------------------------------
// Python str.strip() 的空白判定统一用 textutil.h 的 is_py_space（单一实现，避免漂移）

U32Text strip_u32(const U32Text& s) {
    size_t b = 0, e = s.size();
    while (b < e && is_py_space(s[b])) ++b;
    while (e > b && is_py_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

void rstrip_u32(U32Text& s) {
    while (!s.empty() && is_py_space(s.back())) s.pop_back();
}

void lstrip_indent_u32(U32Text& s) {
    // 只剥全角空格 + 半角空格 + tab（_clean_line 语义，缩进交给排版层）
    size_t b = 0;
    while (b < s.size() && (s[b] == U'\u3000' || s[b] == U' ' || s[b] == U'\t')) ++b;
    s = s.substr(b);
}

// _clean_line：\t → 4 空格、rstrip、lstrip(全角空格+半角空格+tab)
U32Text clean_line(const U32Text& raw) {
    U32Text line;
    line.reserve(raw.size());
    for (char32_t c : raw) {
        if (c == U'\t') { line.append(4, U' '); } else { line.push_back(c); }
    }
    rstrip_u32(line);
    lstrip_indent_u32(line);
    return line;
}

// UTF-8 编码后的字节数（Python len(text.encode("utf-8"))）
size_t utf8_byte_len(const U32Text& t) {
    size_t n = 0;
    for (char32_t c : t) {
        if (c < 0x80) n += 1;
        else if (c < 0x800) n += 2;
        else if (c < 0x10000) n += 3;
        else n += 4;
    }
    return n;
}

// _should_section：meta 与 document 共用同一判据
bool should_section(size_t byte_len, size_t total_chars) {
    return byte_len > kNoChapterSplitThreshold && total_chars > kSectionChars;
}

U32Text u32_from(const std::string& s) { return utf8_to_u32(s); }

nlohmann::json chapter_json(int index, const U32Text& title, size_t total_chars) {
    return nlohmann::json{{"index", index},
                          {"title", u32_to_utf8(title)},
                          {"total_chars", total_chars}};
}

}  // namespace

TxtAdapter::TxtAdapter(const AppConfig& cfg, const BookRef& ref)
    : cfg_(cfg), ref_(ref) {
    format = "txt";
    mode = "flow";
}

const DecodedText& TxtAdapter::load_text() {
    if (text_) return *text_;
    long long max_mb = cfg_.limits.max_book_mb;
    long long size = ref_.size;
    if (max_mb > 0 && size > max_mb * 1024 * 1024) {
        throw ParseError("文件超过上限 " + std::to_string(max_mb) + "MB");
    }
    std::string data;
    {
        std::ifstream in(ref_.path, std::ios::binary);
        if (!in) throw ParseError("文件无法读取: " + ref_.path.string());
        // 一次 seek/tell/read 读入：原 `ss << in.rdbuf()` + `ss.str()` 会多拷
        // 一份整书，解码峰值再叠 wstring/U32 时很容易达到数倍文件大小。
        in.seekg(0, std::ios::end);
        std::streamoff len = in.tellg();
        if (len < 0) throw ParseError("文件无法读取: " + ref_.path.string());
        in.seekg(0, std::ios::beg);
        data.resize(size_t(len));
        if (len > 0) {
            in.read(data.data(), len);
            if (size_t(in.gcount()) != size_t(len)) data.resize(size_t(in.gcount()));
        }
    }
    // 编码：配置覆盖优先 → 检测链 → 逐级降级（textutil.h）
    text_ = decode_text(data, cfg_.encoding_override(ref_.vpath));
    return *text_;
}

// 轻量扫描：只产出章标题与字数，不构建 Block（txt.py::_scan_chapters）
static std::vector<nlohmann::json> scan_chapters(const DecodedText& dt,
                                                 const U32Text& title_guess) {
    const U32Text& text = dt.text;
    std::vector<nlohmann::json> chapters;
    bool has_title = false;             // 当前章标题（None 用标志 + 空串表示）
    U32Text cur_title;
    size_t cur_chars = 0;
    bool heading_seen = false;

    auto flush = [&]() {
        // 首个标题之前若没有正文，不要产生一个空章节
        if (!has_title && cur_chars == 0) return;
        chapters.push_back(chapter_json(int(chapters.size()),
                                        has_title ? cur_title : title_guess, cur_chars));
        cur_chars = 0;
    };

    for (const U32Text& raw : split_lines(text)) {
        U32Text line = strip_u32(raw);
        if (line.empty()) continue;
        auto title = match_chapter_title(line);
        if (title) {
            flush();
            heading_seen = true;
            has_title = true;
            cur_title = *title;
            continue;
        }
        cur_chars += line.size();
    }
    flush();
    if (chapters.empty()) {
        chapters.push_back(chapter_json(0, title_guess, cur_chars));
    }

    if (!heading_seen) {
        size_t total = 0;
        if (!chapters.empty() && chapters[0].contains("total_chars")) {
            total = size_t(chapters[0]["total_chars"].get<long long>());
        }
        size_t byte_len = utf8_byte_len(text);
        if (should_section(byte_len, total)) {
            // 按"整行字符数累计 ≥64KB"切"第 N 节"（与 document 同规则）
            std::vector<nlohmann::json> sections;
            size_t acc = 0;
            for (const U32Text& raw : split_lines(text)) {
                U32Text line = strip_u32(raw);
                if (line.empty()) continue;
                acc += line.size();
                if (acc >= kSectionChars) {
                    sections.push_back(chapter_json(
                        int(sections.size()),
                        u32_from("第 " + std::to_string(sections.size() + 1) + " 节"), acc));
                    acc = 0;
                }
            }
            if (acc > 0 || sections.empty()) {
                sections.push_back(chapter_json(
                    int(sections.size()),
                    u32_from("第 " + std::to_string(sections.size() + 1) + " 节"), acc));
            }
            chapters = std::move(sections);
        }
    }
    return chapters;
}

AdapterMeta TxtAdapter::meta() {
    const DecodedText& dt = load_text();
    U32Text title_guess = u32_from(ref_.title_guess());
    auto chapters = scan_chapters(dt, title_guess);

    AdapterMeta m;
    m.title = u32_to_utf8(title_guess);
    m.format = format;
    m.mode = mode;  // "flow"
    m.author = std::nullopt;
    m.language = is_cjk(dt.text) ? std::optional<std::string>("zh") : std::nullopt;
    m.size = ref_.size ? ref_.size : static_cast<long long>(utf8_byte_len(dt.text));
    m.page_count = std::nullopt;
    m.chapters = std::move(chapters);
    m.supports_json = true;
    return m;
}

// 单章按约 64KB 切"第 N 节"（txt.py::_split_chapter_into_sections）
static std::vector<Chapter> split_chapter_into_sections(Chapter& ch) {
    std::vector<Chapter> sections;
    std::vector<Block> cur;
    size_t cur_chars = 0;
    for (Block& block : ch.blocks) {
        cur.push_back(std::move(block));
        cur_chars += cur.back().text().size();
        if (cur_chars >= kSectionChars) {
            Chapter sec;
            sec.index = int(sections.size());
            sec.title = utf8_to_u32("第 " + std::to_string(sections.size() + 1) + " 节");
            sec.blocks = std::move(cur);
            sections.push_back(std::move(sec));
            cur.clear();
            cur_chars = 0;
        }
    }
    if (!cur.empty() || sections.empty()) {
        Chapter sec;
        sec.index = int(sections.size());
        sec.title = utf8_to_u32("第 " + std::to_string(sections.size() + 1) + " 节");
        sec.blocks = std::move(cur);
        sections.push_back(std::move(sec));
    }
    if (sections.size() == 1) {
        // 未超阈值时不改名，保留书名作为唯一章节标题
        sections[0].title = ch.title;
    }
    return sections;
}

DocumentModel TxtAdapter::document() {
    const DecodedText& dt = load_text();
    U32Text title_guess = u32_from(ref_.title_guess());

    // 章节构建（txt.py::_build_chapters）
    std::vector<Chapter> chapters;
    std::vector<Block> blocks;
    bool has_title = false;
    U32Text cur_title;
    bool heading_seen = false;

    auto flush = [&]() {
        // 首个标题之前若没有块，不要产生一个空章节
        if (blocks.empty() && !has_title) return;
        Chapter ch;
        ch.index = int(chapters.size());
        ch.title = has_title ? cur_title : title_guess;
        ch.blocks = std::move(blocks);
        blocks.clear();
        chapters.push_back(std::move(ch));
    };

    for (const U32Text& raw : split_lines(dt.text)) {
        U32Text line = clean_line(raw);
        if (line.empty()) continue;
        auto title = match_chapter_title(line);
        if (title) {
            flush();
            heading_seen = true;
            has_title = true;
            cur_title = *title;
            Block hb;
            hb.type = BlockType::Heading;
            hb.level = 1;
            hb.spans.push_back(Span{std::move(*title), SpanStyle::None});
            blocks.push_back(std::move(hb));
            continue;
        }
        Block pb;
        pb.type = BlockType::Paragraph;
        pb.spans.push_back(Span{std::move(line), SpanStyle::None});
        blocks.push_back(std::move(pb));
    }
    flush();

    if (chapters.empty()) {
        Chapter ch;
        ch.index = 0;
        ch.title = title_guess;
        chapters.push_back(std::move(ch));
    }

    if (!heading_seen && chapters.size() == 1) {
        size_t byte_len = utf8_byte_len(dt.text);
        if (should_section(byte_len, chapters[0].total_chars())) {
            chapters = split_chapter_into_sections(chapters[0]);
        }
    }
    // 重新编号，保证 index 连续
    for (size_t i = 0; i < chapters.size(); ++i) chapters[i].index = int(i);

    size_t total = 0;
    for (const Chapter& ch : chapters) total += ch.total_chars();

    DocumentModel doc;
    doc.book_id = ref_.book_id;
    doc.format = format;
    doc.meta["title"] = u32_to_utf8(title_guess);
    doc.meta["author"] = nullptr;
    doc.meta["language"] = is_cjk(dt.text) ? nlohmann::json("zh") : nlohmann::json(nullptr);
    doc.meta["encoding"] = dt.encoding;
    doc.meta["total_chars"] = total;
    doc.meta["source"] = ref_.vpath;
    doc.chapters = std::move(chapters);
    return doc;
}

}  // namespace lnv
