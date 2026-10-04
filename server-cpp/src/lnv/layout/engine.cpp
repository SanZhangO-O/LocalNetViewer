// 分页引擎与锚点定位（对应 Python layout/engine.py、layout/anchors.py，§4.4/4.5）
// 逐条对齐 Python 语义：惰性折行、token 边界、避头尾、断页与锚点推进。
#include "lnv/layout/engine.h"

#include <cmath>
#include <limits>

#include "lnv/util/sha1.h"

namespace lnv {
namespace {

// ================================================================ 常量
constexpr double EPS = 0.01;                 // 宽度比较容差
constexpr long MAX_PAGES = 500000;           // 页数防御上限
constexpr size_t LONG_BLOCK_CHARS = 200000;  // 超长块分段长度
constexpr size_t MAX_TOKEN_CHARS = 1024;     // token 按绝对位置强制断开的间隔
constexpr size_t TOKEN_MEASURE_EXACT_MAX = 256;  // 超过此长度只量到超限为止
constexpr double SEPARATOR_RATIO = 1.6;      // separator 高 = 正文行高 × 1.6

// 标题字号相对正文字号的倍率
double heading_ratio(int level) {
    static constexpr double kRatios[] = {1.8, 1.5, 1.3, 1.15, 1.05, 1.0};  // level 1..6
    level = std::max(1, std::min(6, level));
    return kRatios[level - 1];
}

// 行首禁则字符集（避头尾，P2）。必须与 engine.py:40 的 _NO_LINE_START 逐字一致：
// 里面是 U+201D/U+2019（中文弯引号），不是 ASCII 的 " 和 ' —— 写成 ASCII 会让
// "…很好。”" 这类句尾引号被推到下一行，折行位置与 Python 不同 → 页数/锚点漂移。
bool is_no_line_start(char32_t c) {
    static const U32Text kSet = utf8_to_u32(
        "。，、！？：；）」』】》〉”’…—·%!?,.:;)]}>");
    return kSet.find(c) != U32Text::npos;
}

// ---------------------------------------------------------------- Python 数值习惯
// round(x, n)（近似二进制 half-even）；n=0 时返回整数值
double py_round(double v, int ndigits = 2) {
    double f = std::pow(10.0, ndigits);
    return std::nearbyint(v * f) / f;
}
int py_round0(double v) {
    return static_cast<int>(std::lround(std::nearbyint(v)));
}

// ---------------------------------------------------------------- 空白判定（str.strip/isspace 语义）
bool is_py_ws(char32_t c) {
    switch (c) {
        case U'\t': case U'\n': case U'\v': case U'\f': case U'\r':
        case U' ': case U'\x85': case U'\xA0': case U'\x1680':
        case U'\x2028': case U'\x2029': case U'\x202F': case U'\x205F': case U'\x3000':
            return true;
        default:
            return c >= 0x2000 && c <= 0x200A;
    }
}
U32Text rstrip_ws(const U32Text& t) {
    size_t e = t.size();
    while (e > 0 && is_py_ws(t[e - 1])) --e;
    return t.substr(0, e);
}
bool all_ws(const U32Text& t) {
    for (char32_t c : t)
        if (!is_py_ws(c)) return false;
    return true;
}

// ---------------------------------------------------------------- 锚点比较
int cmp_anchor3(const int a[3], const int b[3]) {
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

// ================================================================ 分词
struct Tok {
    U32Text text;
    size_t s = 0, e = 0;  // 段内相对偏移
};

// 惰性分词：空白串（空格/tab）一个 token；is_wide_char 单字一个；
// 其余连续非空白为一个 token，按**绝对位置**每 1024 字符强制断开（保证翻页续排
// 与顺序排版 token 边界一致）。offsets 为 [begin,end) 段内相对偏移。
struct TokenStream {
    const U32Text* text = nullptr;
    size_t begin = 0, end = 0, i = 0;
    // MAX_TOKEN_CHARS 分块的坐标原点：0 = 块内绝对（Python iter_lines(text, start)
    // 直连路径），超长块分段时为段起点（Python _iter_block_lines 把段切成新字符串，
    // 段内偏移从 0 起算）。两者在段起点不是 1024 倍数时给出不同的 token 边界。
    size_t chunk_base = 0;

    TokenStream(const U32Text& t, size_t b, size_t e, size_t cb = 0)
        : text(&t), begin(b), end(e), i(b), chunk_base(cb) {}

    bool next(Tok& out) {
        while (i < end) {
            char32_t ch = (*text)[i];
            if (ch == U' ' || ch == U'\t') {
                size_t j = i;
                while (j < end && ((*text)[j] == U' ' || (*text)[j] == U'\t')) ++j;
                out = {text->substr(i, j - i), i - begin, j - begin};
                i = j;
                return true;
            }
            if (is_wide_char(ch)) {
                out = {U32Text(1, ch), i - begin, i + 1 - begin};
                ++i;
                return true;
            }
            // 按**绝对位置**分块，而不是"从 start 起每 MAX_TOKEN_CHARS 个字符"：
            // 否则"从块中间恢复排版"与"从头顺序排版"会折出不同的行
            size_t rel = i - chunk_base;
            size_t chunk_end = chunk_base + rel - (rel % MAX_TOKEN_CHARS) + MAX_TOKEN_CHARS;
            size_t j = i;
            while (j < end && j < chunk_end && (*text)[j] != U' ' && (*text)[j] != U'\t' &&
                   !is_wide_char((*text)[j]))
                ++j;
            out = {text->substr(i, j - i), i - begin, j - begin};
            i = j;
            return true;
        }
        return false;
    }
};

// LineBreaker 的占位文本：has_breaker=false 时不会被读。绝不能写
// *(const U32Text*)nullptr 来构造空引用（即使不解引用也是 UB），
// 用一个真实空串的引用即可。
const U32Text kEmptyBreakerText;

}  // namespace

// ================================================================ 引擎实现
struct FlowLayoutEngine::Impl {
    Deps d;
    double avail_w = 0, avail_h = 0;
    std::vector<int> loaded_sizes;  // 已 load 的字号（字体度量按字号加载）

