#include "lnv/util/textutil.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <regex>
#include <windows.h>

namespace lnv {

// ---------------------------------------------------------------- UTF 转换
std::string u32_to_utf8(const U32Text& t) {
    std::string out;
    out.reserve(t.size() * 3);
    char buf[4];
    for (char32_t c : t) {
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
        int n;
        if (c < 0x80) { buf[0] = char(c); n = 1; }
        else if (c < 0x800) {
            buf[0] = char(0xC0 | (c >> 6)); buf[1] = char(0x80 | (c & 0x3F)); n = 2;
        } else if (c < 0x10000) {
            buf[0] = char(0xE0 | (c >> 12)); buf[1] = char(0x80 | ((c >> 6) & 0x3F));
            buf[2] = char(0x80 | (c & 0x3F)); n = 3;
        } else {
            buf[0] = char(0xF0 | (c >> 18)); buf[1] = char(0x80 | ((c >> 12) & 0x3F));
            buf[2] = char(0x80 | ((c >> 6) & 0x3F)); buf[3] = char(0x80 | (c & 0x3F)); n = 4;
        }
        out.append(buf, n);
    }
    return out;
}

U32Text utf8_to_u32(const std::string& s) {
    U32Text out;
    out.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char b = s[i];
        char32_t c; size_t len;
        if (b < 0x80) { c = b; len = 1; }
        else if ((b >> 5) == 0x6) { c = b & 0x1F; len = 2; }
        else if ((b >> 4) == 0xE) { c = b & 0x0F; len = 3; }
        else if ((b >> 3) == 0x1E) { c = b & 0x07; len = 4; }
        else { out.push_back(0xFFFD); i++; continue; }
        if (i + len > n) { out.push_back(0xFFFD); break; }
        bool ok = true;
        for (size_t k = 1; k < len; k++) {
            unsigned char cc = s[i + k];
            if ((cc >> 6) != 2) { ok = false; break; }
            c = (c << 6) | (cc & 0x3F);
        }
        if (!ok || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) { out.push_back(0xFFFD); i++; continue; }
        out.push_back(c);
        i += len;
    }
    return out;
}

// ------------------------------------------------------------- 编码解码
namespace {

std::optional<U32Text> from_wide(const std::wstring& w) {
    U32Text out;
    out.reserve(w.size());
    for (size_t i = 0; i < w.size(); i++) {
        wchar_t wc = w[i];
        if (wc >= 0xD800 && wc <= 0xDBFF && i + 1 < w.size() &&
            w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
            out.push_back(0x10000 + ((char32_t(wc) - 0xD800) << 10) + (char32_t(w[i+1]) - 0xDC00));
            i++;
        } else if (wc >= 0xD800 && wc <= 0xDFFF) {
            out.push_back(0xFFFD);
        } else {
            out.push_back(char32_t(wc));
        }
    }
    return out;
}

// UTF-8 严格校验（过长编码/代理区/越界一律拒绝）。start 用于跳过 BOM 前缀，
// 避免 utf-8-sig 路径为了去掉 3 字节 BOM 把整书再拷贝一份。
std::optional<U32Text> decode_utf8_strict(const std::string& bytes, size_t start) {
    U32Text out;
    out.reserve(bytes.size());
    size_t i = start, n = bytes.size();
    while (i < n) {
        unsigned char b = bytes[i];
        char32_t c; size_t len;
        if (b < 0x80) { c = b; len = 1; }
        else if ((b >> 5) == 0x6) { c = b & 0x1F; len = 2; }
        else if ((b >> 4) == 0xE) { c = b & 0x0F; len = 3; }
        else if ((b >> 3) == 0x1E) { c = b & 0x07; len = 4; }
        else return std::nullopt;
        if (i + len > n) return std::nullopt;
        for (size_t k = 1; k < len; k++) {
            unsigned char cc = bytes[i + k];
            if ((cc >> 6) != 2) return std::nullopt;
            c = (c << 6) | (cc & 0x3F);
        }
        // 过滤过长编码 / 代理区 / 越界
        if ((len == 2 && c < 0x80) || (len == 3 && c < 0x800) ||
            (len == 4 && c < 0x10000) || c > 0x10FFFF ||
            (c >= 0xD800 && c <= 0xDFFF)) return std::nullopt;
        out.push_back(c);
        i += len;
    }
    return out;
}

}  // namespace

