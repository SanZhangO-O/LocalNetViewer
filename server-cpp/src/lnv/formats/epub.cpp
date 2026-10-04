// EPUB 适配器实现（照抄 server/app/formats/epub.py 的 zip + 手写 OPF/spine 路径）
#include "lnv/formats/epub.h"

#include <list>  // deque 不支持不完整类型，list 支持（HNode 递归）
#include <miniz.h>
#include <pugixml.hpp>

#include "lnv/formats/container.h"  // decode_zip_name（条目名 cp437 还原）
#include "lnv/util/imaging.h"

namespace lnv {
namespace {
std::string path_to_utf8(const std::filesystem::path& p) {
    auto w = p.wstring();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
}  // namespace


namespace {

// ============================================================ posix 路径工具
std::string posix_dirname(const std::string& p) {
    size_t pos = p.find_last_of('/');
    if (pos == std::string::npos) return "";
    return p.substr(0, pos);
}

std::string posix_basename(const std::string& p) {
    size_t pos = p.find_last_of('/');
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

// 词法规范化（处理 "." 与 ".."），对应 posixpath.normpath（不含 Windows 盘符语义）
std::string posix_normpath(const std::string& p) {
    if (p.empty()) return ".";
    bool abs = !p.empty() && p[0] == '/';
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string seg = p.substr(i, j - i);
        i = j + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (!parts.empty() && parts.back() != "..") parts.pop_back();
            else if (!abs) parts.push_back("..");
            continue;
        }
        parts.push_back(seg);
    }
    std::string out = abs ? "/" : "";
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k) out += "/";
        out += parts[k];
    }
    if (out.empty()) out = abs ? "/" : ".";
    return out;
}

std::string posix_join_norm(const std::string& base_dir, const std::string& href) {
    if (href.empty()) return posix_normpath(base_dir);
    if (base_dir.empty()) return posix_normpath(href);
    return posix_normpath(base_dir + "/" + href);
}

std::string to_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// ============================================================ 实体解码
// 解码 &amp; &lt; &gt; &quot; &apos; &#DDD; &#xHH..;；未知实体原样保留
std::string decode_entities(const std::string& s) {
    if (s.find('&') == std::string::npos) return s;
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '&') { out.push_back(s[i++]); continue; }
        size_t semi = s.find(';', i + 1);
        // 实体名/编号最长 10 字符左右；没有合理的 ';' 则当普通字符
        if (semi == std::string::npos || semi - i > 12) { out.push_back(s[i++]); continue; }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") { out.push_back('&'); i = semi + 1; continue; }
        if (ent == "lt") { out.push_back('<'); i = semi + 1; continue; }
        if (ent == "gt") { out.push_back('>'); i = semi + 1; continue; }
        if (ent == "quot") { out.push_back('"'); i = semi + 1; continue; }
        if (ent == "apos") { out.push_back('\''); i = semi + 1; continue; }
        if (!ent.empty() && (ent[0] == '#' || ent[0] == 'x' || ent[0] == 'X')) {
            long code = -1;
            if (ent[0] == '#') {
                bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
                try {
                    code = std::stol(hex ? ent.substr(2) : ent.substr(1), nullptr,
                                     hex ? 16 : 10);
                } catch (...) { code = -1; }
            }
            if (code > 0 && code <= 0x10FFFF) {
                // 编码为 UTF-8
                unsigned cp = unsigned(code);
                if (cp < 0x80) out.push_back(char(cp));
                else if (cp < 0x800) {
                    out.push_back(char(0xC0 | (cp >> 6)));
                    out.push_back(char(0x80 | (cp & 0x3F)));
                } else if (cp < 0x10000) {
                    out.push_back(char(0xE0 | (cp >> 12)));
                    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(char(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(char(0xF0 | (cp >> 18)));
                    out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
                    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(char(0x80 | (cp & 0x3F)));
                }
                i = semi + 1;
                continue;
            }
        }
        out.push_back(s[i++]);  // 未知实体原样
    }
    return out;
}

// ============================================================ 轻量 HTML 树
// children 用 deque：解析期间入栈的元素指针必须稳定（vector 会整块搬移）
struct HNode {
    bool is_text = false;
    std::string name;                                  // 元素名（小写）
    std::map<std::string, std::string> attrs;          // 属性（键小写，值已实体解码）
    std::string text;                                  // 文本节点内容（已实体解码）
    std::list<HNode> children;  // std::list 允许不完整类型

    HNode* add_element(std::string n) {
        children.emplace_back();
        children.back().name = std::move(n);
        return &children.back();
    }
    void add_text(std::string t) {
        if (t.empty()) return;
        children.emplace_back();
        children.back().is_text = true;
        children.back().text = std::move(t);
    }
};

inline bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// 手写容错 HTML 解析：标签流 + 实体；script/style 原始内容整体跳过
//
// 深度上限：树本身必须有限深 —— 后续所有遍历（has_block_descendant / epub_walk /
// find_body / collect_spans / gather_all_text）以及 std::list<HNode> 的递归析构都按层
// 递归，实测 6 万层 <div> 会让整个进程 STATUS_STACK_OVERFLOW 死亡（审查 B3）。
// 超过上限的**整棵子树被丢弃**（不是拍平成兄弟节点）：Python 对这类文档给出
// 0 字符 / 0 页，丢弃子树才能与之一致；正常文档（深度 << 200）完全不受影响。
constexpr size_t kMaxHtmlDepth = 200;

