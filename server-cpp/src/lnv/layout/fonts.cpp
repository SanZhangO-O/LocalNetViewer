// 字体管理实现（对应 Python app/layout/fonts.py）
// 度量约定（PORT_SPEC §4.4）：宽字符宽度由 is_wide_char 查表决定（= 字号，
// 由调用方处理），本模块只负责非宽字符的 FreeType 度量与光栅化。
#include "lnv/layout/fonts.h"

#include <ft2build.h>
#include FT_FREETYPE_H

namespace lnv {

// ---------------------------------------------------------------- 码段表
// 与 Python fonts.py::is_wide_char 逐段一致（§4.4），不得增删。
bool is_wide_char(char32_t c) {
    if (c < 0x1100) return false;
    return (c >= 0x1100 && c <= 0x115F)
        || (c >= 0x2E80 && c <= 0x303E)
        || (c >= 0x3041 && c <= 0x33FF)
        || (c >= 0x3400 && c <= 0x4DBF)
        || (c >= 0x4E00 && c <= 0x9FFF)
        || (c >= 0xA000 && c <= 0xA4CF)
        || (c >= 0xAC00 && c <= 0xD7A3)
        || (c >= 0xF900 && c <= 0xFAFF)
        || (c >= 0xFE10 && c <= 0xFE19)
        || (c >= 0xFE30 && c <= 0xFE6F)
        || (c >= 0xFF00 && c <= 0xFF60)
        || (c >= 0xFFE0 && c <= 0xFFE6)
        || (c >= 0x1F300 && c <= 0x1FAFF)
        || (c >= 0x20000 && c <= 0x3FFFD);
}

namespace {

constexpr const char* kFontExts[] = {".ttf", ".otf", ".ttc", ".otc"};
// 与 Python MAX_FONT_FILES 一致的扫描上限
constexpr size_t kMaxFontFiles = 600;
// can_draw_cjk 探测字号（px）
constexpr int kGlyphProbeSize = 64;

// fonts.py CJK_HINTS：字形探测全军覆没时按文件名兜底
constexpr const char* kCjkHints[] = {
    "notosanssc", "noto sans sc", "notoserifsc", "sourcehansans", "source han",
    "思源", "sarasa", "lxgw", "msyh", "microsoft yahei", "simhei", "simsun",
    "dengxian", "pingfang", "hiragino", "songti", "kaiti", "fangsong", "wqy",
    "wenquanyi", "droidsansfallback", "arial unicode", "unifont",
};

std::string ascii_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// Python str.strip() 的空白等价（与 textutil 的 trim 语义一致）
std::string trim_ws(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n\f\v");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n\f\v");
    return s.substr(b, e - b + 1);
}

// 系统字体目录（fonts.py _SYSTEM_FONT_DIRS）：Windows 除 C:\Windows\Fonts 外
// 还有用户级 %LOCALAPPDATA%\Microsoft\Windows\Fonts —— 只扫前者会漏掉用户安装的字体
std::vector<std::filesystem::path> system_font_dirs() {
    std::vector<std::filesystem::path> out;
    out.emplace_back(L"C:\\Windows\\Fonts");
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
        out.emplace_back(std::filesystem::path(local) / "Microsoft" / "Windows" / "Fonts");
    return out;
}

bool name_suggests_cjk(const std::string& id, const std::string& file_name) {
    std::string hay = ascii_lower(id + " " + file_name);
    for (const char* hint : kCjkHints) {
        std::string h = ascii_lower(hint);
        if (hay.find(h) != std::string::npos) return true;
    }
    return false;
}

bool is_font_ext(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    for (const char* e : kFontExts)
        if (ext == e) return true;
    return false;
}

// 递归扫描目录下的字体文件，按路径排序（对齐 Python sorted(rglob) 语义），
// 返回 (路径, stem) 列表，总数不超过 kMaxFontFiles。
std::vector<std::pair<std::filesystem::path, std::string>> scan_font_dir(
        const std::filesystem::path& dir) {
    std::vector<std::pair<std::filesystem::path, std::string>> out;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return out;
    std::vector<std::filesystem::path> entries;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        entries.push_back(it->path());
    }
    // Python 的 sorted(rglob("*")) 比较的是 pathlib.Path：Windows 上先按
    // **大小写不敏感**的规范化路径比较，再按原串（区分大小写）比较。
    // 只按 generic_string() 排序会得到不同的顺序 —— 而顺序决定"首个内置/首个含中文
    // 字形/名字提示"的默认字体选择，进而影响 layout_id（审查 L12）。
    auto fold = [](std::string s) {
        for (char& c : s) c = char(::tolower((unsigned char)c));
        return s;
    };
    std::sort(entries.begin(), entries.end(),
              [&](const auto& a, const auto& b) {
                  std::string sa = a.generic_string(), sb = b.generic_string();
                  std::string fa = fold(sa), fb = fold(sb);
                  if (fa != fb) return fa < fb;
                  return sa < sb;
              });
    for (const auto& p : entries) {
        if (out.size() >= kMaxFontFiles) break;
        std::error_code ec2;
        if (!std::filesystem::is_regular_file(p, ec2) || ec2) continue;
        if (!is_font_ext(p)) continue;
        std::string stem = p.stem().string();
        // 与 Python _face_id 一致：stem 去空白后为空则跳过
        size_t b = stem.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        size_t e = stem.find_last_not_of(" \t\r\n");
        stem = stem.substr(b, e - b + 1);
        if (stem.empty()) continue;
        out.emplace_back(p, stem);
    }
    return out;
}