// 编码名 → Windows 代码页（0 = 未知）。这就是 textutil 与 fb2 共用的唯一映射表
// （原 fb2.cpp 的 code_page_of 已删除并入此处）：既有名称的取值与优先级不得改动。
UINT codepage_of(const std::string& enc) {
    std::string e;
    e.reserve(enc.size());
    for (char c : enc) e.push_back(char(::tolower((unsigned char)c)));
    // textutil 原有名称（取值与优先级保持不变；后附原 fb2 独有别名）
    if (e == "ascii") return 20127;        // US-ASCII 严格
    if (e == "cp1252" || e == "windows-1252" || e == "latin-1" || e == "latin1")
        return 1252;
    // GB18030 用专用 CP 54936（失败回退 GBK 936，见 decode_strict）
    if (e == "gb18030") return 54936;
    if (e == "gbk" || e == "gb2312" || e == "cp936" || e == "gb2312-80" ||
        e == "gb_2312-80" || e == "x-gbk") return 936;
    if (e == "big5" || e == "big5hkscs" || e == "cp950" || e == "big5-hkscs" ||
        e == "cn-big5") return 950;
    if (e == "shift_jis" || e == "sjis" || e == "cp932" || e == "shift_jis_2004" ||
        e == "shift_jisx0213" || e == "mskanji" || e == "shift-jis" ||
        e == "ms_kanji" || e == "x-sjis") return 932;
    // 日文 EUC-JP（Windows CP51932）。Python charset-normalizer 候选里有 euc_jp，
    // 短日文样本它常与 gb18030 同为零乱度，候选表缺了它就只能按平票规则判给中文。
    if (e == "euc_jp" || e == "euc-jp" || e == "eucjp" || e == "ujis" || e == "u_jis" ||
        e == "x-euc-jp") return 51932;
    // 韩文（charset-normalizer 对 cp949 文本返回 "cp949"）
    if (e == "cp949" || e == "euc_kr" || e == "euc-kr" || e == "uhc" || e == "ms949" ||
        e == "ks_c_5601-1987" || e == "euckr") return 949;
    // 俄文（原 fb2 表）
    if (e == "koi8-r" || e == "koi8r") return 20866;
    if (e == "koi8-u" || e == "koi8ru" || e == "koi8-ru") return 21866;
    // windows-NNN / cpNNN / win-NNN / x-cpNNN（原 fb2 表）
    auto digits_after = [&](const char* prefix) -> int {
        size_t pn = std::strlen(prefix);
        if (e.size() <= pn || e.compare(0, pn, prefix) != 0) return -1;
        const char* s = e.c_str() + pn;
        int v = 0;
        for (const char* q = s; *q; ++q) {
            if (*q < '0' || *q > '9') return -1;
            v = v * 10 + (*q - '0');
            if (v > 65535) return -1;
        }
        return v > 0 ? v : -1;
    };
    for (const char* p : {"windows-", "cp", "win-", "x-cp"}) {
        int v = digits_after(p);
        if (v > 0) return UINT(v);
    }
    // iso-8859-N（原 fb2 表；N=12 无对应代码页）
    if (e.rfind("iso-8859-", 0) == 0) {
        static const int kIso[] = {28591, 28592, 28593, 28594, 28595, 28596, 28597,
                                   28598, 28599, 28600, 28601, -1,    28603, 28604,
                                   28605, 28606};
        int num = digits_after("iso-8859-");
        if (num >= 1 && num <= 16 && kIso[num - 1] > 0) return UINT(kIso[num - 1]);
    }
    return 0;
}

