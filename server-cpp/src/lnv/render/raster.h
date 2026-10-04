#pragma once
// 页面渲染（对应 Python render/raster.py、render/jsonout.py）
#include "lnv/layout/engine.h"
#include "lnv/layout/fonts.h"
#include "lnv/util/imaging.h"

namespace lnv {

struct RenderDeps {
    FontManager* fonts = nullptr;
    const std::filesystem::path* base_dir = nullptr;  // 相对路径解析（config base）
    // 图片资产加载（EPUB 内嵌图等）：path → bytes；失败返回 false
    std::function<bool(const std::string&, std::string&)> asset_loader;
    long long max_source_pixels = 30000000;
    long long max_page_pixels = 4000000;
    bool jpeg_for_photo_pages = true;
    std::string theme = "white";  // 主题名（§4.7 THEMES；未知→white）
    // 客户端请求的渲染倍率（?scale=，0.5..2.0）。渲染与 ETag/缓存键必须用同一个值，
    // 否则会出现"ETag 说是 2.0 倍、字节其实是 1 倍"的缓存污染。
    double scale = 1.0;
};

// flow 页渲染 → (图片字节, mime)
ImageBytes render_flow_page(const DocumentModel& doc, const PageMap& pm, int page,
                            const LayoutProfile& profile, const RenderDeps& deps);

// §4.7 clamp_scale：clamp 0.5..2.0，超出 max_page_pixels 时每次 -0.05。
// 固定版式取页也要用它（viewport_w = round(viewport.w × clamp_scale(scale, …))），
// 否则 render.max_page_pixels 对漫画/图集完全不生效。
double clamp_scale(double scale, const LayoutProfile& profile, long long max_page_pixels);

// 文字封面（§4.7 text_cover）：配色 = COVER_PALETTE[sha1(title)[:8] mod 6]
ImageBytes text_cover(const std::string& title, const std::optional<std::string>& author,
                      int width, FontManager* fonts);

// JSON 模式页（§4.7 build_page_json）
nlohmann::json build_page_json(const DocumentModel& doc, const PageMap& pm, int page,
                               const LayoutProfile& profile, FontManager* fonts,
                               const std::string& book_id);

}  // namespace lnv