HNode parse_html_tree(const std::string& src) {
    HNode root;
    root.name = "#root";
    std::vector<HNode*> stack{&root};
    // >0 表示正在丢弃一个"超过深度上限"的子树：其中的文本与标签全部忽略，
    // 只数开/闭标签以找到该子树的结尾。
    size_t skipped_depth = 0;
    static const char* const kVoidTags[] = {"area", "base", "br", "col", "embed", "hr",
                                            "img", "input", "link", "meta", "param",
                                            "source", "track", "wbr"};
    auto is_void = [](const std::string& n) {
        for (const char* t : kVoidTags)
            if (n == t) return true;
        return false;
    };

    size_t i = 0, n = src.size();
    while (i < n) {
        size_t lt = src.find('<', i);
        if (lt == std::string::npos) {
            if (skipped_depth == 0) stack.back()->add_text(decode_entities(src.substr(i)));
            break;
        }
        if (lt > i && skipped_depth == 0)
            stack.back()->add_text(decode_entities(src.substr(i, lt - i)));
        i = lt;
        if (i + 1 >= n) break;
        // 注释
        if (src.compare(i, 4, "<!--") == 0) {
            size_t end = src.find("-->", i + 4);
            i = end == std::string::npos ? n : end + 3;
            continue;
        }
        // DOCTYPE / 处理指令
        if (src[i + 1] == '!' || src[i + 1] == '?') {
            size_t end = src.find('>', i);
            i = end == std::string::npos ? n : end + 1;
            continue;
        }
        // 结束标签
        if (src[i + 1] == '/') {
            size_t end = src.find('>', i);
            if (skipped_depth > 0) {   // 被丢弃的子树：只数层级，不弹栈
                skipped_depth--;
                i = end == std::string::npos ? n : end + 1;
                continue;
            }
            std::string tag = to_lower(src.substr(i + 2, (end == std::string::npos ? n : end) - i - 2));
            // 去掉尾部空白
            while (!tag.empty() && is_space(tag.back())) tag.pop_back();
            // 弹栈直到弹出同名元素（找不到就整条忽略，容错）
            if (!tag.empty()) {
                for (size_t k = stack.size(); k-- > 1;) {
                    if (stack[k]->name == tag) {
                        stack.resize(k);
                        break;
                    }
                }
            }
            i = end == std::string::npos ? n : end + 1;
            continue;
        }
        // 开始标签
        size_t j = i + 1;
        while (j < n && !is_space(src[j]) && src[j] != '>' && src[j] != '/') ++j;
        std::string tag = to_lower(src.substr(i + 1, j - i - 1));
        if (tag.empty()) {  // "<" 后不是标签，当普通文本
            if (skipped_depth == 0) stack.back()->add_text("<");
            i = i + 1;
            continue;
        }
        bool too_deep = skipped_depth > 0 || stack.size() > kMaxHtmlDepth;
        HNode* node = too_deep ? nullptr : stack.back()->add_element(tag);
        bool self_closed = false;
        // 属性解析
        while (j < n) {
            while (j < n && is_space(src[j])) ++j;
            if (j >= n) break;
            if (src[j] == '>') { ++j; break; }
            if (src[j] == '/') {  // 自闭合
                if (j + 1 < n && src[j + 1] == '>') { self_closed = true; j += 2; break; }
                ++j;
                continue;
            }
            size_t ks = j;
            while (j < n && !is_space(src[j]) && src[j] != '=' && src[j] != '>' && src[j] != '/') ++j;
            std::string key = to_lower(src.substr(ks, j - ks));
            while (j < n && is_space(src[j])) ++j;
            std::string value;
            if (j < n && src[j] == '=') {
                ++j;
                while (j < n && is_space(src[j])) ++j;
                if (j < n && (src[j] == '"' || src[j] == '\'')) {
                    char q = src[j++];
                    size_t ve = src.find(q, j);
                    if (ve == std::string::npos) { value = src.substr(j); j = n; }
                    else { value = src.substr(j, ve - j); j = ve + 1; }
                } else {
                    size_t ve = j;
                    while (ve < n && !is_space(src[ve]) && src[ve] != '>') ++ve;
                    value = src.substr(j, ve - j);
                    j = ve;
                }
                value = decode_entities(value);
            }
            if (!key.empty() && node) node->attrs[key] = value;
        }
        i = j;
        if (self_closed || is_void(tag)) continue;
        if (too_deep) {           // 丢弃整棵超深子树（与 Python 的 0 字符一致）
            skipped_depth++;
            continue;
        }
        // script/style：原始内容整体跳过（内容不是 HTML，也不参与文本）
        if (tag == "script" || tag == "style") {
            std::string close = "</" + tag;
            size_t ci = i;
            while (ci < n) {
                size_t pos;
                // 大小写不敏感地找 "</script"/"</style"
                for (pos = ci; pos + close.size() <= n; ++pos) {
                    size_t k = 0;
                    while (k < close.size() &&
                           to_lower(std::string(1, src[pos + k]))[0] == close[k]) ++k;
                    if (k == close.size()) break;
                }
                if (pos + close.size() > n) { ci = n; break; }
                size_t after = pos + close.size();
                while (after < n && is_space(src[after])) ++after;
                if (after < n && src[after] == '>') { ci = after + 1; break; }
                ci = pos + 1;  // 假闭合（如 "</scriptx"），继续找
            }
            i = ci;
            continue;  // 该元素不入栈（整棵子树等价于被跳过）
        }
        stack.push_back(node);
    }
    return root;
}