std::optional<U32Text> decode_strict(const std::string& bytes, const std::string& enc) {
    if (bytes.empty()) return U32Text();
    std::string e;
    e.reserve(enc.size());
    for (char c : enc) e.push_back(char(::tolower((unsigned char)c)));

    // Python 风格别名：config 的 encoding_overrides 与 parsed 缓存里的 meta.encoding
    // 会写 "utf_8"/"utf_8_sig"（下划线），必须与 "utf-8" 等价，否则覆盖配置静默失效。
    if (e == "utf_8" || e == "utf8") e = "utf-8";
    else if (e == "utf_8_sig" || e == "utf8-sig" || e == "utf8_sig") e = "utf-8-sig";
    else if (e == "utf_16" || e == "utf16") e = "utf-16";
    else if (e == "utf_16_le") e = "utf-16-le";
    else if (e == "utf_16_be") e = "utf-16-be";
    else if (e == "utf_32" || e == "utf32") e = "utf-32";
    else if (e == "utf_32_le") e = "utf-32-le";
    else if (e == "utf_32_be") e = "utf-32-be";
    else if (e == "latin_1" || e == "iso8859-1" || e == "iso-8859-1") e = "cp1252";

    // UTF-16 / UTF-32 家族手工解码（与 Python codecs 语义对齐：接受 BE/LE，
    // 消费 BOM 由上层 lstrip \ufeff 处理）
    //
    // 代理对必须**合并成一个码点**（否则星平面字符被拆成两个码点，其后所有
    // char 锚点整体偏移），孤立代理按 Python 一样判为解码失败（审查 H6）。
    auto decode_utf16_units = [](const std::vector<uint16_t>& units) -> std::optional<U32Text> {
        U32Text out;
        out.reserve(units.size());
        for (size_t i = 0; i < units.size(); i++) {
            uint16_t u = units[i];
            if (u >= 0xD800 && u <= 0xDBFF) {
                if (i + 1 >= units.size()) return std::nullopt;      // 孤立高代理（截断）
                uint16_t lo = units[i + 1];
                if (lo < 0xDC00 || lo > 0xDFFF) return std::nullopt;  // 高代理后不是低代理
                out.push_back(char32_t(0x10000) + ((char32_t(u) - 0xD800) << 10) +
                              (char32_t(lo) - 0xDC00));
                i++;
            } else if (u >= 0xDC00 && u <= 0xDFFF) {
                return std::nullopt;                                  // 孤立低代理
            } else {
                out.push_back(u);
            }
        }
        return out;
    };

    if (e == "utf-16-le" || e == "utf-16-be") {
        if (bytes.size() % 2) return std::nullopt;
        bool le = e == "utf-16-le";
        std::vector<uint16_t> units;
        units.reserve(bytes.size() / 2);
        for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
            unsigned char b0 = (unsigned char)bytes[i], b1 = (unsigned char)bytes[i + 1];
            units.push_back(uint16_t(le ? (b0 | (b1 << 8)) : ((b0 << 8) | b1)));
        }
        return decode_utf16_units(units);
    }
    if (e == "utf-16" || e == "utf-32" || e == "utf-32-le" || e == "utf-32-be") {
        // 只用偏移量剥 BOM，不整书拷贝（原实现的 `std::string data = bytes`
        // 会让解码峰值多出一份文件）。
        size_t off = 0;
        bool be = e == "utf-32-be";
        if (e == "utf-16") {
            if (bytes.size() >= 2) {
                if ((unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE) { be = false; off = 2; }
                else if ((unsigned char)bytes[0] == 0xFE && (unsigned char)bytes[1] == 0xFF) { be = true; off = 2; }
                else return decode_strict(bytes, "utf-16-le");
            }
            if ((bytes.size() - off) % 2) return std::nullopt;
            std::vector<uint16_t> units;
            units.reserve((bytes.size() - off) / 2);
            for (size_t i = off; i + 1 < bytes.size(); i += 2) {
                unsigned char b0 = (unsigned char)bytes[i], b1 = (unsigned char)bytes[i + 1];
                units.push_back(uint16_t(be ? ((b0 << 8) | b1) : (b0 | (b1 << 8))));
            }
            return decode_utf16_units(units);
        }
        // UTF-32：BOM 决定字节序并消费；无 BOM 时 Python 的 utf-32 按本机字节序
        // （Windows/x86 = LE）。原实现固定按 LE 解，UTF-32BE 整书必是垃圾码点。
        if (e == "utf-32" && bytes.size() >= 4) {
            if ((unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE &&
                (unsigned char)bytes[2] == 0x00 && (unsigned char)bytes[3] == 0x00) { be = false; off = 4; }
            else if ((unsigned char)bytes[0] == 0x00 && (unsigned char)bytes[1] == 0x00 &&
                     (unsigned char)bytes[2] == 0xFE && (unsigned char)bytes[3] == 0xFF) { be = true; off = 4; }
        }
        if ((bytes.size() - off) % 4) return std::nullopt;
        U32Text out;
        out.reserve((bytes.size() - off) / 4);
        for (size_t i = off; i + 3 < bytes.size(); i += 4) {
            char32_t u = be ? ((char32_t((unsigned char)bytes[i]) << 24) | (char32_t((unsigned char)bytes[i+1]) << 16) |
                               (char32_t((unsigned char)bytes[i+2]) << 8) | (unsigned char)bytes[i+3])
                            : (char32_t((unsigned char)bytes[i]) | (char32_t((unsigned char)bytes[i+1]) << 8) |
                               (char32_t((unsigned char)bytes[i+2]) << 16) | (char32_t((unsigned char)bytes[i+3]) << 24));
            // Python 3.4+ 的 utf-32 解码器拒绝孤立代理与 >U+10FFFF（bpo-12892）：
            // 不校验会把 0xFFFE0000 之类的值当作"解码成功"写进正文，从而跳过降级。
            if (u > 0x10FFFF || (u >= 0xD800 && u <= 0xDFFF)) return std::nullopt;
            out.push_back(u);
        }
        return out;
    }
    if (e == "utf-8-sig") {
        if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB &&
            (unsigned char)bytes[2] == 0xBF) return decode_utf8_strict(bytes, 3);
        return decode_utf8_strict(bytes, 0);
    }
    if (e == "utf-8") return decode_utf8_strict(bytes, 0);

    UINT cp = codepage_of(e);
    if (cp == 0) return std::nullopt;
    int wlen = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, bytes.data(),
                                   (int)bytes.size(), nullptr, 0);
    if (cp == 54936 && wlen == 0) {
        // 老 Windows 无 GB18030 独立代码页时回退 GBK
        wlen = MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, bytes.data(),
                                   (int)bytes.size(), nullptr, 0);
        cp = 936;
    }
    if (wlen <= 0) return std::nullopt;
    std::wstring w(wlen, L'\0');
    if (MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(),
                            w.data(), wlen) <= 0)
        return std::nullopt;
    return from_wide(w);
}

std::optional<std::string> detect_bom(const std::string& d) {
    auto starts = [&](const char* b, size_t n) {
        return d.size() >= n && std::memcmp(d.data(), b, n) == 0;
    };
    if (starts("\xEF\xBB\xBF", 3)) return std::string("utf-8-sig");
    if (starts("\xFF\xFE\x00\x00", 4) || starts("\x00\x00\xFE\xFF", 4))
        return std::string("utf-32");
    if (starts("\xFF\xFE", 2) || starts("\xFE\xFF", 2)) return std::string("utf-16");
    return std::nullopt;
}