// 用整个文件字节建 face（FT_New_Memory_Face），避免 Windows 上 FT_New_Face
// 的 ANSI 路径问题；字节缓冲必须与 face 同生命周期（由 Impl 持有）。
bool read_file_bytes(const std::filesystem::path& p, std::vector<unsigned char>& out) {
    std::error_code ec;
    auto size = std::filesystem::file_size(p, ec);
    if (ec || size == 0 || size > (1ull << 30)) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.resize(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(out.data()), out.size());
    return static_cast<size_t>(f.gcount()) == out.size();
}

}  // namespace

// ---------------------------------------------------------------- Impl
struct FontManager::Impl {
    FT_Library lib = nullptr;
    // 整体互斥锁：load/char_length/ascent/descent/render_glyph 串行化（简单为主）。
    // 递归锁：FaceGuard 在作用域内持锁，期间仍会调用 char_length/render_glyph。
    std::recursive_mutex mtx;
    FT_Face face = nullptr;                 // 当前已加载 face
    std::vector<unsigned char> face_bytes;  // face 的内存映像（须先于 face 释放）
    std::string loaded_id;                  // 当前 face 对应的 font_id
    int loaded_size = 0;                    // 当前 face 对应的像素字号
    std::unordered_map<std::string, std::filesystem::path> paths;  // id → 文件路径
    // (font_id, size, codepoint) → advance 像素宽度 缓存
    std::map<std::tuple<std::string, int, char32_t>, double> char_cache;

    Impl() { FT_Init_FreeType(&lib); }
    ~Impl() {
        if (face) FT_Done_Face(face);
        if (lib) FT_Done_FreeType(lib);
    }

    // 加载指定 id/字号的 face；已加载相同 (id,size) 时直接复用。
    bool ensure(const std::string& font_id, int size_px) {
        if (face && loaded_id == font_id && loaded_size == size_px) return true;
        auto it = paths.find(font_id);
        if (it == paths.end()) return false;
        std::vector<unsigned char> bytes;
        if (!read_file_bytes(it->second, bytes)) return false;
        FT_Face new_face = nullptr;
        if (FT_New_Memory_Face(lib, bytes.data(), static_cast<FT_Long>(bytes.size()),
                               0, &new_face) != 0)
            return false;
        // 像素字号：Set_Pixel_Sizes(width=size_px, height=0)，
        // 与 Python getlength 的像素字号语义对齐
        if (FT_Set_Pixel_Sizes(new_face, size_px, 0) != 0) {
            FT_Done_Face(new_face);
            return false;
        }
        if (face) FT_Done_Face(face);
        face = new_face;
        face_bytes = std::move(bytes);
        loaded_id = font_id;
        loaded_size = size_px;
        return true;
    }
};

