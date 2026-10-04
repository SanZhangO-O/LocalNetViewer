// 页面渲染实现（对应 Python render/raster.py、render/jsonout.py，规格 §4.4/§4.7）
// 说明：C++ LayoutItem 没有 y 字段（Python 条目自带 y），这里按条目顺序累计
//   y += height + space_after 得到同一布局语义（与 engine 的纵向累加一致）。
#include "lnv/render/raster.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "lnv/util/sha1.h"

namespace lnv {

namespace {

// ============================================================ 主题与颜色
struct Rgb {
    int r = 0, g = 0, b = 0;
};

struct Theme {
    Rgb bg;
    Rgb fg;
    Rgb muted;  // blend(fg, bg, 0.45) 线性
};

Rgb hex_to_rgb(const char* v) {
    // "RRGGBB" → RGB
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    return Rgb{nib(v[0]) * 16 + nib(v[1]), nib(v[2]) * 16 + nib(v[3]),
               nib(v[4]) * 16 + nib(v[5])};
}

Rgb blend(const Rgb& a, const Rgb& b, double ratio) {
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    auto ch = [ratio](int x, int y) {
        return int(std::lround(x * (1.0 - ratio) + y * ratio));
    };
    return Rgb{ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b)};
}

// 主题表（§4.7 THEMES）；未知 → white
Theme theme_of(const std::string& theme_name) {
    std::string key = theme_name.empty() ? "white" : theme_name;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    const char* bg_hex = "FFFFFF";
    const char* fg_hex = "222222";
    if (key == "paper")      { bg_hex = "F5F0E6"; fg_hex = "2B2B2B"; }
    else if (key == "sepia") { bg_hex = "F0E2C8"; fg_hex = "3B2E1E"; }
    else if (key == "green") { bg_hex = "CCE8CF"; fg_hex = "1F2A1F"; }
    else if (key == "dark")  { bg_hex = "111418"; fg_hex = "C8C8C8"; }
    Theme t;
    t.bg = hex_to_rgb(bg_hex);
    t.fg = hex_to_rgb(fg_hex);
    t.muted = blend(t.fg, t.bg, 0.45);
    return t;
}

// ============================================================ scale clamp
}  // namespace

// §4.7 clamp_scale：clamp 0.5..2.0，超 max_page_pixels 时每次 -0.05
double clamp_scale(double scale, const LayoutProfile& profile, long long max_page_pixels) {
    double s = std::max(0.5, std::min(2.0, scale > 0.0 ? scale : 1.0));
    if (max_page_pixels <= 0) return s;
    while (s > 0.5) {
        const double w = profile.viewport.w * s;
        const double h = profile.viewport.h * s;
        if (w * h <= double(max_page_pixels)) break;
        s = std::round((s - 0.05) * 100.0) / 100.0;
    }
    return std::max(0.5, s);
}