namespace {
bool plausible_text(const U32Text& t) {
    if (t.empty()) return false;
    size_t bad = 0;
    for (char32_t c : t)
        if (c < 32 && c != U'\t' && c != U'\r' && c != U'\n' && c != U'\f' && c != U'\v') bad++;
    return bad / (double)t.size() < 0.02;
}

// ---- 与 charset-normalizer mess 检测器的窄对齐 ----------------------------
// Python 侧 chaos 是多个插件之和；这里只近似与本项目决策相关的四个：
// Unprintable（控制符/FFFD/PUA）、CjkUncommon（非常用汉字占比）、
// TooManyAccentuated（重音字母占比）、TooManySymbolOrPunctuation（符号/标点
// 占比）。原实现只数控制符/FFFD/谚文，Big5/SJIS 被 GB18030 误读出的"错字
// 汉字"全部零乱度，平票时又被固定 rank 判给 gb18030（审查 major）。

bool is_cjk_ideograph(char32_t c) {
    return (c >= 0x2E80 && c <= 0x2EFF) ||    // CJK 部首补充
           (c >= 0x31C0 && c <= 0x31EF) ||    // CJK 笔画
           (c >= 0x3400 && c <= 0x4DBF) ||    // 扩展 A
           (c >= 0x4E00 && c <= 0x9FFF) ||    // 基本区
           (c >= 0xF900 && c <= 0xFAFF) ||    // 兼容表意
           (c >= 0x20000 && c <= 0x323AF);    // 扩展 B..H（含兼容补充）
}

// charset-normalizer 3.4.x COMMON_CJK_CHARACTERS 的三段并集。非常用汉字是
// "错字汉字"最直接的信号：正确解码的中文正文绝大多数命中该集合，而误读出的
// 生僻字不命中。
const U32Text& common_cjk_table() {
    static const char* kCommon =
        "的一是在不了有和人这中大为上个国我以要他时来用们生到作地于出就分对成会可主发年动同工也能下过子说产种面而方后多定行学法所民得经十三之进着等部度家电力里如水化高自二理起小物现实加量都两体制机当使点从业本去把性好应开它合还因由其些然前外天政四日那社义事平形相全表间样与关各重新线内数正心反你明看原又么利比或但质气第向道命此变条只没结解问意建月公无系军很情者最立代想已通并提直题党程展五果料象员革位入常文总次品式活设及管特件长求老头基资边流路级少图山统接知较将组见计别她手角期根论运农指几九区强放决西被干做必战先回则任取据处队南给色光门即保治北造百规热领七海口东导器压志世金增争济阶油思术极交受联什认六共权收证改清己美再采转更单风切打白教速花带安场身车例真务具万每目至达走积示议声报斗完类八离华名确才科张信马节话米整空元况今集温传土许步群广石记需段研界拉林律叫且究观越织装影算低持音众书布复容儿须际商非验连断深难近矿千周委素技备半办青省列习响约支般史感劳便团往酸历市克何除消构府太准精值号率族维划选标写存候毛亲快效斯院查江型眼王按格养易置派层片始却专状育厂京识适属圆包火住调满县局照参红细引听该铁价严龙飞"
        "日一国年大十二本中長出三時行見月分後前生五間上東四今金九入学高円子外八六下来気小七山話女北午百書先名川千水半男西電校語土木聞食車何南万毎白天母火右読友左休父雨"
        "一二三四五六七八九十百千萬上下左右中人女子大小山川日月火水木金土父母天地國名年時文校學生";
    static const U32Text table = [] {
        U32Text v = utf8_to_u32(kCommon);
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    }();
    return table;
}

bool is_common_cjk(char32_t c) {
    const U32Text& t = common_cjk_table();
    return std::binary_search(t.begin(), t.end(), c);
}

// 近似 TooManyAccentuatedPlugin 的"带重音拉丁字母"（名称表无法内嵌，用码点
// 区间代替 unicodedata.name 匹配）；判罚阈值与 Python 一致（≥0.35）。
bool is_accented_latin(char32_t c) {
    if (c >= 0xC0 && c <= 0xFF) return c != 0xD7 && c != 0xF7;
    return c >= 0x100 && c <= 0x17F;
}

// 近似 TooManySymbolOrPunctuationPlugin：标点计 1、符号计 2，占比 ≥0.3 计罚。
// cp1252 误读 CJK 字节会解出大量 Latin-1 符号（¤ ¬ ² ´ ¼ …），Python 正是靠
// 该插件把 cp1252 候选排除，否则它会以"零乱度"抢走 CJK 候选的平票。
int sym_punct_weight(char32_t c) {
    if (c < 0x80) {
        return ((c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) ||
                (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E)) ? 1 : 0;
    }
    if (c >= 0xA1 && c <= 0xBF) {
        if (c == 0xAA || c == 0xB5 || c == 0xBA) return 0;  // ª µ º 是字母
        return (c == 0xA1 || c == 0xBF || c == 0xA7 || c == 0xB7) ? 1 : 2;
    }
    if (c == 0xD7 || c == 0xF7) return 2;  // × ÷
    if ((c >= 0x2000 && c <= 0x206F) || (c >= 0x20A0 && c <= 0x20CF) ||
        (c >= 0x2100 && c <= 0x214F) || (c >= 0x2190 && c <= 0x2BFF) ||
        (c >= 0x3000 && c <= 0x303F) || (c >= 0xFE30 && c <= 0xFE4F) ||
        (c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) ||
        (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65))
        return 1;
    return 0;
}

// 乱度启发式：解码后"可疑字符"占比（控制符/FFFD/PUA 是强错误信号，
// 非常用汉字、重音字母、符号标点按 charset-normalizer 插件口径补罚）。
// hangul_ok：谚文只对韩文候选（cp949）和有效 UTF-8 是合理字符；GBK/Big5 字节
// 被 UTF-16BE 误读、中文被 cp949 误读都会整段变 Hangul，对其它候选必须计罚。
double chaos_of(const U32Text& t, bool hangul_ok) {
    if (t.empty()) return 1.0;
    size_t bad = 0, accented = 0, sym = 0;
    size_t cjk = 0, uncommon = 0;
    for (char32_t c : t) {
        if (c == 0xFFFD ||
            (c < 32 && c != U'\t' && c != U'\r' && c != U'\n' && c != U'\f' && c != U'\v') ||
            (c >= 0xE000 && c <= 0xF8FF)) { bad++; continue; }
        if (!hangul_ok && c >= 0xAC00 && c <= 0xD7AF) { bad++; continue; }
        if (is_cjk_ideograph(c)) {
            cjk++;
            if (!is_common_cjk(c)) uncommon++;
            continue;
        }
        if (is_accented_latin(c)) accented++;
        sym += size_t(sym_punct_weight(c));
    }
    double score = bad / (double)t.size();
    // CjkUncommonPlugin：(2u - n) / (5*max(n,16))，只罚"非常用字过量"，
    // 容忍正常繁体书里接近一半的非常用字。
    if (cjk >= 4) {
        double u = (2.0 * uncommon - cjk) / (5.0 * std::max<size_t>(cjk, 16));
        if (u > 0) score += u;
    }
    if (t.size() >= 8 && accented / (double)t.size() >= 0.35)
        score += accented / (double)t.size();
    double sym_ratio = sym / (double)t.size();
    if (sym_ratio >= 0.3) score += sym_ratio;
    return score;
}

struct Candidate { std::string enc; double chaos; double multibyte; };

// UTF-8 前缀校验：返回"完整且合法"的字节数；尾部被截断的不完整序列按合法处理
// （不计入长度），真正的编码错误（非法首字节/坏续字节/过长编码/代理区/越界）返回 SIZE_MAX。
// 用途：编码探测的采样是前 64 KiB，正好切断一个多字节字符时**不能**据此否定 UTF-8 ——
// 否则整本 UTF-8 中文书会被判成 cp1252（乱码 + 页数错，见审查 B2）。
size_t utf8_valid_prefix_len(const std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char b = (unsigned char)s[i];
        if (b < 0x80) { i++; continue; }
        size_t need;
        char32_t c;
        if ((b >> 5) == 0x6) { c = b & 0x1F; need = 2; }
        else if ((b >> 4) == 0xE) { c = b & 0x0F; need = 3; }
        else if ((b >> 3) == 0x1E) { c = b & 0x07; need = 4; }
        else return SIZE_MAX;
        if (i + need > n) return i;   // 尾部截断 → 合法前缀
        for (size_t k = 1; k < need; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc >> 6) != 2) return SIZE_MAX;
            c = (c << 6) | (cc & 0x3F);
        }
        if ((need == 2 && c < 0x80) || (need == 3 && c < 0x800) ||
            (need == 4 && c < 0x10000) || c > 0x10FFFF ||
            (c >= 0xD800 && c <= 0xDFFF)) return SIZE_MAX;
        i += need;
    }
    return i;
}