// ============================================================ HTML 块规则
const char* const kHeadingLevels[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
const char* const kTextContainers[] = {
    "p", "div", "li", "blockquote", "pre", "td", "th", "figcaption",
    "dd", "dt", "section", "article", "main", "aside", "header", "footer",
    "caption", "address"};
const char* const kBlockLevelExtra[] = {"tr", "table", "ul", "ol", "dl", "figure"};
const char* const kSkipTags[] = {"script", "style", "head", "title", "nav", "noscript",
                                 "template", "iframe", "object", "video", "audio", "math"};

int heading_level_of(const std::string& n) {
    for (int i = 0; i < 6; ++i)
        if (n == kHeadingLevels[i]) return i + 1;
    return 0;
}

bool is_text_container(const std::string& n) {
    for (const char* t : kTextContainers)
        if (n == t) return true;
    return false;
}

bool is_block_level(const std::string& n) {
    if (heading_level_of(n) || is_text_container(n)) return true;
    for (const char* t : kBlockLevelExtra)
        if (n == t) return true;
    return false;
}

bool is_skip_tag(const std::string& n) {
    for (const char* t : kSkipTags)
        if (n == t) return true;
    return false;
}

// CJK 判定（epub.py _CJK_SPACE_RE 的字符类）
inline bool is_cjk_class(char32_t c) {
    return (c >= 0x3000 && c <= 0x303F) || (c >= 0x3040 && c <= 0x30FF) ||
           (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||
           (c >= 0xFF00 && c <= 0xFFEF);
}

// Python str.strip() 的空白集合（元数据用）
inline bool is_ws_cp(char32_t c) {
    switch (c) {
        case U'\t': case U'\n': case U'\v': case U'\f': case U'\r':
        case U' ': case U'\x85': case U'\xA0': case U'\x1680':
        case U'\x2028': case U'\x2029': case U'\x202F': case U'\x205F': case U'\x3000':
            return true;
        default:
            return c >= 0x2000 && c <= 0x200A;
    }
}

// 空白折叠：[ \t\r\n\f\v]+ → 单空格（等价 re.sub(r"\s+", " ", s)）
// 注意**首尾也要各留一个空格**：Python epub.py:482 就是这样处理每个文本节点的，
// 之后 _tidy_spans 只对首个 span 做 lstrip、末个 span 做 rstrip。
// 早期实现在 out 为空时丢掉前导空白，于是 <p>a<b> b</b></p> 会拼成 "ab"、
// <p>x<span> </span>y</p> 会拼成 "xy" —— 词粘连、折行位置与页数全变。
U32Text collapse_ws(const U32Text& in) {
    U32Text out;
    out.reserve(in.size());
    bool in_ws = false;
    for (char32_t c : in) {
        if (c == U' ' || c == U'\t' || c == U'\r' || c == U'\n' || c == U'\f' ||
            c == U'\v') {
            in_ws = true;
            continue;
        }
        if (in_ws) out.push_back(U' ');
        in_ws = false;
        out.push_back(c);
    }
    if (in_ws) out.push_back(U' ');  // 与 re.sub 一致（尾部也替换）
    return out;
}

// CJK 相邻空格删除：(?<=[CJK])[ \t]+(?=[CJK])（折叠后至多单空格）
U32Text remove_cjk_spaces(const U32Text& in) {
    if (in.size() < 3) return in;
    U32Text out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char32_t c = in[i];
        if (c == U' ' && i > 0 && i + 1 < in.size() && is_cjk_class(in[i - 1]) &&
            is_cjk_class(in[i + 1])) {
            continue;
        }
        out.push_back(c);
    }
    return out;
}

U32Text tidy_one(const U32Text& t) { return remove_cjk_spaces(collapse_ws(t)); }

void append_span(std::vector<Span>& spans, U32Text text, SpanStyle style) {
    if (text.empty()) return;
    if (!spans.empty() && spans.back().style == style) {
        spans.back().text += text;
    } else {
        spans.push_back(Span{std::move(text), style});
    }
}

// _tidy_spans：逐 span 折叠 + CJK 删空格；首尾 trim；合并空 span；中间 rstrip
void tidy_spans(std::vector<Span>& spans) {
    for (Span& s : spans) s.text = tidy_one(s.text);
    if (!spans.empty()) {
        // 首 span 去 leading 空格、末 span 去 trailing 空格
        size_t b = 0;
        while (b < spans.front().text.size() && spans.front().text[b] == U' ') ++b;
        spans.front().text = spans.front().text.substr(b);
        while (!spans.back().text.empty() && spans.back().text.back() == U' ')
            spans.back().text.pop_back();
    }
    std::vector<Span> cleaned;
    cleaned.reserve(spans.size());
    for (Span& s : spans)
        if (!s.text.empty()) cleaned.push_back(std::move(s));
    spans = std::move(cleaned);
    for (size_t i = 0; i + 1 < spans.size(); ++i) {
        while (!spans[i].text.empty() && spans[i].text.back() == U' ')
            spans[i].text.pop_back();
    }
}

U32Text spans_text(const std::vector<Span>& spans) {
    U32Text out;
    for (const Span& s : spans) out += s.text;
    return out;
}

SpanStyle style_of(bool bold, bool italic) {
    if (bold && italic) return SpanStyle::BoldItalic;
    if (bold) return SpanStyle::Bold;
    if (italic) return SpanStyle::Italic;
    return SpanStyle::None;
}

// 是否有块级后代（_has_block_descendant）
bool has_block_descendant(const HNode& n) {
    for (const HNode& c : n.children) {
        if (c.is_text) continue;
        if (is_block_level(c.name)) return true;
        if (has_block_descendant(c)) return true;
    }
    return false;
}

// 收集文本，语义 = BeautifulSoup 的 get_text(" ", strip=True)：
// **每个文本节点各自 strip（只去首尾）**，非空片段之间用单个空格连接。
// 注意不能整体 collapse —— 那样 "Chapter    One" 会被压成 "Chapter One"，
// 与 Python 的 nav 目录标题不一致（审查 M7）。Python 的 str.strip() 覆盖 Unicode
// 空白（含 U+3000/U+00A0），这里按同一集合判断。
U32Text gather_all_text(const HNode& n) {
    auto is_uws = [](char32_t c) {
        return c == U' ' || c == U'\t' || c == U'\r' || c == U'\n' || c == U'\f' ||
               c == U'\v' || c == 0x85 || c == 0xA0 || c == 0x3000 ||
               (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
               c == 0x202F || c == 0x205F;
    };
    std::vector<U32Text> parts;
    std::function<void(const HNode&)> walk = [&](const HNode& e) {
        for (const HNode& c : e.children) {
            if (c.is_text) {
                U32Text t = utf8_to_u32(c.text);
                size_t b = 0, en = t.size();
                while (b < en && is_uws(t[b])) ++b;
                while (en > b && is_uws(t[en - 1])) --en;
                if (en > b) parts.push_back(t.substr(b, en - b));
            } else {
                walk(c);
            }
        }
    };
    walk(n);
    U32Text joined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) joined.push_back(U' ');
        joined += parts[i];
    }
    return joined;
}