namespace {

// ============================================================ glyph blit
// 8bit 灰度字形 → 与前景色合成到 RGBA 缓冲（自写 blit，flow 页与封面共用）
void blit_glyph(ImageBuf& canvas, const GlyphBitmap& g, int pen_x, int baseline_y,
                const Rgb& fg) {
    if (g.pixels == nullptr || g.width <= 0 || g.rows <= 0) return;
    // 字形位图原点 = (pen_x + left, baseline - top)
    const int x0 = pen_x + g.left;
    const int y0 = baseline_y - g.top;
    for (int row = 0; row < g.rows; ++row) {
        const int dy = y0 + row;
        if (dy < 0 || dy >= canvas.h) continue;
        auto* dst = &canvas.rgba[(size_t(dy) * canvas.w) * 4];
        const auto* src = g.pixels + (size_t(row) * g.pitch);
        for (int col = 0; col < g.width; ++col) {
            const int dx = x0 + col;
            if (dx < 0 || dx >= canvas.w) continue;
            const unsigned char cov = src[col];
            if (cov == 0) continue;
            auto* px = &dst[size_t(dx) * 4];
            if (cov >= 255) {
                px[0] = (unsigned char)fg.r;
                px[1] = (unsigned char)fg.g;
                px[2] = (unsigned char)fg.b;
            } else {
                // out = bg + (fg - bg) * cov / 255
                px[0] = (unsigned char)std::lround(px[0] + (fg.r - px[0]) * cov / 255.0);
                px[1] = (unsigned char)std::lround(px[1] + (fg.g - px[1]) * cov / 255.0);
                px[2] = (unsigned char)std::lround(px[2] + (fg.b - px[2]) * cov / 255.0);
            }
        }
    }
}

// 画一段文本（给定基线 y）。spacing 绝对值 > 0.01 时逐字加间距（间距 = 字宽 +
// letter_spacing；此处恒逐字绘制，无 kerning，与分页逐字度量同源）；
// bold = 偏移 max(1, 字号/32) 二次绘制（不改行宽）；italic 无特殊处理。
void draw_text(ImageBuf& canvas, FontManager& fonts, const U32Text& text,
               double x0, double baseline_y, double spacing, const Rgb& fg,
               bool bold, double font_size) {
    if (text.empty()) return;
    auto pass = [&](double start_x) {
        double x = start_x;
        for (char32_t ch : text) {
            blit_glyph(canvas, fonts.render_glyph(ch),
                       int(std::lround(x)), int(std::lround(baseline_y)), fg);
            // advance：宽字符恒 = 字号（is_wide_char 语义在 char_length 内），
            // 其余 = 字形 advance；统一加 letter_spacing（与分页度量一致）
            double adv;
            try {
                adv = fonts.char_length(ch);
            } catch (...) {
                adv = font_size;
            }
            x += adv + spacing;
        }
    };
    pass(x0);
    if (bold) pass(x0 + std::max(1.0, font_size / 32.0));  // 模拟加粗
}

// ============================================================ 缺失图占位
// 灰框 + "图片无法显示" 文字（字体用 fonts 默认；加载失败则只画框）
void draw_missing_image(ImageBuf& canvas, double x, double y, int w, int h,
                        FontManager* fonts) {
    const unsigned char gray = 150;
    const int bx0 = int(std::lround(x)), by0 = int(std::lround(y));
    const int bx1 = int(std::lround(x + w)), by1 = int(std::lround(y + h));
    auto hline = [&](int yy, int xa, int xb) {
        if (yy < 0 || yy >= canvas.h) return;
        for (int xx = std::max(0, xa); xx <= std::min(canvas.w - 1, xb); ++xx) {
            auto* p = &canvas.rgba[(size_t(yy) * canvas.w + xx) * 4];
            p[0] = p[1] = p[2] = gray;
        }
    };
    auto vline = [&](int xx, int ya, int yb) {
        if (xx < 0 || xx >= canvas.w) return;
        for (int yy = std::max(0, ya); yy <= std::min(canvas.h - 1, yb); ++yy) {
            auto* p = &canvas.rgba[(size_t(yy) * canvas.w + xx) * 4];
            p[0] = p[1] = p[2] = gray;
        }
    };
    hline(by0, bx0, bx1);
    hline(by1 - 1, bx0, bx1);
    vline(bx0, by0, by1);
    vline(bx1 - 1, by0, by1);

    if (fonts == nullptr || fonts->default_id_raw().empty()) return;
    // 字号 = max(10, min(28, h//6 or 12))（对齐 Python default_font 取号）
    const int font_size = std::max(10, std::min(28, h / 6 > 0 ? h / 6 : 12));
    if (!fonts->load(fonts->default_id(), font_size)) return;
    const Rgb label_fg{120, 120, 120};
    const double baseline = std::lround(y) + 8 + fonts->ascent();
    draw_text(canvas, *fonts, utf8_to_u32("图片无法显示"), std::lround(x) + 8,
              baseline, 0.0, label_fg, false, double(font_size));
}

// ============================================================ 图片条目绘制
void draw_image_item(ImageBuf& canvas, const LayoutItem& item,
                     const RenderDeps& deps, double left, double y, double avail_w,
                     double scale) {
    const int target_w = std::max(1, int(std::lround(item.display_w * scale)));
    const int target_h = std::max(1, int(std::lround(item.display_h * scale)));
    const double px = left + std::max(0.0, (avail_w - target_w) / 2.0);
    const std::string asset = item.image_block ? item.image_block->asset : std::string();

    std::string data;
    bool loaded = false;
    if (deps.asset_loader && !asset.empty()) {
        try {
            loaded = deps.asset_loader(asset, data);
        } catch (...) {
            loaded = false;
        }
    }
    ImageBuf img;
    if (loaded && !data.empty()) {
        try {
            img = decode_image(data, deps.max_source_pixels);
            img = resize_to(img, target_w, target_h);
        } catch (...) {
            img = ImageBuf{};  // 解码/缩放失败 → 灰框占位
            loaded = false;
        }
    }
    if (!loaded || img.rgba.empty()) {
        draw_missing_image(canvas, px, y, target_w, target_h, deps.fonts);
        return;
    }
    // 水平居中粘贴（alpha 合成）
    const int ox = int(std::lround(px)), oy = int(std::lround(y));
    for (int row = 0; row < img.h; ++row) {
        const int dy = oy + row;
        if (dy < 0 || dy >= canvas.h) continue;
        for (int col = 0; col < img.w; ++col) {
            const int dx = ox + col;
            if (dx < 0 || dx >= canvas.w) continue;
            const auto* s = &img.rgba[(size_t(row) * img.w + col) * 4];
            auto* d = &canvas.rgba[(size_t(dy) * canvas.w + dx) * 4];
            if (s[3] >= 255) {
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            } else if (s[3] > 0) {
                const int a = s[3];
                d[0] = (unsigned char)((s[0] * a + d[0] * (255 - a)) / 255);
                d[1] = (unsigned char)((s[1] * a + d[1] * (255 - a)) / 255);
                d[2] = (unsigned char)((s[2] * a + d[2] * (255 - a)) / 255);
            }
        }
    }
    // 注意：不要把 img.has_alpha 直接传染给画布。画布底色恒为不透明（Python 的
    // Image.new("RGB")），上面也只会写入 255 的 alpha；一旦把 has_alpha 置真，
    // encode_png 就会输出 RGBA 色彩类型（6），文本页体积比 Python 大 ~20%（审查 M9）。
}

// ============================================================ 照片页判定
// §4.7 is_photo_page：图片面积 ≥ 内容区×0.5，或（无文本行且最大图宽 ≥ 可用宽×0.8）
bool is_photo_page(const std::vector<LayoutItem>& items, double content_area,
                   double avail_w) {
    if (items.empty() || content_area <= 0) return false;
    double area = 0.0;
    bool has_text = false;
    double max_w = 0.0;
    for (const auto& it : items) {
        if (it.kind == LayoutItem::Kind::Image) {
            area += it.display_w * it.display_h;
            max_w = std::max(max_w, it.display_w);
        } else if (it.kind == LayoutItem::Kind::TextLine) {
            has_text = true;
        }
    }
    if (area >= content_area * 0.5) return true;
    return (!has_text) && avail_w > 0 && max_w >= avail_w * 0.8;
}

// ============================================================ 文字封面辅助
// 封面字体：fonts 为空 / 加载失败 → false（调用方跳过绘制、宽度退化估算）
bool cover_font(FontManager* fonts, int size) {
    if (fonts == nullptr) return false;
    try {
        return fonts->load(fonts->resolve_id(std::string()), size);
    } catch (...) {
        return false;
    }
}

// 文本宽度（优先字体度量，失败退 len×size×0.6，对齐 Python 兜底）
double cover_text_width(FontManager* fonts, const U32Text& text, int size) {
    if (fonts == nullptr) return double(text.size()) * size * 0.6;
    double w = 0.0;
    for (char32_t ch : text) {
        try {
            w += fonts->char_length(ch);
        } catch (...) {
            w += size;
        }
    }
    return w;
}

// 逐字符贪心换行，最多 max_lines 行，超出末行去 1 字符加 "…"（对齐 _wrap_for_cover）
std::vector<U32Text> wrap_for_cover(FontManager* fonts, const U32Text& text,
                                    int size, double max_width, int max_lines) {
    // " ".join(text.split())：按空白折叠（含全角空格）
    U32Text norm;
    bool in_ws = false;
    for (char32_t ch : text) {
        const bool ws = ch == U' ' || ch == U'\t' || ch == U'\n' || ch == U'\r' ||
                        ch == U'\v' || ch == U'\f' || ch == 0x3000 || ch == 0x00A0;
        if (ws) {
            in_ws = true;
        } else {
            if (in_ws && !norm.empty()) norm.push_back(U' ');
            in_ws = false;
            norm.push_back(ch);
        }
    }
    if (norm.empty()) return {U32Text(1, U' ')};

    std::vector<U32Text> lines;
    U32Text cur;
    for (char32_t ch : norm) {
        U32Text probe = cur;
        probe.push_back(ch);
        const double width = cover_text_width(fonts, probe, size);
        if (!cur.empty() && width > max_width) {
            lines.push_back(cur);
            cur.assign(1, ch);
            if (int(lines.size()) >= max_lines - 1) break;
        } else {
            cur = probe;
        }
    }
    if (!cur.empty() && int(lines.size()) < max_lines) lines.push_back(cur);
    // 溢出加省略号
    size_t joined = 0;
    for (const auto& l : lines) joined += l.size();
    if (int(lines.size()) >= max_lines && joined < norm.size()) {
        auto& last = lines.back();
        if (!last.empty()) last.pop_back();
        last.push_back(U'…');
    }
    if (lines.empty()) lines.push_back(norm.substr(0, 1));
    return lines;
}

// ============================================================ 画布构造
// RGBA 画布并填充背景色（RGB→RGBA 缓冲）
ImageBuf make_canvas(int w, int h, const Rgb& bg) {
    ImageBuf c;
    c.w = std::max(1, w);
    c.h = std::max(1, h);
    c.rgba.assign(size_t(c.w) * c.h * 4, 0);
    c.has_alpha = false;
    for (size_t i = 0; i < c.rgba.size(); i += 4) {
        c.rgba[i] = (unsigned char)bg.r;
        c.rgba[i + 1] = (unsigned char)bg.g;
        c.rgba[i + 2] = (unsigned char)bg.b;
        c.rgba[i + 3] = 255;
    }
    return c;
}

}  // namespace