bool is_valid_utf8_sample(const std::string& d) {
    return utf8_valid_prefix_len(d) != SIZE_MAX;
}

bool is_pure_ascii(const std::string& d) {
    for (unsigned char c : d) if (c >= 0x80) return false;
    return true;
}
}  // namespace

std::optional<std::string> detect_bomless_utf16(const std::string& data) {
    std::string sample = data.substr(0, 8192);
    if (sample.size() < 8) return std::nullopt;
    if (sample.size() % 2) sample.pop_back();
    size_t half = sample.size() / 2;
    if (half == 0) return std::nullopt;
    size_t nul_even = 0, nul_odd = 0;
    for (size_t i = 0; i < sample.size(); i += 2) {
        if (sample[i] == 0) nul_even++;
        if (sample[i + 1] == 0) nul_odd++;
    }
    std::vector<std::string> candidates;
    if (nul_odd / (double)half >= 0.2) candidates.push_back("utf-16-le");
    if (nul_even / (double)half >= 0.2) candidates.push_back("utf-16-be");
    for (auto& enc : candidates) {
        auto t = decode_strict(sample, enc);
        if (t && plausible_text(*t)) return enc;
    }
    return std::nullopt;
}

std::string detect_encoding(const std::string& data) {
    if (auto bom = detect_bom(data)) return *bom;
    if (auto u16 = detect_bomless_utf16(data)) return *u16;

    // 启发式（替代 charset-normalizer）：候选编码试解码，乱度最低者胜；
    // 平票按 multi_byte_usage + 候选顺序，并对韩/中冲突做收窄修正（详见下）。
    std::string sample = data.substr(0, 65536);
    // 纯 ASCII：charset-normalizer 会报 "ascii"，这里同样先判（否则 cp1252 也能解码，
    // 会把纯 ASCII 书标成 cp1252，与 Python 的 meta.encoding 不一致）。
    if (is_pure_ascii(sample)) return "ascii";
    std::vector<Candidate> cands;
    // 候选集合按 Python charset-normalizer 的实测行为补齐：
    // - cp949 不能少：没有韩文编码时，整本韩文小说会被当中文解出乱码；
    // - euc_jp 不能少：短日文（EUC-JP）与 gb18030 常同为零乱度，缺了它日文会被
    //   平票规则判给中文（Python 的 test_adversarial_fixes 编码矩阵要求 euc_jp）；
    // - utf_16_le/be 不能少：无 BOM 的中文 UTF-16 高低字节都不为 0，
    //   detect_bomless_utf16 的 NUL 车道启发必然漏检，Python 侧靠
    //   charset-normalizer 试 utf_16_le/be 兜住（审查 minor）。名字用下划线，
    //   和它报出的 meta.encoding（"utf_16_le"，不是 "utf-16-le"）逐字节一致。
    for (const char* enc : {"utf-8", "gb18030", "big5", "shift_jis", "cp949",
                            "cp1252", "euc_jp", "utf_16_be", "utf_16_le"}) {
        const std::string name(enc);
        // 谚文只对韩文候选（与有效 UTF-8）是合理字符
        const bool hangul_ok = name == "utf-8" || name == "cp949";
        if (name == "utf-8") {
            // 采样在 64 KiB 处可能切断多字节字符：按"尾部允许不完整序列"校验并只解码
            // 完整部分，否则 utf-8 会因严格校验失败而落选（审查 B2）。
            size_t ok = utf8_valid_prefix_len(sample);
            if (ok == SIZE_MAX) continue;
            auto t = decode_strict(sample.substr(0, ok), enc);
            if (t) cands.push_back({name, chaos_of(*t, hangul_ok),
                                    1.0 - (double)t->size() / (double)sample.size()});
            continue;
        }
        auto t = decode_strict(sample, enc);
        if (t) cands.push_back({name, chaos_of(*t, hangul_ok),
                                1.0 - (double)t->size() / (double)sample.size()});
    }
    if (!cands.empty()) {
        // 平票规则对齐 Python 的 CharsetMatch.__lt__ + _normalizer_choice：
        // 1) 乱度相同者里 multi_byte_usage（1 - 解码字符数/字节数）大者优先 ——
        //    cp1252 单字节解码占比 0，不能抢走 gb18030/big5（≈0.5）的平票；
        // 2) 仍平票按候选顺序（IANA 字母序，utf_8 例外优先）；
        // 3) 窄修正：平票候选同时含韩文族与中文族时选中文（短 GB18030 中文与
        //    cp949 的已知冲突）；其它平票尊重顺序，避免为修中文读坏日文。
        double best_chaos = cands.front().chaos;
        for (const Candidate& c : cands)
            best_chaos = std::min(best_chaos, c.chaos);
        std::vector<const Candidate*> tied;
        for (const Candidate& c : cands)
            if (std::fabs(c.chaos - best_chaos) < 1e-9) tied.push_back(&c);
        bool has_korean = false, has_chinese = false;
        for (const Candidate* c : tied) {
            has_korean = has_korean || c->enc == "cp949";
            has_chinese = has_chinese || c->enc == "gb18030" || c->enc == "big5";
        }
        // 平票时的最终顺序 = Python charset-normalizer 的稳定顺序：utf_8 有
        // priority +1，其余按 IANA 名称字母序（big5 < cp1252 < cp949 < euc_jp <
        // gb18030 < shift_jis < utf_16_be < utf_16_le）。gb18030 不再恒胜，
        // 这与 Python 一致，重排的目的就是消除旧的系统偏差。
        auto rank = [](const std::string& e) {
            if (e == "utf-8") return 0;
            if (e == "big5") return 1;
            if (e == "cp1252") return 2;
            if (e == "cp949") return 3;
            if (e == "euc_jp") return 4;
            if (e == "gb18030") return 5;
            if (e == "shift_jis") return 6;
            if (e == "utf_16_be") return 7;
            return 8;  // utf_16_le
        };
        const Candidate* best = nullptr;
        for (const Candidate* c : tied) {
            if (has_korean && has_chinese && c->enc != "gb18030" && c->enc != "big5")
                continue;
            if (!best || c->multibyte > best->multibyte + 1e-9 ||
                (std::fabs(c->multibyte - best->multibyte) <= 1e-9 &&
                 rank(c->enc) < rank(best->enc)))
                best = c;
        }
        // 标签对齐 Python：charset-normalizer 报 "utf_8"（不是 "utf-8"），
        // 这样两边写出的 parsed 缓存 meta.encoding 逐字节相同。
        if (best->enc == "utf-8") return "utf_8";
        return best->enc;
    }
    if (is_valid_utf8_sample(sample)) return "ascii";
    return "gb18030";
}