// _append_direct_text：只取容器直接文本子节点，输出单个 paragraph
void append_direct_text(std::vector<Block>& blocks, const HNode& tag) {
    std::vector<U32Text> parts;
    for (const HNode& c : tag.children) {
        if (!c.is_text) continue;
        U32Text t = utf8_to_u32(c.text);
        // strip 后非空才收（Python: if text.strip()）
        U32Text collapsed = collapse_ws(t);
        bool any = false;
        for (char32_t ch : collapsed)
            if (ch != U' ') { any = true; break; }
        if (any) parts.push_back(std::move(t));
    }
    if (parts.empty()) return;
    U32Text joined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) joined.push_back(U' ');
        joined += parts[i];
    }
    U32Text text = tidy_one(joined);
    // strip 两端空格
    size_t b = 0, e2 = text.size();
    while (b < e2 && text[b] == U' ') ++b;
    while (e2 > b && text[e2 - 1] == U' ') --e2;
    text = text.substr(b, e2 - b);
    if (text.empty()) return;
    Block blk;
    blk.type = BlockType::Paragraph;
    blk.spans.push_back(Span{std::move(text), SpanStyle::None});
    blocks.push_back(std::move(blk));
}

// _spans：后代文本节点 + br→空格 + img→空格；样式取祖先链（b/strong、i/em）
void collect_spans(const HNode& n, bool bold, bool italic,
                   std::vector<Span>& spans) {
    for (const HNode& c : n.children) {
        if (c.is_text) {
            U32Text t = tidy_one(utf8_to_u32(c.text));
            if (!t.empty()) append_span(spans, std::move(t), style_of(bold, italic));
            continue;
        }
        if (c.name == "br") {
            append_span(spans, U32Text(1, U' '), style_of(bold, italic));
            continue;
        }
        if (c.name == "img") {
            append_span(spans, U32Text(1, U' '), SpanStyle::None);
            continue;
        }
        bool b2 = bold || c.name == "b" || c.name == "strong";
        bool i2 = italic || c.name == "i" || c.name == "em";
        collect_spans(c, b2, i2, spans);
    }
}

}  // namespace

// ============================================================ EpubAdapter
EpubAdapter::EpubAdapter(const AppConfig& cfg, const BookRef& ref) : cfg_(cfg), ref_(ref) {
    format = "epub";
    mode = "flow";
    auto* zip = new mz_zip_archive{};
    if (!mz_zip_reader_init_file(zip, path_to_utf8(ref_.path).c_str(), 0)) {
        delete zip;
        throw ParseError("EPUB 容器无法打开: " + ref_.path.string());
    }
    zip_ = zip;
    int count = mz_zip_reader_get_num_files(zip);
    for (int idx = 0; idx < count; ++idx) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(zip, unsigned(idx), &st)) continue;
        // 条目名解码（flag 0x800 → 原名；否则 cp437 还原）
        std::string name = decode_zip_name(st.m_filename, st.m_bit_flag);
        names_.push_back(name);
        by_name_[name] = idx;
        by_lower_[to_lower(name)].push_back(idx);
        by_base_[to_lower(posix_basename(name))].push_back(idx);
    }
}

EpubAdapter::~EpubAdapter() {
    if (zip_) {
        mz_zip_reader_end(static_cast<mz_zip_archive*>(zip_));
        delete static_cast<mz_zip_archive*>(zip_);
    }
}

// 条目大小上限（压缩包守卫，防 zip 炸弹）
namespace {
struct EpubOut {
    std::string* out = nullptr;
    long long limit = 0;   // ≤0 不限
    bool too_big = false;
};

// miniz 流式回调：返回已消费字节数，返回 0 表示中止（读到 limit+1 即判定超限）
size_t epub_read_cb(void* pOpaque, mz_uint64, const void* pBuf, size_t n) {
    auto* ctx = static_cast<EpubOut*>(pOpaque);
    if (ctx->limit > 0 && (long long)(ctx->out->size() + n) > ctx->limit + 1) {
        size_t take = (size_t)std::max<long long>(0, ctx->limit + 1 - (long long)ctx->out->size());
        ctx->out->append(static_cast<const char*>(pBuf), take);
        ctx->too_big = true;
        return 0;
    }
    ctx->out->append(static_cast<const char*>(pBuf), n);
    return n;
}
}  // namespace

std::string EpubAdapter::read_guarded(int index, const std::string& entry) {
    auto* zip = static_cast<mz_zip_archive*>(zip_);
    long long limit = (cfg_.limits.max_archive_entry_mb > 0
                           ? cfg_.limits.max_archive_entry_mb * 1024 * 1024
                           : 0);
    // 流式读取 + 上限回调。**不要**按"声明的解压后大小"提前拒绝：声明可能撒谎
    // （实测：中心目录写 2.75 GiB、实际几百字节的文件，Python 能正常读），而
    // extract_to_callback 是边解压边回调、不会按声明尺寸预分配，超限由回调中止
    // ——安全性由 limit+1 保证，行为与 Python 的 fh.read(limit+1) 一致。
    std::string out;
    EpubOut ctx{&out, limit, false};
    if (!mz_zip_reader_extract_to_callback(zip, unsigned(index), epub_read_cb, &ctx, 0) ||
        ctx.too_big) {
        out.clear();
        if (ctx.too_big && limit > 0) {
            throw ParseError("EPUB 条目超过上限（" +
                             std::to_string(limit / 1024 / 1024) + "MB）: " + entry);
        }
        throw ParseError("EPUB 条目无法读取: " + entry);
    }
    return out;
}