// ============================================================ render_flow_page
ImageBytes render_flow_page(const DocumentModel& doc, const PageMap& pm, int page,
                            const LayoutProfile& profile, const RenderDeps& deps) {
    const PageRecord* record = pm.record(page);
    if (record == nullptr) throw std::out_of_range("页码越界: " + std::to_string(page));

    FontManager* fonts = deps.fonts;
    if (fonts == nullptr) throw std::invalid_argument("render_flow_page 需要 FontManager");

    // 布局：从页锚点重排一页（行位置/字号与分页完全同源）
    FlowLayoutEngine::Deps ed;
    ed.doc = &doc;
    ed.fonts = fonts;
    ed.profile = &profile;
    ed.font_id = fonts->resolve_id(profile.font.family);
    FlowLayoutEngine engine(ed);
    auto laid = engine.items_for_page(pm, page);
    if (!laid) throw std::out_of_range("页码越界: " + std::to_string(page));

    const double s = clamp_scale(deps.scale, profile, deps.max_page_pixels);
    const Theme th = theme_of(deps.theme.empty() ? std::string("white") : deps.theme);
    ImageBuf canvas = make_canvas(int(std::lround(profile.viewport.w * s)),
                                  int(std::lround(profile.viewport.h * s)), th.bg);

    const double left = double(profile.margin.left) * s;
    const double top = double(profile.margin.top) * s;
    const double avail_w = double(profile.avail_width()) * s;
    const double spacing = profile.font.letter_spacing_px * s;

    // 条目逐条绘制：坐标 = margin×scale + 条目 y×scale（y 按顺序累计）
    double cursor_y = 0.0;
    for (const auto& item : laid->items) {
        const double y = top + cursor_y * s;
        if (item.kind == LayoutItem::Kind::TextLine) {
            const int font_size = std::max(4, int(std::lround(item.font_size * s)));
            // FaceGuard：整行绘制期间独占 face，避免并发请求把 face 换成别的字号
            // （否则字形会按别人的字号画出来，图与排版不一致）
            FontManager::FaceGuard guard(*fonts, ed.font_id, font_size);
            if (guard.ok()) {
                // 文本基线垂直居中于行框；负则 0
                const double glyph_h = fonts->ascent() + fonts->descent();
                const double box_h = item.height * s;
                const double ty = y + std::max(0.0, (box_h - glyph_h) / 2.0);
                const double baseline = ty + fonts->ascent();
                draw_text(canvas, *fonts, item.line_text, left, baseline, spacing,
                          th.fg, item.bold, double(font_size));
            }
        } else if (item.kind == LayoutItem::Kind::Image) {
            draw_image_item(canvas, item, deps, left, y, avail_w, s);
        } else if (item.kind == LayoutItem::Kind::Separator) {
            // 条目高度中点水平线：x 从 8% 到 92% 可用宽，muted 色，线宽 max(1, round(scale))
            const int ly = int(std::lround(y + item.height * s / 2.0));
            const int thick = std::max(1, int(std::lround(s)));
            const int xa = int(std::lround(left + avail_w * 0.08));
            const int xb = int(std::lround(left + avail_w * 0.92));
            for (int t = 0; t < thick; ++t) {
                const int yy = ly + t;
                if (yy < 0 || yy >= canvas.h) continue;
                for (int xx = std::max(0, xa); xx <= std::min(canvas.w - 1, xb); ++xx) {
                    auto* p = &canvas.rgba[(size_t(yy) * canvas.w + xx) * 4];
                    p[0] = (unsigned char)th.muted.r;
                    p[1] = (unsigned char)th.muted.g;
                    p[2] = (unsigned char)th.muted.b;
                }
            }
        }
        // ChapterMark 不占高度、不绘制
        cursor_y += item.height + item.space_after;
    }

    // 编码：照片页且允许 → JPEG q85；否则 PNG
    const double content_area =
        double(profile.avail_width()) * double(profile.avail_height());
    const bool prefer_jpeg =
        deps.jpeg_for_photo_pages &&
        is_photo_page(laid->items, content_area, double(profile.avail_width()));
    EncodeOpts opts;
    opts.prefer_jpeg = prefer_jpeg;
    opts.jpeg_quality = 85;
    return encode_image(canvas, opts);
}