DecodedText decode_text(const std::string& bytes, const std::optional<std::string>& encoding) {
    // 与 Python FALLBACK_ENCODINGS 逐项对齐（多一个 cp949 会让"双方都解不出"的样本
    // 出现不同的兜底结果）
    static const char* kFallback[] = {"utf-8", "gb18030", "big5", "shift_jis", "cp1252"};
    std::vector<std::string> candidates;
    if (encoding && !encoding->empty()) candidates.push_back(*encoding);
    candidates.push_back(detect_encoding(bytes));
    for (const char* f : kFallback) candidates.push_back(f);

    std::vector<std::string> tried;
    for (const auto& enc : candidates) {
        std::string low;
        low.reserve(enc.size());
        for (char c : enc) low.push_back(char(::tolower((unsigned char)c)));
        if (std::find(tried.begin(), tried.end(), low) != tried.end()) continue;
        tried.push_back(low);
        auto t = decode_strict(bytes, enc);
        if (!t) continue;
        // 消费可能残留的 BOM（一次 erase；逐个 erase(begin) 每轮都要整体搬移）
        size_t b = 0;
        while (b < t->size() && (*t)[b] == 0xFEFF) b++;
        if (b) t->erase(0, b);
        // move：否则整书 U32 文本还会被拷进返回值（峰值 +1×）
        return {std::move(*t), enc};
    }
    // 最后兜底：UTF-8 replace
    U32Text t = utf8_to_u32(bytes);
    size_t b = 0;
    while (b < t.size() && t[b] == 0xFEFF) b++;
    if (b) t.erase(0, b);
    return {std::move(t), "utf-8/replace"};
}