FontManager::FontManager() : impl_(std::make_unique<Impl>()) {}
FontManager::~FontManager() = default;

FontManager::FaceGuard::FaceGuard(FontManager& fm, const std::string& font_id, int size_px)
    : fm_(&fm) {
    auto* lk = new std::unique_lock<std::recursive_mutex>(fm.impl_->mtx);
    lock_ = lk;
    if (size_px < 4) size_px = 4;
    ok_ = fm.impl_->ensure(font_id, size_px);
}

FontManager::FaceGuard::~FaceGuard() {
    delete static_cast<std::unique_lock<std::recursive_mutex>*>(lock_);
}

// ---------------------------------------------------------------- init
void FontManager::init(const std::filesystem::path& fonts_dir, const std::string& default_font) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    faces_.clear();
    impl_->paths.clear();
    default_id_.clear();
    impl_->char_cache.clear();
    fonts_dir_ = fonts_dir;

    auto register_face = [&](const std::filesystem::path& path, const std::string& id,
                             bool builtin) {
        FontFaceInfo info;
        info.id = id;
        info.name = id;  // 语义约定：name = id（Python 用 stem，二者同值）
        info.file_name = path.filename().string();
        info.builtin = builtin;
        auto it = std::find_if(faces_.begin(), faces_.end(),
                               [&](const FontFaceInfo& f) { return f.id == id; });
        if (it != faces_.end()) {
            // 同 id 重复：内置字体优先于系统字体
            if (!builtin) return;
            *it = info;
        } else {
            faces_.push_back(info);
        }
        impl_->paths[id] = path;
    };

    // 1) 内置字体目录（builtin，全部收录）
    for (auto& [path, stem] : scan_font_dir(fonts_dir)) register_face(path, stem, true);

    // 2) 系统字体目录：Python 的 discover() 收录**全部**字体（中文能力只用于挑默认值，
    //    不做收录过滤）。早期实现只收录能画中文的字体，导致 /render/fonts 列表比 Python
    //    少一大截，客户端字体选择器里看不到 Arial/Calibri 等，font.family 也无法命中。
    //    can_draw_cjk 只在挑默认字体时调用（结果按路径缓存，避免重复光栅化探测）。
    static std::mutex cjk_cache_mtx;
    static std::unordered_map<std::string, bool> cjk_cache;  // 路径 → 是否可画中文
    auto can_draw_cjk_cached = [&](const std::filesystem::path& path) {
        std::string key = path.generic_string();
        std::lock_guard<std::mutex> ck(cjk_cache_mtx);
        auto cit = cjk_cache.find(key);
        if (cit != cjk_cache.end()) return cit->second;
        bool ok = can_draw_cjk(path);  // 探测较慢；单次 init 内串行可接受
        cjk_cache[key] = ok;
        return ok;
    };

    for (const auto& dir : system_font_dirs())
        for (auto& [path, stem] : scan_font_dir(dir)) register_face(path, stem, false);

    if (faces_.empty()) return;  // 无可用字体：default_id_ 保持空

    auto path_of = [&](const std::string& id) -> std::filesystem::path {
        auto it = impl_->paths.find(id);
        return it == impl_->paths.end() ? std::filesystem::path() : it->second;
    };

    // 默认字体级联，逐步照抄 fonts.py::discover()
    const std::string want = ascii_lower(trim_ws(default_font));
    if (!want.empty()) {
        for (const auto& f : faces_) {
            if (ascii_lower(f.id) == want || ascii_lower(f.name) == want) {
                default_id_ = f.id;
                break;
            }
        }
    }
    if (default_id_.empty()) {
        // 1) 内置字体目录里真的能画中文的（用户按 README 放了 Noto Sans SC 走这里）
        for (const auto& f : faces_)
            if (f.builtin && can_draw_cjk_cached(path_of(f.id))) { default_id_ = f.id; break; }
        if (default_id_.empty())
            for (const auto& f : faces_)
                if (f.builtin) { default_id_ = f.id; break; }
    }
    if (default_id_.empty()) {
        // 2) 任何真的画得出中文的字体
        for (const auto& f : faces_)
            if (can_draw_cjk_cached(path_of(f.id))) { default_id_ = f.id; break; }
    }
    if (default_id_.empty()) {
        // 3) 字形探测全军覆没时按名字猜（不能直接取列表第一个：Windows 上会是 Arial，
        //    整本中文渲染成方框）
        for (const auto& f : faces_)
            if (name_suggests_cjk(f.id, f.file_name)) { default_id_ = f.id; break; }
    }
    if (default_id_.empty()) default_id_ = faces_.front().id;
}

