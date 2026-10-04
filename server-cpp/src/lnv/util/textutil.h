#pragma once
// 文本工具：编码检测、解码、行处理、章节切分（对应 Python app/util/textutil.py）
// 内部统一使用 char32（与 Python 的"按码点索引"语义一致）。
#include <optional>
#include <string>
#include <vector>

namespace lnv {

using U32Text = std::u32string;

struct DecodedText {
    U32Text text;
    std::string encoding;  // 实际使用的编码名（对齐 Python 风格）
};

// BOM 检测：utf-8-sig / utf-32 / utf-16
std::optional<std::string> detect_bom(const std::string& bytes);
// 无 BOM UTF-16 兜底判定：utf-16-le / utf-16-be / nullopt
std::optional<std::string> detect_bomless_utf16(const std::string& bytes);
// BOM → 无 BOM UTF-16 → 启发式（多编码试解码 + 乱度评分，等价 charset-normalizer 的
// 收窄用途）→ ascii → gb18030 兜底
std::string detect_encoding(const std::string& bytes);

// 严格解码（失败返回 nullopt）。enc 见 FALLBACK_ENCODINGS 与 "utf-16-le/be"、
// "utf-32"、"utf-8-sig"、"gb18030"、"big5"、"shift_jis"、"cp949"、"euc_jp"、
// "cp1252"、"ascii"。
std::optional<U32Text> decode_strict(const std::string& bytes, const std::string& enc);

// decode_text：encoding 提示 → 检测 → FALLBACK_ENCODINGS 逐级降级，最后 replace。
DecodedText decode_text(const std::string& bytes, const std::optional<std::string>& encoding);

// 编码名 → Windows 代码页（0 = 未知）。textutil 解码与 fb2 转码共用同一张表；
// 返回值即 UINT（unsigned int），不在此公共头引入 windows.h。
unsigned int codepage_of(const std::string& enc);

// ---------------------------------------------------------------- 行处理
// Python str.isspace 语义的空白判定（str.strip() 与正则 \s 共用，含 0x1C-0x1F）
bool is_py_space(char32_t c);
std::vector<U32Text> split_lines(const U32Text& text);
U32Text normalize_spaces(const U32Text& text);

// ---------------------------------------------------------------- 章节切分
// 行是否是章节标题；是则返回标题
std::optional<U32Text> match_chapter_title(const U32Text& line);

bool is_cjk(const U32Text& text, double ratio = 0.05);
inline size_t count_chars(const U32Text& text) { return text.size(); }

// 搜索命中片段：命中前后各 radius 个字符（按码点）
U32Text snippet_around(const U32Text& text, size_t start, size_t end, size_t radius = 30);

// ------------------------------------------------------------- UTF-8 转换
std::string u32_to_utf8(const U32Text& t);
U32Text utf8_to_u32(const std::string& s);

// 自然排序键（数字感知，目录优先排序在 library 层做）
bool natural_less(const std::string& a, const std::string& b);
// 对 U32 文本的自然比较
bool natural_less_u32(const U32Text& a, const U32Text& b);

}  // namespace lnv