// ---------------------------------------------------------------- 行处理
std::vector<U32Text> split_lines(const U32Text& text) {
    std::vector<U32Text> out;
    U32Text cur;
    for (size_t i = 0; i < text.size(); i++) {
        char32_t c = text[i];
        if (c == U'\r') {
            if (i + 1 < text.size() && text[i + 1] == U'\n') i++;
            out.push_back(std::move(cur));
            cur = U32Text();
        } else if (c == U'\n') {
            out.push_back(std::move(cur));
            cur = U32Text();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(std::move(cur));
    return out;
}

U32Text normalize_spaces(const U32Text& text) {
    U32Text t;
    t.reserve(text.size());
    for (char32_t c : text) {
        if (c == U'\t') { t += U"    "; continue; }
        t.push_back(c == 0x3000 ? U' ' : c);
    }
    // 压缩 [ \f\v]{2,} → 单空格
    U32Text out;
    out.reserve(t.size());
    size_t i = 0;
    while (i < t.size()) {
        char32_t c = t[i];
        if (c == U' ' || c == U'\f' || c == U'\v') {
            size_t j = i;
            while (j < t.size() && (t[j] == U' ' || t[j] == U'\f' || t[j] == U'\v')) j++;
            if (j - i >= 2) out.push_back(U' ');
            else out.push_back(c);
            i = j;
        } else {
            out.push_back(c);
            i++;
        }
    }
    // strip
    size_t b = 0, e = out.size();
    auto is_sp = [](char32_t c) { return c == U' ' || c == U'\f' || c == U'\v'; };
    while (b < e && is_sp(out[b])) b++;
    while (e > b && is_sp(out[e - 1])) e--;
    return out.substr(b, e - b);
}

// ---------------------------------------------------------------- 章节切分
namespace {
// 对齐 Python 正则（按码点语义）：
//   ^\s*第[零一二三四五六七八九十百千万两0-9]{1,10}[章卷回节部篇集]\s*.{0,30}$
//   ^\s*Chapter\s+\d+.{0,30}$  (ignorecase)
//   ^\s*(序章|序言|前言|引子|楔子|后记|尾声|番外|附录).{0,20}$
bool is_cjk_num(char32_t c) {
    static const char32_t* nums = U"零一二三四五六七八九十百千万两";
    if (c >= U'0' && c <= U'9') return true;
    return std::u32string(nums).find(c) != std::u32string::npos;
}
// Python str 模式的 \d 匹配 Unicode 十进制数字（含全角 ０-９）；Pattern 1 的
// [0-9] 只认 ASCII。两者不要混用，否则两边识别的章节集合不一致。
bool is_unicode_digit(char32_t c) {
    if (c >= U'0' && c <= U'9') return true;
    return c >= 0xFF10 && c <= 0xFF19;
}
}  // namespace

// Python str.isspace 语义的 Unicode 空白集合（str.strip()/正则 \s 与 txt.cpp 共用）。
// 只认 ' ' '\t' 与全角空格时，"\xa0第1章 测试"（网页粘贴常见的 NBSP 前导）在 Python
// 是章节标题、在 C++ 是普通段落 → 章节列表、每章 total_chars 与全部页锚点都不同。
// \x1c-\x1f 也是 Python 的空白，漏掉会让 meta/document 的字数与 Python 计数口径不一致。
bool is_py_space(char32_t c) {
    switch (c) {
        case U'\t': case U'\n': case U'\v': case U'\f': case U'\r':
        case U'\x1C': case U'\x1D': case U'\x1E': case U'\x1F':
        case U' ': case U'\x85': case U'\xA0': case U'\x1680':
        case U'\x2028': case U'\x2029': case U'\x202F': case U'\x205F': case U'\x3000':
            return true;
        default:
            return c >= 0x2000 && c <= 0x200A;
    }
}

std::optional<U32Text> match_chapter_title(const U32Text& line_in) {
    // strip
    U32Text line = line_in;
    size_t b = 0, e = line.size();
    while (b < e && is_py_space(line[b])) b++;
    while (e > b && is_py_space(line[e - 1])) e--;
    line = line.substr(b, e - b);
    if (line.empty() || line.size() > 60) return std::nullopt;

    // Pattern 1: 第X章/卷/回/节/部/篇/集
    if (!line.empty() && line[0] == U'第') {
        size_t i = 1, cnt = 0;
        while (i < line.size() && cnt < 10 && is_cjk_num(line[i])) { i++; cnt++; }
        if (cnt >= 1 && cnt <= 10 && i < line.size()) {
            char32_t tag = line[i];
            static const char32_t* tags = U"章卷回节部篇集";
            if (std::u32string(tags).find(tag) != std::u32string::npos) {
                size_t j = i + 1;
                while (j < line.size() && is_py_space(line[j])) j++;
                size_t rest = line.size() - j;
                if (rest <= 30) return line;
            }
        }
    }
    // Pattern 2: Chapter\s+\d+...
    {
        size_t i = 0;
        auto starts_with = [&](const char32_t* p, size_t plen, size_t pos) {
            if (pos + plen > line.size()) return false;
            for (size_t k = 0; k < plen; k++) {
                char32_t a = line[pos + k], bch = p[k];
                if (a >= U'A' && a <= U'Z') a = a - U'A' + U'a';
                if (bch >= U'A' && bch <= U'Z') bch = bch - U'A' + U'a';
                if (a != bch) return false;
            }
            return true;
        };
        if (starts_with(U"chapter", 7, i)) {
            i += 7;
            size_t sp = i;
            while (i < line.size() && is_py_space(line[i])) i++;
            if (i > sp) {
                size_t d0 = i;
                while (i < line.size() && is_unicode_digit(line[i])) i++;
                if (i > d0 && line.size() - i <= 30) return line;
            }
        }
    }
    // Pattern 3: 特殊章节名
    {
        static const char32_t* heads[] = {U"序章", U"序言", U"前言", U"引子", U"楔子",
                                          U"后记", U"尾声", U"番外", U"附录"};
        for (auto* h : heads) {
            std::u32string hs(h);
            if (line.size() >= hs.size() && line.compare(0, hs.size(), hs) == 0) {
                if (line.size() - hs.size() <= 20) return line;
            }
        }
    }
    return std::nullopt;
}

bool is_cjk(const U32Text& text, double ratio) {
    if (text.empty()) return false;
    size_t sample_n = std::min<size_t>(text.size(), 20000);
    size_t cjk = 0;
    for (size_t i = 0; i < sample_n; i++) {
        char32_t c = text[i];
        if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3040 && c <= 0x30FF)) {
            cjk++;
            if (cjk / (double)std::max<size_t>(1, sample_n) >= ratio) return true;
        }
    }
    return false;
}