// ---------------------------------------------------------------- 查询
std::string FontManager::resolve_id(const std::string& family) const {
    // 对齐 fonts.py::get()：空/auto → 默认；精确 id → 大小写不敏感 id 或 name →
    // 子串命中（"notosans" 命中 NotoSansSC-Regular）→ 默认
    std::string fam = ascii_lower(trim_ws(family));
    if (fam.empty() || fam == "auto") return default_id();
    for (const auto& f : faces_)
        if (f.id == family) return f.id;
    for (const auto& f : faces_)
        if (ascii_lower(f.id) == fam || ascii_lower(f.name) == fam) return f.id;
    for (const auto& f : faces_)
        if (ascii_lower(f.id).find(fam) != std::string::npos) return f.id;
    return default_id();
}

std::vector<FontFaceInfo> FontManager::list_infos() const {
    // list_infos 语义：name = id，default 标记命中 default_id 的项
    std::vector<FontFaceInfo> out = faces_;
    const std::string did = default_id();
    for (auto& f : out) f.is_default = (f.id == did);
    return out;
}

std::filesystem::path FontManager::face_path(const std::string& font_id) const {
    // 对齐 fonts.py::get()：未知 id 回落到默认字体（不是 404），
    // 否则 /render/fonts/{未知}/file 两边一个 200 一个 404。
    auto it = impl_->paths.find(resolve_id(font_id));
    return it == impl_->paths.end() ? std::filesystem::path() : it->second;
}

// ---------------------------------------------------------------- 度量
bool FontManager::load(const std::string& font_id, int size_px) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (size_px < 4) size_px = 4;  // 对齐 Python max(4, int(size_px))
    return impl_->ensure(font_id, size_px);
}

double FontManager::char_length(char32_t ch) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!impl_->face) return 0.0;
    auto key = std::make_tuple(impl_->loaded_id, impl_->loaded_size, ch);
    auto cit = impl_->char_cache.find(key);
    if (cit != impl_->char_cache.end()) return cit->second;
    // 无字形 → glyph 0（.notdef）的 advance。Pillow 的 getlength 走 FT_Load_Char，
    // 未映射码位同样落到 .notdef；返回 0 会让含生僻符号的段落宽度与 Python 不同，
    // 进而折行位置、页数与锚点全部漂移（PORT_SPEC §8 高风险点）。
    FT_UInt gindex = FT_Get_Char_Index(impl_->face, ch);
    if (FT_Load_Glyph(impl_->face, gindex, FT_LOAD_DEFAULT) != 0) return 0.0;
    double w = impl_->face->glyph->advance.x / 64.0;  // 26.6 → 像素
    impl_->char_cache.emplace(std::move(key), w);
    return w;
}

double FontManager::char_length_at(const std::string& font_id, int size_px, char32_t ch) {
    // 关键：load + 测量必须在同一个锁内。FontManager 只持有一个 face，
    // 分成两次加锁调用时并发线程会把 face 换成别的字号（见头文件说明）。
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (size_px < 4) size_px = 4;
    if (!impl_->ensure(font_id, size_px)) return 0.0;
    auto key = std::make_tuple(impl_->loaded_id, impl_->loaded_size, ch);
    auto cit = impl_->char_cache.find(key);
    if (cit != impl_->char_cache.end()) return cit->second;
    FT_UInt gindex = FT_Get_Char_Index(impl_->face, ch);
    if (FT_Load_Glyph(impl_->face, gindex, FT_LOAD_DEFAULT) != 0) return 0.0;
    double w = impl_->face->glyph->advance.x / 64.0;
    impl_->char_cache.emplace(std::move(key), w);
    return w;
}