    explicit Impl(Deps dep) : d(std::move(dep)) {
        avail_w = static_cast<double>(d.profile->avail_width());
        avail_h = static_cast<double>(d.profile->avail_height());
    }

    int base_size() const { return d.profile->font.size_px; }
    double spacing() const { return d.profile->font.letter_spacing_px; }
    double line_ratio() const { return d.profile->font.line_height; }
    int indent_chars() const { return d.profile->paragraph.first_line_indent_chars; }

    // 标题字号：倍率取整，≥8
    int heading_size(int level) const {
        return std::max(8, py_round0(base_size() * heading_ratio(level)));
    }

    // 字号对应的字体必须先 load（heading 与正文可能不同字号）
    void ensure_font(int size) {
        for (int s : loaded_sizes)
            if (s == size) return;
        d.fonts->load(d.font_id, size);
        loaded_sizes.push_back(size);
    }

    // 单字符宽度：宽字符恒 = 字号 + letter_spacing（查表，不走字体）；
    // 其余 = FreeType advance + spacing。
    // 走 char_length_at（一次调用内完成"加载该字号 + 测量"）：早期实现用
    // ensure_font(size) 只检查"本引擎是否加载过该字号"，而 FontManager 内部
    // 只保留**一个** face，于是标题字号（158）之后量正文（88）会拿到 158 的宽度，
    // 折行随调用顺序变化 —— 分页（build）与逐页渲染（items_for_page）算出的
    // 行数不一致，页锚点与页内容对不上，正文会被整段跳过。
    double char_width(char32_t ch, int size) {
        if (is_wide_char(ch)) return size + spacing();
        return d.fonts->char_length_at(d.font_id, size, ch) + spacing();
    }

