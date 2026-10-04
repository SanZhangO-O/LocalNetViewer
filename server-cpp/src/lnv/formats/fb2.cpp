// FB2 适配器实现（照抄 server/app/formats/fb2.py）
#include "lnv/formats/fb2.h"

#include <pugixml.hpp>
#include <windows.h>  // MultiByteToWideChar：把声明编码（cp1251/koi8-r/gb2312…）转 UTF-8

#include "lnv/util/imaging.h"
#include "lnv/util/paths.h"
#include "lnv/util/textutil.h"

namespace lnv {

namespace {

// ---- pugixml 辅助 ----------------------------------------------------
std::string local_name(std::string_view n) {
    size_t pos = n.rfind(':');
    return std::string(pos == std::string_view::npos ? n : n.substr(pos + 1));
}

// 递归遍历的深度上限（深嵌套 FB2 会打崩进程，审查 B3）
constexpr int kMaxXmlDepth = 200;

pugi::xml_node find_first_local(const pugi::xml_node& root, const char* local, int depth = 0) {
    if (depth > kMaxXmlDepth) return {};
    for (pugi::xml_node n = root.first_child(); n; n = n.next_sibling()) {
        if (local_name(n.name()) == local) return n;
        if (pugi::xml_node sub = find_first_local(n, local, depth + 1)) return sub;
    }
    return {};
}

void find_all_local(const pugi::xml_node& root, const char* local,
                    std::vector<pugi::xml_node>& out, int depth = 0) {
    if (depth > kMaxXmlDepth) return;
    for (pugi::xml_node n = root.first_child(); n; n = n.next_sibling()) {
        if (local_name(n.name()) == local) out.push_back(n);
        find_all_local(n, local, out, depth + 1);
    }
}

// itertext：所有后代文本按序拼接
std::string xml_all_text(const pugi::xml_node& n) {
    std::string out;
    std::function<void(const pugi::xml_node&, int)> walk =
        [&](const pugi::xml_node& e, int depth) {
        if (depth > kMaxXmlDepth) return;
        for (pugi::xml_node c = e.first_child(); c; c = c.next_sibling()) {
            if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata)
                out += c.value();
            else
                walk(c, depth + 1);
        }
    };
    walk(n, 0);
    return out;
}

// " ".join(x.split())：折叠全部空白为单空格并去首尾
std::string collapse_ws_utf8(const std::string& s) {    U32Text u = utf8_to_u32(s);    U32Text out;
    out.reserve(u.size());
    bool in_ws = false;
    for (char32_t c : u) {
        bool ws = c == U' ' || c == U'\t' || c == U'\r' || c == U'\n' || c == U'\f' ||
                  c == U'\v' || c == 0x85 || c == 0xA0 || c == 0x3000 ||
                  (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
                  c == 0x202F || c == 0x205F;
        if (ws) { in_ws = true; continue; }
        if (in_ws && !out.empty()) out.push_back(U' ');
        in_ws = false;
        out.push_back(c);
    }
    return u32_to_utf8(out);
}

// Python xmlutil.text_local / all_text(...).strip()：只去首尾，内部空白原样保留
std::string strip_ws_utf8(const std::string& s) {
    U32Text u = utf8_to_u32(s);
    auto is_ws = [](char32_t c) {
        return c == U' ' || c == U'\t' || c == U'\r' || c == U'\n' || c == U'\f' ||
               c == U'\v' || c == 0x85 || c == 0xA0 || c == 0x3000 ||
               (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
               c == 0x202F || c == 0x205F;
    };
    size_t b = 0, e = u.size();
    while (b < e && is_ws(u[b])) ++b;
    while (e > b && is_ws(u[e - 1])) --e;
    return u32_to_utf8(u.substr(b, e - b));
}

// 自写 base64 解码（validate=False 语义：跳过字母表之外的字节）
std::string base64_decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve(in.size() / 4 * 3);
    int buf = 0, bits = 0;
    for (char c : in) {
        int v = val(c);
        if (v < 0) continue;  // '=' 与其它非法字符一律跳过
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(char((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// ---- XML 声明编码嗅探 -------------------------------------------------
// pugixml 只认 UTF 家族（外加 latin1）：声明 windows-1251/koi8-r/gb2312 的 FB2
// （俄文/中文常见）按 UTF-8 硬解会整本乱码，还会被 parsed 缓存固化。Python 的
// lxml 按声明解码，这里先把字节转成 UTF-8 再交给 pugixml（对齐 fb2.py 的行为）。

// 读 XML 声明里的 encoding 名（声明必须在文件开头；缺失返回空）
std::string declared_xml_encoding(const std::string& bytes) {
    size_t limit = std::min<size_t>(bytes.size(), 1024);
    std::string_view head(bytes.data(), limit);
    if (head.rfind("<?xml", 0) != 0) return "";
    size_t decl_end = head.find("?>", 5);
    if (decl_end == std::string_view::npos) return "";
    size_t e = head.find("encoding", 5);
    if (e == std::string_view::npos || e > decl_end) return "";
    size_t i = e + 8;
    while (i < decl_end && (head[i] == ' ' || head[i] == '\t')) ++i;
    if (i >= decl_end || head[i] != '=') return "";
    ++i;
    while (i < decl_end && (head[i] == ' ' || head[i] == '\t')) ++i;
    if (i >= decl_end || (head[i] != '"' && head[i] != '\'')) return "";
    char q = head[i++];
    size_t endq = head.find(q, i);
    if (endq == std::string_view::npos || endq > decl_end) return "";
    return std::string(head.substr(i, endq - i));
}

// 按 XML 声明把非 UTF 字节转 UTF-8；无需转换/无法识别返回 nullopt
std::optional<std::string> transcode_xml_to_utf8(const std::string& bytes) {
    if (bytes.size() < 4) return std::nullopt;
    const unsigned char b0 = (unsigned char)bytes[0], b1 = (unsigned char)bytes[1];
    // BOM（UTF-8/16/32）与无 BOM UTF-16/32 的零字节模式都由 pugixml 自己识别
    if ((b0 == 0xEF && b1 == 0xBB) || (b0 == 0xFF && b1 == 0xFE) ||
        (b0 == 0xFE && b1 == 0xFF) || (b0 == 0 && b1 == 0))
        return std::nullopt;
    std::string enc = declared_xml_encoding(bytes);
    std::string low;
    low.reserve(enc.size());
    for (char c : enc) low.push_back(c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c);
    // UTF 家族/ascii/latin1 保持原行为：不转换，交给 pugixml（共享表里
    // ascii→20127、latin1→1252，直接走转码会改变这些声明的解码口径）
    if (low.rfind("utf", 0) == 0 || low == "ascii" || low == "latin1" ||
        low == "latin-1")
        return std::nullopt;
    unsigned cp = codepage_of(enc);
    if (cp == 0 || bytes.size() > 0x7FFFFFFFull) return std::nullopt;  // 超 int 上限不转
    // 只信 Windows NLS 实际支持的代码页；不认识的名字仍按 pugixml 原行为
    static const unsigned kKnown[] = {437,   850,   852,   855,   857,   858,   860,   861,
                                      862,   863,   864,   865,   866,   869,   874,   932,
                                      936,   949,   950,   1250,  1251,  1252,  1253,  1254,
                                      1255,  1256,  1257,  1258,  20866, 21866, 51932, 28591,
                                      28592, 28593, 28594, 28595, 28596, 28597, 28598, 28599,
                                      28603, 28604, 28605, 28606, 54936};
    bool known = false;
    for (unsigned k : kKnown)
        if (k == cp) { known = true; break; }
    if (!known) return std::nullopt;
    int n = MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (n <= 0) return std::nullopt;
    std::wstring wide((size_t)n, L'\0');
    if (MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), wide.data(), n) != n)
        return std::nullopt;
    int m = WideCharToMultiByte(CP_UTF8, 0, wide.data(), n, nullptr, 0, nullptr, nullptr);
    if (m <= 0) return std::nullopt;
    std::string out((size_t)m, '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), n, out.data(), m, nullptr, nullptr) != m)
        return std::nullopt;
    return out;
}

}  // namespace

Fb2Adapter::Fb2Adapter(const AppConfig& cfg, const BookRef& ref) : cfg_(cfg), ref_(ref) {
    format = "fb2";
    mode = "flow";
}

// doc_ 挂载前由 load() 内的 unique_ptr 托管，析构时必须 delete（审查 B1：原实现
// 只在解析失败分支 delete，成功路径整棵 DOM 随适配器泄漏）
Fb2Adapter::~Fb2Adapter() { delete static_cast<pugi::xml_document*>(doc_); }

void Fb2Adapter::load() {
    if (loaded_) return;
    long long max_mb = cfg_.limits.max_book_mb;
    if (max_mb > 0 && ref_.size > max_mb * 1024 * 1024) {
        throw ParseError("文件超过上限 " + std::to_string(max_mb) + "MB");
    }
    {
        std::ifstream in(ref_.path, std::ios::binary);
        if (!in) throw ParseError("文件无法读取: " + ref_.path.string());
        std::ostringstream ss;
        ss << in.rdbuf();
        xml_bytes_ = ss.str();
    }
    // 声明非 UTF 编码（windows-1251/koi8-r/gb2312 等）先转 UTF-8：pugixml 遇到
    // 不认识的声明只会按 UTF-8 硬解，lxml 则按声明解码（Python 行为基准）
    if (auto converted = transcode_xml_to_utf8(xml_bytes_))
        xml_bytes_ = std::move(*converted);

    // unique_ptr 承接解析：解析失败与后续任何提前 return/异常都不会泄漏 DOM
    // （审查 B1：原实现只在解析失败分支 delete，成功路径整棵 DOM 泄漏）
    auto doc = std::make_unique<pugi::xml_document>();
    // 禁 DTD/实体（防 XXE）
    pugi::xml_parse_result res = doc->load_buffer(
        xml_bytes_.data(), xml_bytes_.size(), pugi::parse_default);
    if (!res) {
        throw ParseError(std::string("FB2 XML 解析失败: ") + res.description());
    }

    // binary：id → (原始字节, content-type)。先写局部，全部成功后再提交，
    // load() 失败后重试不会留下半成品索引
    pugi::xml_node root = doc->document_element();
    std::vector<std::pair<std::string, std::pair<std::string, std::string>>> binaries;
    std::map<std::string, size_t> binary_index;
    std::vector<pugi::xml_node> bins;
    find_all_local(root, "binary", bins);
    for (const pugi::xml_node& b : bins) {
        std::string bid = b.attribute("id").as_string();
        // strip
        size_t bs = bid.find_first_not_of(" \t\r\n");
        if (bs == std::string::npos) continue;
        size_t be = bid.find_last_not_of(" \t\r\n");
        bid = bid.substr(bs, be - bs + 1);
        if (bid.empty()) continue;
        std::string ctype = b.attribute("content-type").as_string();
        if (ctype.empty()) ctype = "image/jpeg";
        std::string payload = xml_all_text(b);
        // strip 空白
        size_t ps = payload.find_first_not_of(" \t\r\n");
        if (ps == std::string::npos) continue;
        size_t pe = payload.find_last_not_of(" \t\r\n");
        payload = payload.substr(ps, pe - ps + 1);
        std::string raw = base64_decode(payload);
        binary_index[bid] = binaries.size();
        binaries.push_back({bid, {std::move(raw), std::move(ctype)}});
    }
    binaries_ = std::move(binaries);
    binary_index_ = std::move(binary_index);
    doc_ = doc.release();
    loaded_ = true;
}

// ---- 元数据辅助 ----
namespace {

// description/title-info（可能不存在 → 空节点）
pugi::xml_node title_info_of(const pugi::xml_document& doc) {
    pugi::xml_node desc = find_first_local(doc.document_element(), "description");
    if (!desc) return {};
    return find_first_local(desc, "title-info");
}

std::optional<std::string> authors_of(const pugi::xml_node& info) {
    if (!info) return std::nullopt;
    std::vector<pugi::xml_node> author_els;
    find_all_local(info, "author", author_els);
    std::vector<std::string> names;
    for (const pugi::xml_node& author : author_els) {
        // 直接子元素中取 first/middle/last/nickname（Python 遍历直接子元素）
        std::vector<std::string> parts;
        for (pugi::xml_node el = author.first_child(); el; el = el.next_sibling()) {
            std::string ln = local_name(el.name());
            if (ln == "first-name" || ln == "middle-name" || ln == "last-name" ||
                ln == "nickname") {
                std::string p = collapse_ws_utf8(xml_all_text(el));
                if (!p.empty()) parts.push_back(std::move(p));
            }
        }
        // first+middle+last+nickname 空格拼接
        std::string name;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i) name += " ";
            name += parts[i];
        }
        if (!name.empty()) names.push_back(std::move(name));
    }
    if (names.empty()) return std::nullopt;
    // 多作者逗号连接
    std::string joined;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) joined += ", ";
        joined += names[i];
    }
    return joined;
}

// 每个 body 下直属 section 分章
std::vector<pugi::xml_node> sections_of(const pugi::xml_document& doc) {
    std::vector<pugi::xml_node> bodies;
    find_all_local(doc.document_element(), "body", bodies);
    std::vector<pugi::xml_node> out;
    for (const pugi::xml_node& body : bodies) {
        for (pugi::xml_node sec = body.first_child(); sec; sec = sec.next_sibling()) {
            if (local_name(sec.name()) == "section") out.push_back(sec);
        }
    }
    return out;
}

// 按 local-name 取属性（Python xmlutil.attr_local）：pugixml 保留 "l:" / "xlink:"
// 前缀，精确比较永远匹配不到 FB2 里的 <image l:href="#id"/>。
std::string attr_local(const pugi::xml_node& el, const char* name) {
    std::string direct = el.attribute(name).as_string();
    if (!direct.empty()) return direct;
    for (pugi::xml_attribute a : el.attributes()) {
        std::string key = a.name();
        auto colon = key.rfind(':');
        if (colon != std::string::npos) {
            if (key.compare(colon + 1, std::string::npos, name) == 0) return a.as_string();
        } else if (key == name) {
            return a.as_string();
        }
    }
    return "";
}

// 章标题：直属 title 子元素的文本
std::string section_title(const pugi::xml_node& section) {
    for (pugi::xml_node child = section.first_child(); child; child = child.next_sibling()) {
        if (local_name(child.name()) == "title") {
            std::string text = collapse_ws_utf8(xml_all_text(child));
            if (!text.empty()) return text;
        }
    }
    return "";
}

// 图片块：href（去 #）→ binary；probe 失败 600×800
std::optional<Block> image_block_of(Fb2Adapter& adapter, const pugi::xml_node& el) {
    // attr_local：FB2 规范里插图写作 <image l:href="#id"/>，pugixml 保留前缀，
    // el.attribute("href") 匹配不到 —— 必须按 local-name 取（Python xmlutil.attr_local）。
    // 取不到就等于所有插图都丢了，分页与页锚点全变。
    std::string href = attr_local(el, "href");
    if (href.empty()) href = attr_local(el, "src");
    size_t hb = href.find_first_not_of('#');
    href = hb == std::string::npos ? std::string() : href.substr(hb);
    href.erase(href.find_last_not_of(" \t\r\n") + 1);
    if (href.empty()) return std::nullopt;
    auto entry = adapter.find_binary(href);
    if (!entry) return std::nullopt;
    std::pair<int, int> size{600, 800};
    auto probed = probe_size(entry->first);
    if (probed.first > 0 && probed.second > 0) size = probed;
    Block b;
    b.type = BlockType::Image;
    b.asset = href;
    b.w = size.first;
    b.h = size.second;
    return b;
}

// _section_blocks 的递归实现。depth 与 find_* 同一上限：十万层嵌套 section/
// epigraph 会让递归走栈溢出（审查 B2），超限子树直接不再下钻（正常书深度 << 200）
void section_blocks(const pugi::xml_node& section, int level, int depth, Fb2Adapter& adapter,
                    std::vector<Block>& blocks) {
    if (depth > kMaxXmlDepth) return;
    static const char* const kTextTags[] = {"v", "text-author", "date", "td", "th"};
    static const char* const kRecurseTags[] = {"epigraph", "cite", "poem", "stanza",
                                               "annotation", "table", "tr"};
    auto in = [](const char* const* arr, size_t n, const std::string& s) {
        for (size_t i = 0; i < n; ++i)
            if (s == arr[i]) return true;
        return false;
    };
    for (pugi::xml_node child = section.first_child(); child; child = child.next_sibling()) {
        std::string tag = local_name(child.name());
        if (tag == "title") {
            std::string text = collapse_ws_utf8(xml_all_text(child));
            if (!text.empty()) {
                Block b;
                b.type = BlockType::Heading;
                b.level = std::max(1, std::min(6, level));
                b.spans.push_back(Span{utf8_to_u32(text), SpanStyle::None});
                blocks.push_back(std::move(b));
            }
            continue;
        }
        if (tag == "subtitle") {
            std::string text = collapse_ws_utf8(xml_all_text(child));
            if (!text.empty()) {
                Block b;
                b.type = BlockType::Heading;
                b.level = std::max(1, std::min(6, level + 1));
                b.spans.push_back(Span{utf8_to_u32(text), SpanStyle::None});
                blocks.push_back(std::move(b));
            }
            continue;
        }
        if (tag == "p" || in(kTextTags, 5, tag)) {
            std::string text = collapse_ws_utf8(xml_all_text(child));
            if (!text.empty()) {
                Block b;
                b.type = BlockType::Paragraph;
                b.spans.push_back(Span{utf8_to_u32(text), SpanStyle::None});
                blocks.push_back(std::move(b));
            }
            continue;
        }
        if (tag == "empty-line") {
            Block b;
            b.type = BlockType::Separator;
            blocks.push_back(std::move(b));
            continue;
        }
        if (tag == "image") {
            if (auto blk = image_block_of(adapter, child)) blocks.push_back(std::move(*blk));
            continue;
        }
        if (tag == "section") {
            section_blocks(child, level + 1, depth + 1, adapter, blocks);  // 嵌套 section +1
            continue;
        }
        if (in(kRecurseTags, 7, tag)) {
            section_blocks(child, level, depth + 1, adapter, blocks);  // 同层递归
            continue;
        }
    }
}

}  // namespace

AdapterMeta Fb2Adapter::meta() {
    load();
    auto* doc = static_cast<pugi::xml_document*>(doc_);
    pugi::xml_node info = title_info_of(*doc);

    std::string title;
    std::optional<std::string> language;
    if (info) {
        // 叶子节点不能用真值判断（pugixml 用 is not None 等价判断存在性）
        pugi::xml_node title_el = find_first_local(info, "book-title");
        if (title_el) title = collapse_ws_utf8(xml_all_text(title_el));
        pugi::xml_node lang_el = find_first_local(info, "lang");
        if (lang_el) {
            std::string lg = collapse_ws_utf8(xml_all_text(lang_el));
            if (!lg.empty()) language = lg;
        }
    }
    std::string title_guess = stem_of(ref_.name());
    if (title.empty()) title = title_guess;

    AdapterMeta meta;
    meta.title = title;
    meta.format = format;
    meta.mode = mode;  // "flow"
    meta.author = authors_of(info);
    meta.language = language;
    meta.size = ref_.size;
    meta.page_count = std::nullopt;
    auto sections = sections_of(*doc);
    for (size_t i = 0; i < sections.size(); ++i) {
        std::string t = section_title(sections[i]);
        if (t.empty()) t = "第 " + std::to_string(i + 1) + " 节";
        meta.chapters.push_back(nlohmann::json{{"index", i},
                                               {"title", t},
                                               {"total_chars", nullptr}});
    }
    meta.supports_json = true;
    return meta;
}

DocumentModel Fb2Adapter::document() {
    load();
    auto* doc = static_cast<pugi::xml_document*>(doc_);
    pugi::xml_node info = title_info_of(*doc);

    std::string title = stem_of(ref_.name());
    std::optional<std::string> language;
    if (info) {
        pugi::xml_node title_el = find_first_local(info, "book-title");
        if (title_el) {
            // Python fb2.py:147 只 strip（不折叠内部空白）：collapse 会把
            // "My\n  Book" 变成 "My Book" 并写进 parsed 缓存（审查 L9）
            std::string t = strip_ws_utf8(xml_all_text(title_el));
            if (!t.empty()) title = t;
        }
        pugi::xml_node lang_el = find_first_local(info, "lang");
        if (lang_el) {
            std::string lg = collapse_ws_utf8(xml_all_text(lang_el));
            if (!lg.empty()) language = lg;
        }
    }

    DocumentModel model;
    model.book_id = ref_.book_id;
    model.format = format;
    auto sections = sections_of(*doc);
    size_t total_chars = 0;
    for (size_t i = 0; i < sections.size(); ++i) {
        Chapter ch;
        ch.index = int(i);
        std::string t = section_title(sections[i]);
        ch.title = utf8_to_u32(t.empty() ? "第 " + std::to_string(i + 1) + " 节" : t);
        std::vector<Block> blocks;
        section_blocks(sections[i], 1, 0, *this, blocks);
        ch.blocks = std::move(blocks);
        total_chars += ch.total_chars();
        model.chapters.push_back(std::move(ch));
    }
    if (model.chapters.empty()) {
        // 无章 → 单匿名章（书名标题）
        Chapter ch;
        ch.index = 0;
        ch.title = utf8_to_u32(title);
        model.chapters.push_back(std::move(ch));
    }
    model.meta["title"] = title;
    model.meta["author"] =
        authors_of(info) ? nlohmann::json(*authors_of(info)) : nlohmann::json(nullptr);
    model.meta["language"] =
        language ? nlohmann::json(*language) : nlohmann::json(nullptr);
    model.meta["total_chars"] = total_chars;
    model.meta["source"] = ref_.vpath;
    return model;
}

// binary id → (原始字节, content-type)；找不到返回 nullopt
std::optional<std::pair<std::string, std::string>> Fb2Adapter::find_binary(
    const std::string& href) {
    load();
    auto it = binary_index_.find(href);
    if (it == binary_index_.end()) return std::nullopt;
    return binaries_[it->second].second;
}

std::optional<ImageBytes> Fb2Adapter::cover(int width) {
    load();
    auto* doc = static_cast<pugi::xml_document*>(doc_);
    std::string name;
    pugi::xml_node info = title_info_of(*doc);
    if (info) {
        pugi::xml_node coverpage = find_first_local(info, "coverpage");
        if (coverpage) {
            // coverpage 后代里的 image 元素（depth 上限同 find_*，防深嵌套栈溢出）
            std::function<void(const pugi::xml_node&, int)> walk =
                [&](const pugi::xml_node& n, int depth) {
                    if (depth > kMaxXmlDepth) return;
                    for (pugi::xml_node c = n.first_child(); c && name.empty();
                         c = c.next_sibling()) {
                        if (local_name(c.name()) == "image") {
                            std::string href = attr_local(c, "href");
                            if (!href.empty()) {
                                size_t hb = href.find_first_not_of('#');
                                name = hb == std::string::npos ? "" : href.substr(hb);
                            }
                        }
                        if (name.empty()) walk(c, depth + 1);
                    }
                };
            walk(coverpage, 0);
        }
    }
    // 退化：第一个 binary
    if (name.empty() && !binaries_.empty()) name = binaries_.front().first;
    auto it = binary_index_.find(name);
    if (it != binary_index_.end()) {
        const auto& [raw, ctype] = binaries_[it->second].second;
        if (width <= 0) return ImageBytes{raw, ctype};
        try {
            ImageBuf img = decode_image(raw, cfg_.limits.max_source_pixels);
            if (img.w > width) img = resize_image(img, width, 0);
            return encode_image(img, EncodeOpts{.prefer_jpeg = true, .jpeg_quality = 84});
        } catch (...) {
            return ImageBytes{raw, ctype};  // 解码失败退原图
        }
    }
    return std::nullopt;
}

ImageBytes Fb2Adapter::asset(const std::string& path) {
    load();
    std::string key = path;
    size_t kb = key.find_first_not_of('#');
    key = kb == std::string::npos ? std::string() : key.substr(kb);
    auto it = binary_index_.find(key);
    if (it == binary_index_.end()) {
        // 大小写不敏感兜底
        std::string low = key;
        for (char& c : low)
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        for (const auto& [bid, idx] : binary_index_) {
            std::string b2 = bid;
            for (char& c : b2)
                if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            if (b2 == low) { it = binary_index_.find(bid); break; }
        }
    }
    if (it == binary_index_.end()) {
        throw AssetNotFoundError("FB2 内不存在资源: " + path);
    }
    return ImageBytes{binaries_[it->second].second.first, binaries_[it->second].second.second};
}

}  // namespace lnv