double FontManager::ascent() const {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!impl_->face || !impl_->face->size) return 0.0;
    return impl_->face->size->metrics.ascender / 64.0;
}

double FontManager::descent() const {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    if (!impl_->face || !impl_->face->size) return 0.0;
    // metrics.descender 为负值，返回其绝对值（正值像素）
    return -impl_->face->size->metrics.descender / 64.0;
}

GlyphBitmap FontManager::render_glyph(char32_t ch) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mtx);
    GlyphBitmap out;
    if (!impl_->face) return out;
    // 无字形 → 画 .notdef 方框（Pillow 同样会画出方框），而不是留空
    FT_UInt gindex = FT_Get_Char_Index(impl_->face, ch);
    if (FT_Load_Glyph(impl_->face, gindex, FT_LOAD_RENDER) != 0) return out;
    FT_GlyphSlot g = impl_->face->glyph;
    if (!g->bitmap.buffer || g->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) return out;
    out.left = g->bitmap_left;
    out.top = g->bitmap_top;
    out.width = static_cast<int>(g->bitmap.width);
    out.rows = static_cast<int>(g->bitmap.rows);
    out.pitch = g->bitmap.pitch;
    out.pixels = g->bitmap.buffer;
    // 注意：pixels 指向 FreeType slot 内部缓冲，仅在下次 render_glyph/load
    // 调用（或 FontManager 析构）前有效；调用方不得释放。
    return out;
}

// ---------------------------------------------------------------- CJK 探测
bool FontManager::can_draw_cjk(const std::filesystem::path& font_path) {
    // 光栅化 U+4E2D（"中"）与 U+E123（.notdef 基准），像素有有效差异 → true
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) return false;
    bool result = false;
    std::vector<unsigned char> bytes;
    FT_Face face = nullptr;
    do {
        if (!read_file_bytes(font_path, bytes)) break;
        if (FT_New_Memory_Face(lib, bytes.data(), static_cast<FT_Long>(bytes.size()),
                               0, &face) != 0)
            break;
        FT_Set_Pixel_Sizes(face, kGlyphProbeSize, 0);

        // 光栅化单个字符，返回像素缓冲（行间距按 pitch 压实）；失败返回空
        auto raster = [&](char32_t cp) {
            std::vector<unsigned char> px;
            FT_UInt gi = FT_Get_Char_Index(face, static_cast<FT_ULong>(cp));
            if (gi == 0) return px;
            if (FT_Load_Glyph(face, gi, FT_LOAD_RENDER) != 0) return px;            FT_Bitmap& bm = face->glyph->bitmap;
            if (!bm.buffer || bm.width == 0 || bm.rows == 0) return px;
            if (bm.pixel_mode != FT_PIXEL_MODE_GRAY) return px;
            px.resize(static_cast<size_t>(bm.width) * bm.rows);
            for (unsigned int r = 0; r < bm.rows; ++r)
                std::memcpy(px.data() + r * bm.width, bm.buffer + r * bm.pitch, bm.width);
            return px;
        };

        std::vector<unsigned char> cjk = raster(0x4E2D);
        if (cjk.empty()) break;  // 完全无墨迹：视为不可用
        std::vector<unsigned char> notdef = raster(0xE123);
        if (notdef.size() != cjk.size()) {
            result = true;  // 尺寸不同即说明不是同一个（方框）字形
        } else {
            result = std::memcmp(cjk.data(), notdef.data(), cjk.size()) != 0;
        }
    } while (false);
    if (face) FT_Done_Face(face);
    FT_Done_FreeType(lib);
    return result;
}

}  // namespace lnv