    // token 宽度；返回 (宽度, 是否精确)。长 token（>256 字符）只量到超过 limit
    // 为止——"只是下界"这一事实必须告诉调用方：避头尾的 8% 容忍度依赖精确宽度。
    std::pair<double, bool> token_width(const U32Text& tok, int size, double limit) {
        if (tok.size() <= TOKEN_MEASURE_EXACT_MAX) {
            double w = 0;
            for (char32_t c : tok) w += char_width(c, size);
            return {w, true};
        }
        double w = 0;
        for (char32_t c : tok) {
            w += char_width(c, size);
            if (w > limit) return {w, false};
        }
        return {w, true};
    }

    // 超宽单词（英文/URL）做字符级强制断行；返回 (片段, 消费字符数, 片段宽)
    struct HardSplit {
        U32Text piece;
        size_t taken = 0;
        double width = 0;
    };
    HardSplit hard_split(const U32Text& tok, double limit, int size) {
        HardSplit hs;
        double w = 0;
        size_t take = 0;
        for (char32_t ch : tok) {
            double cw = char_width(ch, size);
            if (take > 0 && w + cw > limit + EPS) break;
            w += cw;
            ++take;
        }
        take = std::max<size_t>(1, std::min(take, tok.size()));
        double width = 0;
        for (size_t i = 0; i < take; ++i) width += char_width(tok[i], size);
        hs.piece = tok.substr(0, take);
        hs.taken = take;
        hs.width = width;
        return hs;
    }

    // ------------------------------------------------ 单段贪心折行（iter_lines）
    // 惰性版本：产出一行就可以立刻排版。单槽推回缓冲保存"断点之前的 token"。
    struct LineBreaker {
        Impl* E;
        const U32Text* text;   // 完整块文本
        size_t seg_base = 0;   // 段起点（块内绝对位置）
        size_t seg_end = 0;
        int size = 0;
        int indent_chars = 0;
        double indent_px = 0;
        bool emit_empty = true;

        TokenStream source{nullptr, 0, 0};
        bool source_init = false;
        bool has_pending = false;
        Tok pending;
        bool first_line = true;
        int produced = 0;
        bool done = false;

        LineBreaker(Impl* e, const U32Text& t, size_t base, size_t end, int sz, int ind,
                    bool emit, size_t chunk_base = 0)
            : E(e), text(&t), seg_base(base), seg_end(end), size(sz), indent_chars(ind),
              indent_px(ind ? ind * static_cast<double>(sz) : 0.0), emit_empty(emit),
              source(t, base, end, chunk_base) {}

        bool next_token(Tok& out) {
            if (has_pending) {
                out = pending;
                has_pending = false;
                return true;
            }
            return source.next(out);
        }
        void push_back(const Tok& t) {
            pending = t;
            has_pending = true;
        }