// 依次尝试：原名 / unquote / 去前导 / / normpath / 小写全名 / basename 唯一匹配
std::string EpubAdapter::read_entry(const std::string& name) {
    std::vector<std::string> cands;
    cands.push_back(name);
    cands.push_back(url_unquote(name));
    {
        std::string stripped = name;
        size_t p = stripped.find_first_not_of('/');
        cands.push_back(p == std::string::npos ? std::string() : stripped.substr(p));
    }
    cands.push_back(posix_normpath(name));
    for (const std::string& cand : cands) {
        auto it = by_name_.find(cand);
        if (it != by_name_.end()) return read_guarded(it->second, cand);
    }
    {
        auto it = by_lower_.find(to_lower(name));
        if (it != by_lower_.end() && !it->second.empty())
            return read_guarded(it->second.front(), it->first);
    }
    // basename 兜底只在唯一匹配时使用（多同名无法消歧）
    std::string base = to_lower(posix_basename(name));
    auto it = by_base_.find(base);
    if (it != by_base_.end()) {
        if (it->second.size() == 1) return read_guarded(it->second.front(), name);
        if (it->second.size() > 1) {
            throw AssetNotFoundError("EPUB 内存在多个同名资源，无法消歧: " + name +
                                     "（" + std::to_string(it->second.size()) + " 个匹配）");
        }
    }
    throw AssetNotFoundError("EPUB 内不存在资源: " + name);
}

std::string EpubAdapter::resolve_href(const std::string& base_item,
                                      const std::string& href_raw) const {
    std::string href = url_unquote(href_raw);
    // strip
    size_t b = href.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = href.find_last_not_of(" \t\r\n");
    href = href.substr(b, e - b + 1);
    if (href.rfind("data:", 0) == 0) return "";
    size_t hash = href.find('#');
    if (hash != std::string::npos) href = href.substr(0, hash);
    if (href.empty()) return "";
    if (href[0] == '/') {
        std::string stripped = href.substr(href.find_first_not_of('/'));
        return posix_normpath(stripped);
    }
    return posix_join_norm(posix_dirname(base_item), href);
}

// ---- pugixml 辅助（禁 DTD 防 XXE）----
static pugi::xml_document parse_xml(const std::string& bytes, bool& ok) {
    pugi::xml_document doc;
    // parse_default（pugixml 默认不解析 DTD 实体，天然防 XXE）
    pugi::xml_parse_result res =
        doc.load_buffer(bytes.data(), bytes.size(), pugi::parse_default);
    ok = res;
    return doc;
}

static std::string local_name(std::string_view n) {
    size_t pos = n.rfind(':');
    return std::string(pos == std::string_view::npos ? n : n.substr(pos + 1));
}

// XML 树遍历的深度上限：pugixml 能解析任意深度的文档，而下面这些遍历是递归的
// （深 OPF/NCX/FB2 同样能打崩进程，审查 B3）。超过上限的分支直接不再下钻。
constexpr int kMaxXmlDepth = 200;

// 深度优先找第一个 localname 匹配的后代（find_local 语义）
static pugi::xml_node find_first_local(const pugi::xml_node& root, const char* local,
                                       int depth = 0) {
    if (depth > kMaxXmlDepth) return {};
    for (pugi::xml_node n = root.first_child(); n; n = n.next_sibling()) {
        if (local_name(n.name()) == local) return n;
        pugi::xml_node sub = find_first_local(n, local, depth + 1);
        if (sub) return sub;
    }
    return {};
}

// 所有 localname 匹配的后代（文档序）
static void find_all_local(const pugi::xml_node& root, const char* local,
                           std::vector<pugi::xml_node>& out, int depth = 0) {
    if (depth > kMaxXmlDepth) return;
    for (pugi::xml_node n = root.first_child(); n; n = n.next_sibling()) {
        if (local_name(n.name()) == local) out.push_back(n);
        find_all_local(n, local, out, depth + 1);
    }
}