U32Text snippet_around(const U32Text& text, size_t start, size_t end, size_t radius) {
    size_t a = start > radius ? start - radius : 0;
    size_t b = std::min(text.size(), end + radius);
    U32Text out;
    if (a > 0) out.push_back(U'…');
    out.append(text.substr(a, b - a));
    if (b < text.size()) out.push_back(U'…');
    return out;
}

// ---------------------------------------------------------------- 自然排序
namespace {
int cmp_u32_natural(const U32Text& a, const U32Text& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        char32_t ca = a[i], cb = b[j];
        if (ca >= U'0' && ca <= U'9' && cb >= U'0' && cb <= U'9') {
            // 跳过前导零比较数值，数值相等比长度（更短的更小，对齐 natsort 常见语义）
            size_t i0 = i, j0 = j;
            while (i < a.size() && a[i] == U'0') i++;
            while (j < b.size() && b[j] == U'0') j++;
            size_t id = i, jd = j;
            while (i < a.size() && a[i] >= U'0' && a[i] <= U'9') i++;
            while (j < b.size() && b[j] >= U'0' && b[j] <= U'9') j++;
            size_t la = i - id, lb = j - jd;
            if (la != lb) return la < lb ? -1 : 1;
            for (size_t k = 0; k < la; k++)
                if (a[id + k] != b[jd + k]) return a[id + k] < b[jd + k] ? -1 : 1;
            if ((i - i0) != (j - j0)) return (i - i0) < (j - j0) ? -1 : 1;
            continue;
        }
        if (ca != cb) return ca < cb ? -1 : 1;
        i++; j++;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}
}  // namespace

bool natural_less_u32(const U32Text& a, const U32Text& b) {
    return cmp_u32_natural(a, b) < 0;
}

bool natural_less(const std::string& a, const std::string& b) {
    return natural_less_u32(utf8_to_u32(a), utf8_to_u32(b));
}

}  // namespace lnv