        // 产出一行：行文本（含 \u3000 缩进前缀）+ 块内绝对 [start,end) 偏移
        bool next(U32Text& out, size_t& ls, size_t& le) {
            if (done) return false;
            while (true) {
                Tok first;
                if (!next_token(first)) {
                    done = true;
                    break;
                }
                double limit = E->avail_w - (first_line ? indent_px : 0.0);
                std::vector<U32Text> parts;
                double width = 0;
                size_t line_start = seg_base + first.s;
                size_t line_end = line_start;
                bool pushed = false;  // 内层循环是否以"推回"结束
                Tok token = first;
                while (true) {
                    if (parts.empty() && all_ws(token.text)) {
                        // 块的第一行保留原始起点作为锚点（保证页首锚点 ≤ 页内任何
                        // 位置）；续行则把锚点推到第一个非空白字符
                        if (!first_line) line_start = seg_base + token.e;
                        line_end = seg_base + token.e;
                        if (!next_token(token)) break;
                        continue;
                    }
                    auto [tw, exact] = E->token_width(token.text, size, limit);
                    if (!parts.empty() && width + tw > limit + EPS) {
                        // 避头尾容忍度依赖**精确**宽度：长 token 的 tw 只是下界
                        if (exact && !token.text.empty() &&
                            is_no_line_start(token.text[0]) && width + tw <= limit * 1.08) {
                            // 避头尾：允许 8% 溢出
                        } else {
                            push_back(token);
                            pushed = true;
                            break;
                        }
                    }
                    if (parts.empty() && tw > limit + EPS) {
                        auto hs = E->hard_split(token.text, limit, size);
                        parts.push_back(hs.piece);
                        width += hs.width;
                        line_end = seg_base + token.s + hs.taken;
                        if (hs.taken < token.text.size()) {
                            // 推回剩余部分：第三项必须是原 token 的**结束偏移** e
                            // （Python engine.py:483 push_back((tok_text[taken:],
                            // ts + taken, te))）；写成 token.e - token.s（长度）会让
                            // 续行的 line_start/line_end 变小，页首锚点随之偏移。
                            push_back(Tok{token.text.substr(hs.taken),
                                          token.s + hs.taken, token.e});
                            pushed = true;
                        }
                        break;
                    }
                    parts.push_back(token.text);
                    width += tw;
                    line_end = seg_base + token.e;
                    if (!next_token(token)) break;
                }
                U32Text raw = rstrip_ws(join(parts));
                if (parts.empty() && !pushed && !has_pending) {
                    // 末尾只剩空白：不要为它产出一整行（会白白占一行高度）
                    done = true;
                    break;
                }
                U32Text prefix;
                if (first_line && indent_chars) prefix.assign(indent_chars, U'\u3000');
                out = prefix + raw;
                ls = line_start;
                le = line_end;
                ++produced;
                first_line = false;
                // 终止性：每轮至少消费一个 token（first 一定被消费；推回只发生在
                // parts 非空之后），因此不需要额外的进展计数器
                return true;
            }
            // 整段没有任何可见内容：首段补一个缩进占位空行（后续段不补，否则会
            // 产出零长度文本项、白占一行高度还可能让分页停在那里）
            if (!produced && emit_empty) {
                out.assign(std::max(0, indent_chars), U'\u3000');
                ls = le = seg_base;
                ++produced;
                return true;
            }
            return false;
        }

        static U32Text join(const std::vector<U32Text>& vs) {
            size_t n = 0;
            for (auto& v : vs) n += v.size();
            U32Text r;
            r.reserve(n);
            for (auto& v : vs) r += v;
            return r;
        }
    };

    // ------------------------------------------------ 跨分段折行（_iter_block_lines）
    // 超长块（>200000 字符）分段折行；偏移量还原为块内绝对位置。
    struct BlockLineStream {
        Impl* E;
        const U32Text* text = nullptr;
        size_t offset = 0;
        int size = 0;
        int indent_chars = 0;

        size_t pos = 0, total = 0;
        bool first_segment = true;
        bool has_breaker = false;
        LineBreaker cur{nullptr, kEmptyBreakerText, 0, 0, 0, 0, true};

        explicit BlockLineStream(Impl* e, const U32Text& t, size_t off, int sz, int ind)
            : E(e), text(&t), offset(off), size(sz), indent_chars(ind) {
            pos = off < t.size() ? off : t.size();
            total = t.size();
        }

        bool next(U32Text& out, size_t& s, size_t& e) {
            while (pos < total || has_breaker) {
                if (!has_breaker) {
                    size_t end = std::min(total, pos + LONG_BLOCK_CHARS);
                    // 只有第一段才给缩进 / 允许"整段是空白"时补占位空行；
                    // chunk_base = pos：超长块分段时 token 分块坐标从段首起算
                    cur = LineBreaker(E, *text, pos, end, size,
                                      first_segment ? indent_chars : 0, first_segment, pos);
                    has_breaker = true;
                }
                U32Text line;
                size_t ls = 0, le = 0;
                if (cur.next(line, ls, le)) {
                    out = line;
                    s = ls;  // LineBreaker 已给出块内绝对偏移
                    e = le;
                    return true;
                }
                has_breaker = false;
                first_segment = false;
                pos = cur.seg_end;
            }
            return false;
        }
    };