// itertext：所有后代文本拼接
static std::string xml_all_text(const pugi::xml_node& n) {
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

// " ".join(x.split())：折叠所有空白为单空格并 trim
static std::string collapse_xml_text(const std::string& s) {
    U32Text u = utf8_to_u32(s);
    U32Text c = collapse_ws(u);
    size_t b = 0, e = c.size();
    while (b < e && c[b] == U' ') ++b;
    while (e > b && c[e - 1] == U' ') --e;
    return u32_to_utf8(c.substr(b, e - b));
}

// Python xmlutil.text_local / all_text(...).strip()：**只去首尾**，内部空白原样保留。
// 元数据（dc:title / dc:creator / dc:language / NCX 标签）必须用它：
// 用 collapse_xml_text 会把 "My\n  Book" 变成 "My Book"，与 Python 不同。
static std::string strip_xml_text(const std::string& s) {
    U32Text u = utf8_to_u32(s);
    size_t b = 0, e = u.size();
    while (b < e && is_ws_cp(u[b])) ++b;
    while (e > b && is_ws_cp(u[e - 1])) --e;
    return u32_to_utf8(u.substr(b, e - b));
}

// ---- 目录标题 ----
// EPUB3 nav：nav 文档内所有 <a> 的 href/text
static std::map<std::string, std::string> toc_from_nav(const std::string& html,
                                                       const std::string& nav_name) {
    std::map<std::string, std::string> out;
    HNode root = parse_html_tree(html);
    std::function<void(const HNode&)> walk = [&](const HNode& n) {
        for (const HNode& c : n.children) {
            if (c.is_text) continue;
            if (c.name == "a") {
                auto href = c.attrs.find("href");
                U32Text text = gather_all_text(c);
                if (href != c.attrs.end() && !text.empty()) {
                    std::string target = posix_join_norm(
                        posix_dirname(nav_name), url_unquote(href->second).substr(
                                                     0, url_unquote(href->second).find('#')));
                    out.emplace(std::move(target), u32_to_utf8(text));
                }
            }
            walk(c);
        }
    };
    walk(root);
    return out;
}

// NCX：navPoint/navLabel/text + content/src
static std::map<std::string, std::string> toc_from_ncx(const std::string& xml,
                                                       const std::string& ncx_name) {
    std::map<std::string, std::string> out;
    bool ok = false;
    pugi::xml_document doc = parse_xml(xml, ok);
    if (!ok) return out;
    std::vector<pugi::xml_node> nav_points;
    find_all_local(doc, "navPoint", nav_points);
    for (const pugi::xml_node& np : nav_points) {
        pugi::xml_node label = find_first_local(np, "text");
        pugi::xml_node content = find_first_local(np, "content");
        if (!content || std::string(content.attribute("src").as_string()).empty())
            continue;
        std::string src = content.attribute("src").as_string();
        std::string target =
            posix_join_norm(posix_dirname(ncx_name),
                            url_unquote(src).substr(0, url_unquote(src).find('#')));
        if (label) {
            std::string text = strip_xml_text(xml_all_text(label));
            if (!text.empty()) out.emplace(std::move(target), std::move(text));
        }
    }
    return out;
}

// ---- 封面文件名猜测 ----
// 正则 (^|/)cover[^/]*\.(jpe?g|png|gif|webp|bmp)$ IGNORECASE 的手写等价
static bool cover_name_match(const std::string& name) {
    size_t base_start = name.find_last_of('/');
    std::string base = to_lower(base_start == std::string::npos ? name
                                                                : name.substr(base_start + 1));
    static const char* const exts[] = {".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp"};
    for (const char* e : exts) {
        std::string es(e);
        if (base.size() >= es.size() &&
            base.compare(base.size() - es.size(), es.size(), es) == 0) {
            // stem 必须以 "cover" 开头（对应 (^|/)cover[^/]*\.<ext>$）
            std::string stem = base.substr(0, base.size() - es.size());
            return stem.rfind("cover", 0) == 0;
        }
    }
    return false;
}

static std::optional<std::string> guess_cover(const std::vector<std::string>& image_names) {
    if (image_names.empty()) return std::nullopt;
    for (const std::string& n : image_names)
        if (cover_name_match(n)) return n;
    for (const std::string& n : image_names) {
        std::string base = to_lower(posix_basename(n));
        if (base.find("cover") != std::string::npos) return n;
    }
    return std::nullopt;
}

bool is_image_ext_epub(const std::string& ext) {
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".gif" ||
           ext == ".webp" || ext == ".bmp";
}

// 懒加载模型：容器 + OPF（meta 与 document 共用）
void EpubAdapter::load_model() {
    if (model_) return;
    Model m;
    // 手写 OPF/spine 路径（唯一路径）
    // Python 的判据是"container.xml / OPF 缺失或不可解析 → 失败"，**空 spine 是合法的**
    // （返回 200 + 空章节表）。因此这里记一个 opf_ok，只在 OPF 层面失败时才报错（审查 M5）。
    bool opf_ok = false;
    std::string container_xml;
    bool have_container = true;
    try {
        container_xml = read_entry("META-INF/container.xml");
    } catch (const AssetNotFoundError&) {
        have_container = false;
    }
    if (have_container) {
        bool ok = false;
        pugi::xml_document container = parse_xml(container_xml, ok);
        std::vector<pugi::xml_node> rootfiles;
        if (ok) find_all_local(container, "rootfile", rootfiles);
        std::string opf_path;
        if (!rootfiles.empty())
            opf_path = posix_normpath(rootfiles.front().attribute("full-path").as_string());
        if (!opf_path.empty()) {
            // 图片块的 asset 要按 Python 的口径给"OPF 相对路径"（Python 的 item_name 用的是
            // manifest href，如 text/ch1.xhtml，而不是容器路径 OEBPS/text/ch1.xhtml）：
            // 同一个 href 两边解析出的字符串因此不同，客户端会当成两个 URL（ETag 也随之不同）。
            opf_dir_ = posix_dirname(opf_path);
            std::string opf_bytes;
            bool have_opf = true;
            try {
                opf_bytes = read_entry(opf_path);
            } catch (const AssetNotFoundError&) {
                have_opf = false;
            }
            bool ok2 = false;
            pugi::xml_document opf = parse_xml(opf_bytes, ok2);
            if (have_opf && ok2) {
                opf_ok = true;  // OPF 已成功解析（空 spine 不算失败）
                // DC 元数据（localname 匹配）
                pugi::xml_node t = find_first_local(opf, "title");
                std::string title = t ? strip_xml_text(xml_all_text(t)) : "";
                m.title = title.empty() ? stem_of(ref_.name()) : title;
                pugi::xml_node c = find_first_local(opf, "creator");
                if (c) {
                    std::string a = strip_xml_text(xml_all_text(c));
                    if (!a.empty()) m.author = a;
                }
                pugi::xml_node l = find_first_local(opf, "language");
                if (l) {
                    std::string lg = strip_xml_text(xml_all_text(l));
                    if (!lg.empty()) m.language = lg;
                }
                // manifest：id → href/media-type/properties
                struct ItemInfo {
                    std::string href;
                    std::string media_type;
                    std::string properties;
                };
                std::map<std::string, ItemInfo> manifest;
                std::vector<pugi::xml_node> items;
                find_all_local(opf, "item", items);
                for (const pugi::xml_node& item : items) {
                    std::string id = item.attribute("id").as_string();
                    if (id.empty()) continue;
                    manifest[id] = {item.attribute("href").as_string(),
                                    item.attribute("media-type").as_string(),
                                    item.attribute("properties").as_string()};
                }
                // spine 顺序（仅 application/xhtml*）
                std::vector<pugi::xml_node> itemrefs;
                find_all_local(opf, "itemref", itemrefs);
                for (const pugi::xml_node& ir : itemrefs) {
                    std::string idref = ir.attribute("idref").as_string();
                    auto it = manifest.find(idref);
                    if (it == manifest.end()) continue;
                    if (it->second.media_type.rfind("application/xhtml", 0) != 0) continue;
                    std::string name = posix_join_norm(posix_dirname(opf_path),
                                                       url_unquote(it->second.href));
                    try {
                        Doc d;
                        d.name = name;
                        d.html = read_entry(name);
                        m.docs.push_back(std::move(d));
                    } catch (const AssetNotFoundError&) {
                        continue;
                    }
                }
                // 目录：EPUB3 nav 优先，其次 NCX
                std::map<std::string, std::string> toc_titles;
                for (const auto& [id, info] : manifest) {
                    // properties 含独立词 "nav"
                    std::string props = " " + info.properties + " ";
                    if (props.find(" nav ") == std::string::npos) continue;
                    std::string nav_name =
                        posix_join_norm(posix_dirname(opf_path), url_unquote(info.href));
                    try {
                        toc_titles = toc_from_nav(read_entry(nav_name), nav_name);
                    } catch (...) {
                    }
                    break;
                }
                if (toc_titles.empty()) {
                    for (const auto& [id, info] : manifest) {
                        if (info.media_type != "application/x-dtbncx+xml") continue;
                        std::string ncx_name = posix_join_norm(posix_dirname(opf_path),
                                                               url_unquote(info.href));
                        try {
                            toc_titles = toc_from_ncx(read_entry(ncx_name), ncx_name);
                        } catch (...) {
                        }
                        break;
                    }
                }
                for (Doc& d : m.docs) {
                    auto it = toc_titles.find(d.name);
                    if (it != toc_titles.end()) d.title = it->second;
                }
            }
        }
    }
    if (m.title.empty()) m.title = stem_of(ref_.name());
    if (!opf_ok) {
        mz_zip_reader_end(static_cast<mz_zip_archive*>(zip_));
        throw ParseError("EPUB 结构无法解析（缺少 OPF / spine）");
    }
    // 图片名（sorted() 普通字典序）
    for (const std::string& n : names_)
        if (is_image_ext_epub(ext_of(n))) m.image_names.push_back(n);
    std::sort(m.image_names.begin(), m.image_names.end());
    m.cover_name = guess_cover(m.image_names);
    model_ = std::move(m);
}

const EpubAdapter::Model& EpubAdapter::model() {
    load_model();
    return *model_;
}

AdapterMeta EpubAdapter::meta() {
    const Model& m = model();
    AdapterMeta meta;
    meta.title = m.title.empty() ? stem_of(ref_.name()) : m.title;
    meta.format = format;
    meta.mode = mode;  // "flow"
    meta.author = m.author;
    meta.language = m.language;
    meta.size = ref_.size;
    meta.page_count = std::nullopt;
    for (size_t i = 0; i < m.docs.size(); ++i) {
        const Doc& d = m.docs[i];
        std::string title = d.title.empty() ? stem_of(d.name) : d.title;
        meta.chapters.push_back(nlohmann::json{{"index", i},
                                               {"title", title},
                                               {"total_chars", nullptr}});
    }
    meta.supports_json = true;
    return meta;
}

// ---- HTML → 块 ----
// _image_block：src/data-src/xlink:href/属性名以 href 结尾 → image 块
std::optional<Block> epub_image_block(EpubAdapter& self, const HNode& tag,
                                      const std::string& item_name) {
    std::string src;
    auto get = [&](const char* k) -> std::string {
        auto it = tag.attrs.find(k);
        return it == tag.attrs.end() ? std::string() : it->second;
    };
    src = get("src");
    if (src.empty()) src = get("data-src");
    if (src.empty()) src = get("xlink:href");
    if (src.empty()) {
        // 属性名以 href 结尾（任意命名空间写法）
        for (const auto& [k, v] : tag.attrs) {
            if (k.size() >= 4 && k.compare(k.size() - 4, 4, "href") == 0) {
                src = v;
                break;
            }
        }
    }
    // strip
    size_t b = src.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::nullopt;
    size_t e = src.find_last_not_of(" \t\r\n");
    src = src.substr(b, e - b + 1);
    if (src.empty()) return std::nullopt;
    if (src.rfind("data:", 0) == 0) return std::nullopt;  // data: URI 无法经 /asset 提供
    std::string resolved = self.resolve_href_pub(item_name, src);
    if (resolved.empty()) return std::nullopt;
    auto [w, h] = self.image_size_pub(resolved);
    Block blk;
    blk.type = BlockType::Image;
    // 与 Python 一致：asset 用 OPF 相对路径（见 opf_relative 注释）
    blk.asset = self.opf_relative(resolved);
    blk.w = w;
    blk.h = h;
    return blk;
}

// 遍历（等价 find_all(True) 的文档序逐元素处理；跳过子树 = 逐元素 in_skipped 的等价）
void epub_walk(EpubAdapter& self, const HNode& n, const std::string& item_name,
               std::vector<Block>& blocks) {
    for (const HNode& c : n.children) {
        if (c.is_text) continue;
        if (is_skip_tag(c.name)) continue;  // 整棵子树跳过
        int lvl = heading_level_of(c.name);
        if (lvl) {
            if (has_block_descendant(c)) {
                // 含块级后代：只取直接文本
                append_direct_text(blocks, c);
            } else {
                std::vector<Span> spans;
                collect_spans(c, false, false, spans);
                tidy_spans(spans);
                U32Text text = spans_text(spans);
                bool any = false;
                for (char32_t ch : text)
                    if (ch != U' ') { any = true; break; }
                if (any) {
                    Block blk;
                    blk.type = BlockType::Heading;
                    blk.level = lvl;
                    blk.spans = std::move(spans);
                    blocks.push_back(std::move(blk));
                }
            }
        } else if (is_text_container(c.name)) {
            if (has_block_descendant(c)) {
                // 容器自己的直接文本也保留（顺序在子块之前）
                append_direct_text(blocks, c);
            } else {
                std::vector<Span> spans;
                collect_spans(c, false, false, spans);
                tidy_spans(spans);
                U32Text text = spans_text(spans);
                bool any = false;
                for (char32_t ch : text)
                    if (ch != U' ') { any = true; break; }
                if (any) {
                    Block blk;
                    blk.type = BlockType::Paragraph;
                    blk.spans = std::move(spans);
                    blocks.push_back(std::move(blk));
                }
            }
        } else if (c.name == "hr") {
            Block blk;
            blk.type = BlockType::Separator;
            blocks.push_back(std::move(blk));
        } else if (c.name == "img" || c.name == "image") {
            if (auto blk = epub_image_block(self, c, item_name)) {
                blocks.push_back(std::move(*blk));
            }
        }
        // 继续遍历后代（与 find_all(True) 一致）
        epub_walk(self, c, item_name, blocks);
    }
}

std::vector<Block> EpubAdapter::html_to_blocks(const std::string& html,
                                               const std::string& item_name) {
    HNode root = parse_html_tree(html);
    // 等价 soup.body if soup.body is not None else soup：找首个 body 元素
    const HNode* body = &root;
    std::function<const HNode*(const HNode&)> find_body = [&](const HNode& n) -> const HNode* {
        for (const HNode& c : n.children) {
            if (!c.is_text && c.name == "body") return &c;
            if (const HNode* r = find_body(c)) return r;
        }
        return nullptr;
    };
    if (const HNode* b = find_body(root)) body = b;
    std::vector<Block> blocks;
    epub_walk(*this, *body, item_name, blocks);
    return blocks;
}

std::pair<int, int> EpubAdapter::image_size(const std::string& name) {
    auto it = size_cache_.find(name);
    if (it != size_cache_.end()) return it->second;
    std::pair<int, int> size{600, 800};  // probe 失败默认
    try {
        std::string data = read_entry(name);
        auto probed = probe_size(data);
        if (probed.first > 0 && probed.second > 0) size = probed;
    } catch (...) {
    }
    size_cache_[name] = size;
    return size;
}

// 供 epub_image_block / epub_walk 使用的公开转发（定义于类外辅助函数）
std::string EpubAdapter::resolve_href_pub(const std::string& base, const std::string& href) {
    return resolve_href(base, href);
}

// 容器路径 → OPF 相对路径：把 OPF 目录前缀去掉（Python 的 href 解析基准是 manifest href）。
// 前缀不匹配时原样返回（例如 href 用绝对路径 / 或跨越 OPF 目录的极端写法）。
std::string EpubAdapter::opf_relative(const std::string& container_path) const {
    if (opf_dir_.empty() || opf_dir_ == ".") return container_path;
    std::string prefix = opf_dir_ + "/";
    if (container_path.rfind(prefix, 0) == 0)
        return container_path.substr(prefix.size());
    return container_path;
}
std::pair<int, int> EpubAdapter::image_size_pub(const std::string& name) {
    return image_size(name);
}

DocumentModel EpubAdapter::document() {
    const Model& m = model();
    DocumentModel doc;
    doc.book_id = ref_.book_id;
    doc.format = format;
    size_t total_chars = 0;
    for (size_t i = 0; i < m.docs.size(); ++i) {
        const Doc& d = m.docs[i];
        Chapter ch;
        ch.index = int(i);
        ch.blocks = html_to_blocks(d.html, d.name);
        // 章标题：toc → 首个非空 heading → 文件名 stem
        U32Text title = utf8_to_u32(d.title);
        if (title.empty()) {
            for (const Block& b : ch.blocks) {
                if (b.type != BlockType::Heading) continue;
                U32Text t = b.text();
                bool any = false;
                for (char32_t c2 : t)
                    if (c2 != U' ') { any = true; break; }
                if (any) { title = t; break; }
            }
        }
        if (title.empty()) title = utf8_to_u32(stem_of(d.name));
        ch.title = title;
        total_chars += ch.total_chars();
        doc.chapters.push_back(std::move(ch));
    }
    doc.meta["title"] = m.title.empty() ? stem_of(ref_.name()) : m.title;
    doc.meta["author"] = m.author ? nlohmann::json(*m.author) : nlohmann::json(nullptr);
    doc.meta["language"] =
        m.language ? nlohmann::json(*m.language) : nlohmann::json(nullptr);
    doc.meta["total_chars"] = total_chars;
    doc.meta["source"] = ref_.vpath;
    doc.meta["parser"] = "zip+opf";
    return doc;
}

ImageBytes EpubAdapter::asset(const std::string& path) {
    // posixpath.normpath(path.replace("\\","/").lstrip("/"))
    std::string norm = path;
    for (char& c : norm)
        if (c == '\\') c = '/';
    size_t p = norm.find_first_not_of('/');
    norm = p == std::string::npos ? std::string() : norm.substr(p);
    norm = posix_normpath(norm);
    std::string data = read_entry(norm);
    std::string mime = sniff_mime(data);
    if (mime == "application/octet-stream") mime = mime_from_ext(ext_of(norm));
    return ImageBytes{std::move(data), std::move(mime)};
}

std::optional<ImageBytes> EpubAdapter::cover(int width) {
    const Model& m = model();
    std::optional<std::string> name = m.cover_name;
    if (!name && !m.image_names.empty()) name = m.image_names.front();  // 任何图片第一个
    if (name) {
        try {
            std::string data = read_entry(*name);
            if (width <= 0) {
                std::string mime = sniff_mime(data);
                if (mime == "application/octet-stream") mime = mime_from_ext(ext_of(*name));
                return ImageBytes{std::move(data), std::move(mime)};
            }
            try {
                ImageBuf img = decode_image(data, cfg_.limits.max_source_pixels);
                if (img.w > width) img = resize_image(img, width, 0);
                return encode_image(img, EncodeOpts{.prefer_jpeg = true, .jpeg_quality = 84});
            } catch (...) {
                // 解码失败退原图
                std::string mime = sniff_mime(data);
                if (mime == "application/octet-stream") mime = mime_from_ext(ext_of(*name));
                return ImageBytes{std::move(data), std::move(mime)};
            }
        } catch (const AssetNotFoundError&) {
            // 落到 nullopt
        }
    }
    return std::nullopt;  // 无封面 → 服务层退回文字封面
}

}  // namespace lnv

namespace lnv {


std::unique_ptr<BookAdapter> create_epub_adapter(const AppConfig& cfg, const BookRef& ref) {
    return std::make_unique<EpubAdapter>(cfg, ref);
}

}  // namespace lnv