// ============================================================ text_cover
ImageBytes text_cover(const std::string& title, const std::optional<std::string>& author,
                      int width, FontManager* fonts) {
    const int w = std::max(80, std::min(1024, width > 0 ? width : 320));
    const int h = int(std::lround(w * 1.42));

    // 稳定哈希选配色（进程内 hash 加盐 → 同一本书重启配色会变，必须 sha1）
    static const char* kPalette[][2] = {
        {"2F4858", "FFFFFF"}, {"33658A", "F6F7F8"}, {"5B5F97", "FFFFFF"},
        {"7A4E48", "FFF6EE"}, {"386641", "F2F7F4"}, {"6B4226", "FFF3E4"},
    };
    const std::string hash8 = stable_hash(title, 8);
    unsigned long long idx = 0;
    for (char c : hash8) {
        idx *= 16;
        idx += unsigned(c >= '0' && c <= '9' ? c - '0'
                        : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0);
    }
    const Rgb bg = hex_to_rgb(kPalette[idx % 6][0]);
    const Rgb fg = hex_to_rgb(kPalette[idx % 6][1]);

    ImageBuf canvas = make_canvas(w, h, bg);

    // 顶部装饰条（blend(fg,bg,0.75)）
    const Rgb bar = blend(fg, bg, 0.75);
    const int bar_h = std::max(2, h / 60);
    for (int yy = 0; yy < bar_h && yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
            auto* p = &canvas.rgba[(size_t(yy) * w + xx) * 4];
            p[0] = (unsigned char)bar.r;
            p[1] = (unsigned char)bar.g;
            p[2] = (unsigned char)bar.b;
        }

    const int title_size = std::max(12, int(w * 0.13));
    const int author_size = std::max(10, int(w * 0.082));
    const double max_width = double(w - int(w * 0.16));
    const double x0 = double(int(w * 0.08));

    // 标题：字号 w×0.13，逐字符贪心换行最多 4 行（超出加 "…"）
    const bool have_font = cover_font(fonts, title_size);
    const std::vector<U32Text> lines = wrap_for_cover(
        have_font ? fonts : nullptr, utf8_to_u32(title.empty() ? "无标题" : title),
        title_size, max_width, 4);
    const int line_h = int(title_size * 1.35);
    const int total_h = line_h * int(lines.size());
    int y = std::max(int(h * 0.18), int((h - total_h) / 2) - int(h * 0.06));
    if (have_font && fonts != nullptr) {
        double baseline = y + fonts->ascent();  // draw.text 顶部对齐 → 基线下移 ascent
        for (const auto& line : lines) {
            draw_text(canvas, *fonts, line, x0, baseline, 0.0, fg, false,
                      double(title_size));
            baseline += line_h;
        }
    }

    // 作者：字号 w×0.082，底部最多 2 行，配色 blend(fg,bg,0.25)；
    // 注意 Python 中 line_h // 2 * len(author_lines) 用的是标题的 line_h
    if (author && !author->empty()) {
        const bool have_author_font = cover_font(fonts, author_size);
        const std::vector<U32Text> author_lines = wrap_for_cover(
            have_author_font ? fonts : nullptr, utf8_to_u32(*author),
            author_size, max_width, 2);
        const int ay = h - int(h * 0.14) - line_h / 2 * int(author_lines.size());
        if (have_author_font && fonts != nullptr) {
            const Rgb author_fg = blend(fg, bg, 0.25);
            double baseline = ay + fonts->ascent();
            for (const auto& line : author_lines) {
                draw_text(canvas, *fonts, line, x0, baseline, 0.0, author_fg, false,
                          double(author_size));
                baseline += int(author_size * 1.4);
            }
        }
    }

    EncodeOpts opts;
    opts.prefer_jpeg = true;
    opts.jpeg_quality = 88;
    return encode_image(canvas, opts);
}