    // 文本块条目参数（对应 Python _text_items 的前段计算）
    struct BlockStyle {
        bool is_heading = false;
        int size = 0;
        int line_h2 = 0;  // round(字号×line_height,2)（以 1/100 存，避免 double 比较）
        double line_h = 0;
        int indent_chars = 0;
        bool bold = false, italic = false;
        double spacing_after = 0;
        int level = 0;
    };
    BlockStyle block_style(const Block& blk, size_t offset) {
        BlockStyle st;
        st.is_heading = blk.type == BlockType::Heading;
        st.size = st.is_heading ? heading_size(blk.level) : base_size();
        st.line_h = py_round(st.size * line_ratio(), 2);
        // 段首缩进只在"从块开头排版"时给出：翻页后从块中间继续排版时不能再缩进
        st.indent_chars =
            st.is_heading ? 0 : (offset == 0 ? indent_chars() : 0);
        bool any_bold = false, any_italic = false;
        for (auto& sp : blk.spans) {
            if (sp.style == SpanStyle::Bold || sp.style == SpanStyle::BoldItalic) any_bold = true;
            if (sp.style == SpanStyle::Italic || sp.style == SpanStyle::BoldItalic) any_italic = true;
        }
        st.bold = st.is_heading || any_bold;
        st.italic = !st.is_heading && any_italic;
        st.spacing_after = st.is_heading ? 0.0 : d.profile->paragraph.spacing_px;
        st.level = st.is_heading ? blk.level : 0;
        return st;
    }

    const Chapter* chapter(int ci) const {
        if (ci < 0 || ci >= static_cast<int>(d.doc->chapters.size())) return nullptr;
        return &d.doc->chapters[ci];
    }

    std::array<int, 3> doc_end_anchor() const {
        if (d.doc->chapters.empty()) return {0, 0, 0};
        int last_ch = static_cast<int>(d.doc->chapters.size()) - 1;
        int last_block = std::max(0, static_cast<int>(d.doc->chapters[last_ch].blocks.size()) - 1);
        return {last_ch, last_block + 1, 0};
    }

