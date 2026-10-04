#pragma once
// 字体管理（对应 Python layout/fonts.py）
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "lnv/util/textutil.h"

struct FT_LibraryRec_;
typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;
struct FT_GlyphSlotRec_;
typedef struct FT_GlyphSlotRec_* FT_GlyphSlot;

namespace lnv {

// 宽字符码段表（§4.4，必须逐段一致）
bool is_wide_char(char32_t c);

struct FontFaceInfo {
    std::string id;
    std::string name;
    std::string file_name;
    bool is_default = false;
    bool builtin = false;
};

// 一个已加载字号的度量/光栅化句柄。pixels 指向内部缓冲，仅在下次
// render_glyph*/load 之前有效（调用方须立即消费）。
struct GlyphBitmap {
    int left = 0, top = 0;      // 相对基线的偏移
    int width = 0, rows = 0;
    int pitch = 0;              // 每行字节数（8bit 灰度）
    const unsigned char* pixels = nullptr;
};

class FontManager {
public:
    FontManager();
    ~FontManager();
    FontManager(const FontManager&) = delete;
    FontManager& operator=(const FontManager&) = delete;

    // fonts_dir / default_font 来自配置；启动时扫描内置目录 + 系统字体
    void init(const std::filesystem::path& fonts_dir, const std::string& default_font);

    // "auto"/空 → 默认字体 id；未知名 → 容错回退默认。返回解析后的字体 id。
    std::string resolve_id(const std::string& family) const;
    // 对齐 Python fonts.py::default_id —— 没有任何可用字体时返回字面量 "default"
    // （空串会改变 layout_key 的 font 分量并与 Python 分叉，审查 L13）。
    // init() 会重写 default_id_/faces_，读侧也必须持锁（实现见 fonts.cpp）。
    std::string default_id() const;
    // 内部真实值（无字体时为空串），仅用于"是否装了字体"的判断。
    // 按值返回：锁内拷贝，避免锁外引用到会被 init() 改写的 default_id_
    std::string default_id_raw() const;
    std::vector<FontFaceInfo> list_infos() const;

    // 加载 face（id 已解析）；线程安全（内部锁）。size_px 为像素字号。
    // 返回 false 表示字体不可用。
    bool load(const std::string& font_id, int size_px);

    // 以下方法要求先 load 成功；线程安全
    // 字符 advance 宽度（像素，26.6 已转 double）+ letter_spacing 由调用方加
    double char_length(char32_t ch);
    // 指定字体 + 字号下的字符宽度。"加载该字号 + 测量"在同一个锁内完成：
    // FontManager 内部只保留一个已加载 face，分成 load() + char_length() 两次调用
    // 时，另一个线程（12 线程池并发渲染）可能已经把 face 换成别的字号 —— 量出来的
    // 宽度会属于另一个字号，折行位置随调用顺序变化，同一页每次渲染都可能不同。
    double char_length_at(const std::string& font_id, int size_px, char32_t ch);
    // 行高度量（ascent/descent，正值像素）
    double ascent() const;
    double descent() const;  // 正值

    // 光栅化字符；失败（无字形）返回 pixels==nullptr
    GlyphBitmap render_glyph(char32_t ch);

    // 在给定字号下"独占 face"的 RAII 守卫：作用域内其它线程不会切换 face。
    // 渲染一页文本必须用它把 load → 逐个 render_glyph/ascent/descent 包起来，
    // 否则并发渲染不同字号时会把字形画成别的字号（图与排版不一致）。
    // 内部用递归锁，所以守卫持有期间仍可正常调用 char_length/render_glyph。
    class FaceGuard {
    public:
        FaceGuard(FontManager& fm, const std::string& font_id, int size_px);
        ~FaceGuard();
        FaceGuard(const FaceGuard&) = delete;
        FaceGuard& operator=(const FaceGuard&) = delete;
        bool ok() const { return ok_; }

    private:
        FontManager* fm_ = nullptr;
        bool ok_ = false;
        // 按值持有锁：构造期间 ensure() 若抛 bad_alloc，成员析构必然解锁。
        // 早期写成堆上 new std::unique_lock + void*，ensure 抛异常时构造函数
        // 失败、堆上的锁既不解锁也不释放 → 互斥锁永久死锁（整条渲染管线挂死）。
        std::unique_lock<std::recursive_mutex> lock_;
    };

    // 该字体是否"能画中文"（光栅化 U+4E2D 与 .notdef 基准比较）
    static bool can_draw_cjk(const std::filesystem::path& font_path);

    std::filesystem::path face_path(const std::string& font_id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string default_id_;
    std::filesystem::path fonts_dir_;
    std::vector<FontFaceInfo> faces_;
};

}  // namespace lnv