// ============================================================ build_page_json
namespace {

// RFC3986 unreserved 之外的 UTF-8 字节全部百分号编码（等价 Python quote(safe='')）
std::string url_encode_path(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    auto unreserved = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
    };
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (unreserved(c)) {
            out.push_back(char(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

}  // namespace

nlohmann::json build_page_json(const DocumentModel& doc, const PageMap& pm, int page,
                               const LayoutProfile& profile, FontManager* fonts,
                               const std::string& book_id) {
    const PageRecord* record = pm.record(page);
    if (record == nullptr) throw std::out_of_range("页码越界: " + std::to_string(page));
    const std::string bid = book_id.empty() ? doc.book_id : book_id;

    FlowLayoutEngine::Deps ed;
    ed.doc = &doc;
    ed.fonts = fonts;
    ed.profile = &profile;
    ed.font_id = fonts ? fonts->resolve_id(profile.font.family) : std::string();
    FlowLayoutEngine engine(ed);
    auto laid = engine.items_for_page(pm, page);
    if (!laid) throw std::out_of_range("页码越界: " + std::to_string(page));

    nlohmann::json blocks = nlohmann::json::array();
    // 合并键：kind(heading/paragraph)/chapter/block/level/font_size
    bool has_key = false;
    bool cur_heading = false;
    int cur_chapter = 0, cur_block = 0, cur_level = 0;
    double cur_font_size = 0;
    // 每个块的首条目下标，与 blocks 一一对应（用于回填 y）。
    // 之前用两个游标 + "是否块首行"标志来回填，一旦出现"文本块紧跟文本块"或
    // "图片在前、段落在后"，就会把 y 写到别的块上（客户端按 y 绘制 → 文字叠在页顶）。
    std::vector<size_t> block_first_item;

    size_t idx = 0;
    // —— 主体循环：逐条目产出/合并块；y 因 LayoutItem 无字段而延后统一回填 ——
    idx = 0;
    for (const auto& item : laid->items) {
        if (item.kind == LayoutItem::Kind::TextLine) {
            // heading 判定：与 Python TextItem.level 语义一致（段落 level=0，标题 1..6）
            const bool heading = item.level >= 1;
            const bool same = has_key && heading == cur_heading &&
                              item.chapter == cur_chapter && item.block == cur_block &&
                              item.level == cur_level && item.font_size == cur_font_size;
            if (!same) {
                nlohmann::json block = nlohmann::json::object();
                block["type"] = heading ? "heading" : "paragraph";
                if (heading) block["level"] = item.level > 0 ? item.level : 1;
                block["lines"] = nlohmann::json::array();
                block["font_size"] = (long long)std::llround(item.font_size);
                block["y"] = 0;      // 下方统一回填
                block["height"] = 0.0;
                if (item.bold) block["bold"] = true;
                if (item.italic) block["italic"] = true;
                blocks.push_back(block);
                block_first_item.push_back(idx);
                cur_heading = heading;
                cur_chapter = item.chapter;
                cur_block = item.block;
                cur_level = item.level;
                cur_font_size = item.font_size;
                has_key = true;
            }
            nlohmann::json& last = blocks.back();
            last["lines"].push_back(u32_to_utf8(item.line_text));
            last["height"] = py_num(last["height"].get<double>() + item.height, 2);
        } else if (item.kind == LayoutItem::Kind::Image) {
            has_key = false;
            const std::string asset =
                item.image_block ? item.image_block->asset : std::string();
            nlohmann::json block = nlohmann::json::object();
            block["type"] = "image";
            block["asset"] = "/api/v1/books/" + bid + "/asset?path=" + url_encode_path(asset);
            block["display_w"] = py_num(item.display_w, 2);
            block["display_h"] = py_num(item.display_h, 2);
            block["y"] = 0;  // 下方统一回填
            block["height"] = py_num(item.height, 2);
            blocks.push_back(block);
            block_first_item.push_back(idx);
        } else if (item.kind == LayoutItem::Kind::Separator) {
            has_key = false;
            nlohmann::json block = nlohmann::json::object();
            block["type"] = "separator";
            block["y"] = 0;  // 下方统一回填
            block["height"] = py_num(item.height, 2);
            blocks.push_back(block);
            block_first_item.push_back(idx);
        }
        ++idx;
    }

    // y 统一回填：块 y = 该块首条目处的累计 y（Python jsonout.py: item.y = 引擎累计 used）
    {
        double cursor = 0.0;
        size_t bi = 0;
        for (size_t i = 0; i < laid->items.size(); ++i) {
            if (bi < block_first_item.size() && block_first_item[bi] == i) {
                blocks[bi]["y"] = py_num(cursor, 2);
                ++bi;
            }
            cursor += laid->items[i].height + laid->items[i].space_after;
        }
    }

    nlohmann::json payload = nlohmann::json::object();
    payload["page"] = record->page;
    payload["total_pages"] = pm.total_pages;
    payload["anchor"] = {{"chapter", record->anchor[0]},
                         {"block", record->anchor[1]},
                         {"char", record->anchor[2]}};
    payload["chapter_index"] = record->chapter_index;
    payload["chapter_title"] = u32_to_utf8(record->chapter_title);
    payload["mode"] = pm.mode;
    payload["blocks"] = blocks;
    payload["font"] = {{"family", profile.font.family},
                       {"size_px", profile.font.size_px},
                       {"line_height", profile.font.line_height},
                       {"letter_spacing_px", profile.font.letter_spacing_px}};
    payload["margin"] = {{"left", profile.margin.left},
                         {"right", profile.margin.right},
                         {"top", profile.margin.top},
                         {"bottom", profile.margin.bottom}};
    payload["viewport"] = {{"w", profile.viewport.w}, {"h", profile.viewport.h}};
    // 缩进已含在 lines 的 \u3000 前缀里 → first_line_indent_chars 恒 0
    payload["paragraph"] = {{"spacing_px", profile.paragraph.spacing_px},
                            {"first_line_indent_chars", 0},
                            {"indent_applied_in_lines", true}};
    return payload;
}

}  // namespace lnv