    // 单个条目能否放进当前页；不能则置断点锚。
    // Python engine.py:563 还有一条"height > avail_h 也换页"，但 used ≥ 0，
    // used+height > avail_h 已将其完全覆盖（死分支，原样保留只会误导读者）。
    bool fits(double used, double height, bool page_has_items,
              const std::array<int, 3>& anchor, std::optional<std::array<int, 3>>& pending) {
        if (page_has_items && used + height > avail_h + EPS) {
            pending = anchor;
            return false;
        }
        return true;
    }
};

// ---------------------------------------------------------------- 构造/析构
FlowLayoutEngine::FlowLayoutEngine(Deps d) : impl_(std::make_unique<Impl>(std::move(d))) {}
FlowLayoutEngine::~FlowLayoutEngine() = default;

// ---------------------------------------------------------------- 单页排版
std::optional<FlowLayoutEngine::PageResult> FlowLayoutEngine::layout_page(const int anchor[3]) {
    PageResult res;
    PageRecord& rec = res.record;
    rec.anchor[0] = anchor[0]; rec.anchor[1] = anchor[1]; rec.anchor[2] = anchor[2];

    Impl& E = *impl_;
    double used = 0;
    std::optional<std::array<int, 3>> pending_anchor;
    int cur_chapter = -1;
    bool broke = false;
    const auto& chapters = E.d.doc->chapters;

    for (int ci = std::max(0, anchor[0]); ci < static_cast<int>(chapters.size()) && !broke; ++ci) {
        const Chapter& ch = chapters[ci];
        // 章起始标记：不占高度，仅用于 chapter_new_page 切页
        if (E.d.profile->chapter_new_page && !res.items.empty() && ci != cur_chapter) {
            pending_anchor = std::array<int,3>{ci, 0, 0};
            broke = true;
            break;
        }
        if (cur_chapter < 0) {
            cur_chapter = ci;
            rec.chapter_index = ci;
            rec.chapter_title = ch.title;
        }
        for (int bi = (ci == anchor[0] ? std::max(0, anchor[1]) : 0);
             bi < static_cast<int>(ch.blocks.size()) && !broke; ++bi) {
            const Block& blk = ch.blocks[bi];
            const bool first_blk = (ci == anchor[0] && bi == anchor[1]);

            if (blk.type == BlockType::Image) {
                // 图片：显示尺寸缩入 avail×ratio，且不超过整页可用高
                double w = blk.w > 0 ? blk.w : 0;
                double h = blk.h > 0 ? blk.h : 0;
                if (w <= 0 || h <= 0) { w = 600.0; h = 800.0; }
                double max_w = E.avail_w * E.d.profile->image.max_width_ratio;
                double max_h = E.avail_h * E.d.profile->image.max_height_ratio;
                double ratio = (w > max_w || h > max_h) ? std::min(max_w / w, max_h / h) : 1.0;
                double dw = std::max(1.0, w * ratio);
                double dh = std::max(1.0, h * ratio);
                if (dh > E.avail_h) {  // 永远排不下的图片按整页高缩
                    double shrink = E.avail_h / dh;
                    dh *= shrink;
                    dw *= shrink;
                }
                LayoutItem it;
                it.kind = LayoutItem::Kind::Image;
                it.height = py_round(dh, 2);
                it.space_after = E.d.profile->paragraph.spacing_px;
                it.chapter = ci;
                it.block = bi;
                it.image_block = &blk;
                it.display_w = dw;
                it.display_h = dh;
                if (!E.fits(used, it.height + it.space_after, !res.items.empty(), {ci, bi, 0},
                            pending_anchor)) {
                    broke = true;
                    break;
                }
                it.y = used;
                if (cur_chapter < 0) {
                    cur_chapter = ci;
                    rec.chapter_index = ci;
                    rec.chapter_title = ch.title;
                }
                used += it.height + it.space_after;
                res.items.push_back(std::move(it));
                continue;
            }
            if (blk.type == BlockType::Separator) {
                LayoutItem it;
                it.kind = LayoutItem::Kind::Separator;
                it.height = py_round(E.base_size() * E.line_ratio() * SEPARATOR_RATIO, 2);
                it.space_after = E.d.profile->paragraph.spacing_px;
                it.chapter = ci;
                it.block = bi;
                if (!E.fits(used, it.height + it.space_after, !res.items.empty(), {ci, bi, 0},
                            pending_anchor)) {
                    broke = true;
                    break;
                }
                it.y = used;
                if (cur_chapter < 0) {
                    cur_chapter = ci;
                    rec.chapter_index = ci;
                    rec.chapter_title = ch.title;
                }
                used += it.height + it.space_after;
                res.items.push_back(std::move(it));
                continue;
            }
            if (blk.type != BlockType::Heading && blk.type != BlockType::Paragraph) continue;

            U32Text text = blk.text();
            size_t offset = first_blk ? static_cast<size_t>(std::max(0, anchor[2])) : 0;
            if (offset >= text.size()) continue;

            Impl::BlockStyle st = E.block_style(blk, offset);
            Impl::BlockLineStream ls(&E, text, offset, st.size, st.indent_chars);

            // 一行前瞻：只有"整块的最后一行"才带段后间距
            U32Text line_text;
            size_t c_start = 0, c_end = 0;
            if (!ls.next(line_text, c_start, c_end)) continue;
            auto build_item = [&](const U32Text& txt, size_t cs, double space) {
                LayoutItem it;
                it.kind = LayoutItem::Kind::TextLine;
                it.height = st.line_h;
                it.space_after = space;
                it.chapter = ci;
                it.block = bi;
                it.level = st.level;
                it.bold = st.bold;
                it.italic = st.italic;
                it.font_size = st.size;
                it.line_text = txt;
                // 锚点 char 是 int：>2^31 的块内偏移理论上不会出现（约 86 亿字符），
                // 但直接 static_cast 收窄在越界时是回绕/UB；钳到 INT_MAX 保证
                // 锚点单调不减（配合 build() 的锚点前进防御）
                it.char_offset =
                    cs > static_cast<size_t>(std::numeric_limits<int>::max())
                        ? std::numeric_limits<int>::max()
                        : static_cast<int>(cs);
                return it;
            };
            while (true) {
                U32Text nxt_text;
                size_t n_start = 0, n_end = 0;
                bool has_next = ls.next(nxt_text, n_start, n_end);
                double space = has_next ? 0.0 : st.spacing_after;
                LayoutItem it = build_item(line_text, c_start, space);
                if (!E.fits(used, it.height + space, !res.items.empty(), {ci, bi, it.char_offset},
                            pending_anchor)) {
                    broke = true;
                    break;
                }
                it.y = used;
                if (cur_chapter < 0) {
                    cur_chapter = ci;
                    rec.chapter_index = ci;
                    rec.chapter_title = ch.title;
                }
                used += it.height + space;
                res.items.push_back(std::move(it));
                if (!has_next) break;
                line_text = std::move(nxt_text);
                c_start = n_start;
                c_end = n_end;
            }
        }
    }

    if (!res.items.empty()) {
        const LayoutItem& first = res.items[0];
        rec.anchor[0] = first.chapter;
        rec.anchor[1] = first.block;
        rec.anchor[2] = first.kind == LayoutItem::Kind::TextLine ? first.char_offset : 0;
        if (first.kind == LayoutItem::Kind::TextLine) rec.first_line = first.line_text;
        // 章节信息以首个条目所属章节为准（章标记可能被合并到上一页）
        rec.chapter_index = first.chapter;
        if (const Chapter* c = E.chapter(first.chapter)) rec.chapter_title = c->title;
    } else if (!pending_anchor) {
        std::array<int, 3> da = E.doc_end_anchor();
        rec.anchor[0] = da[0]; rec.anchor[1] = da[1]; rec.anchor[2] = da[2];
    }
    res.next_anchor = pending_anchor;
    return res;
}

// ---------------------------------------------------------------- 全量构建
PageMap FlowLayoutEngine::build() {
    Impl& E = *impl_;
    std::vector<PageRecord> pages;
    std::map<int, int> chapter_starts;
    std::array<int, 3> anchor{0, 0, 0};
    std::optional<std::array<int, 3>> last_next;
    long guard = 0;

    while (true) {
        if (++guard > MAX_PAGES) {
            throw ParseError("分页失败：页数超过上限（" + std::to_string(MAX_PAGES) + "）");
        }
        auto laid = layout_page(anchor.data());
        if (!laid) break;
        if (!laid->items.empty()) {
            PageRecord r = laid->record;
            r.page = static_cast<int>(pages.size()) + 1;
            pages.push_back(r);
            // 章起始页按"该章第一个条目落在哪一页"记录：
            // chapter_new_page=false 时一章可能从页中间开始，只看页首章节会漏记
            for (auto& it : laid->items)
                chapter_starts.emplace(it.chapter, r.page);
        }
        if (!laid->next_anchor) {
            last_next.reset();
            break;
        }
        std::array<int, 3> nxt = *laid->next_anchor;
        std::array<int, 3> cur = {anchor[0], anchor[1], anchor[2]};
        if (cmp_anchor3(nxt.data(), cur.data()) <= 0) {
            // 防御：锚点必须前进，否则结束（避免死循环）
            last_next.reset();
            break;
        }
        last_next = nxt;
        anchor = nxt;
    }

    PageMap pm;
    pm.mode = "flow";
    pm.total_pages = static_cast<int>(pages.size());
    pm.pages = std::move(pages);
    pm.layout_id = E.d.profile->layout_id();
    pm.book_id = E.d.doc->book_id;
    for (const auto& ch : E.d.doc->chapters) {
        PageMapChapter c;
        c.index = ch.index;
        c.title = ch.title;
        c.total_chars = static_cast<int>(ch.total_chars());
        auto it = chapter_starts.find(ch.index);
        if (it != chapter_starts.end()) c.start_page = it->second;
        pm.chapters.push_back(std::move(c));
    }
    std::array<int, 3> end = E.doc_end_anchor();
    if (last_next) end = *last_next;
    pm.end_anchor[0] = end[0]; pm.end_anchor[1] = end[1]; pm.end_anchor[2] = end[2];
    pm.created_at = 0.0;
    return pm;
}

// ---------------------------------------------------------------- 重建页条目
std::optional<FlowLayoutEngine::PageResult> FlowLayoutEngine::items_for_page(const PageMap& pm,
                                                                             int page) {
    const PageRecord* rec = pm.record(page);
    if (!rec) return std::nullopt;
    return layout_page(rec->anchor);
}

// ---------------------------------------------------------------- layout_key
std::string compute_layout_key(const LayoutProfile& profile,
                               const std::string& resolved_font_id) {
    nlohmann::json j = {
        {"engine", LAYOUT_ENGINE_VERSION},   // "1"
        {"font", resolved_font_id},          // 已解析的字体 id（"auto" 解析为实际字体）
        {"geometry", profile.geometry_json()},
        {"parser", "1"},
    };
    // nlohmann 对象自动按键排序；dump() 无空白 → sha1 前 12 hex → 前缀 "L"
    std::string s = j.dump();
    Sha1 h;
    h.update(s.data(), s.size());
    return "L" + h.hexdigest().substr(0, 12);
}

// ---------------------------------------------------------------- 锚点定位（§4.5）
namespace {

// 返回 (页码, 是否越界收敛)
std::pair<int, bool> page_of_anchor(const PageMap& pm, const int target[3]) {
    const auto& pages = pm.pages;
    if (pages.empty()) return {1, true};
    if (cmp_anchor3(target, pages.front().anchor) < 0)
        return {pages.front().page, cmp_anchor3(target, pages.front().anchor) != 0};
    if (cmp_anchor3(target, pm.end_anchor) >= 0) {
        // 越过文档末尾（内容已变化 / 进度来自旧版本）→ 收敛到最后一页
        return {pages.back().page, true};
    }
    // 二分查找"最后一个 anchor ≤ target"的页
    size_t lo = 0, hi = pages.size() - 1;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (cmp_anchor3(pages[mid].anchor, target) <= 0)
            lo = mid;
        else
            hi = mid - 1;
    }
    return {pages[lo].page, false};
}

}  // namespace

LocateResult locate(const PageMap& pm, const int target[3]) {
    auto [page, clamped] = page_of_anchor(pm, target);
    LocateResult r;
    r.page = page;
    r.total_pages = pm.total_pages;
    r.clamped = clamped;
    int total = std::max(1, pm.total_pages);
    r.ratio = std::min(1.0, std::max(0.0, (page - 0.5) / static_cast<double>(total)));

    const PageRecord* rec = pm.record(page);
    r.chapter_index = rec ? rec->chapter_index : 0;
    // 章节以"定位到的那一页"为准（与 /pages/{n}/meta 一致），标题优先取章节表
    bool title_found = false;
    for (const auto& ch : pm.chapters) {
        if (ch.index == r.chapter_index) {
            r.chapter_title = ch.title;
            title_found = true;
            break;
        }
    }
    if (!title_found && rec) r.chapter_title = rec->chapter_title;

    // 锚点自身所属章节：只在文档内且章节表里确实有这一章时才给 target.chapter，
    // 否则回落到定位页章节
    int anchor_chapter = r.chapter_index;
    if (!clamped) {
        for (const auto& ch : pm.chapters) {
            if (ch.index == target[0]) {
                anchor_chapter = target[0];
                break;
            }
        }
    }
    r.anchor_chapter_index = anchor_chapter;
    return r;
}

}  // namespace lnv
