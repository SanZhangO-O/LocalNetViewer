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

// Windows 代码页。GB18030 用专用 CP 54936（失败回退 GBK 936）。
UINT codepage_of(const std::string& enc) {
    std::string e;
    e.reserve(enc.size());
    for (char c : enc) e.push_back(char(::tolower((unsigned char)c)));
    if (e == "ascii") return 20127;        // US-ASCII 严格
    if (e == "cp1252" || e == "windows-1252" || e == "latin-1" || e == "latin1")
        return 1252;
    if (e == "gb18030") return 54936;
    if (e == "gbk" || e == "gb2312" || e == "cp936" || e == "gb2312-80") return 936;
    if (e == "big5" || e == "big5hkscs" || e == "cp950") return 950;
    if (e == "shift_jis" || e == "sjis" || e == "cp932" || e == "shift_jis_2004" ||
        e == "shift_jisx0213" || e == "mskanji") return 932;
    // 韩文（charset-normalizer 对 cp949 文本返回 "cp949"）
    if (e == "cp949" || e == "euc_kr" || e == "euc-kr" || e == "uhc" || e == "ms949")
        return 949;
    return 0;
}

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

}  // namespace

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
        std::string data = bytes;
        bool be = e == "utf-32-be";
        if (e == "utf-16") {
            if (data.size() >= 2) {
                if ((unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xFE) { be = false; data = data.substr(2); }
                else if ((unsigned char)data[0] == 0xFE && (unsigned char)data[1] == 0xFF) { be = true; data = data.substr(2); }
                else return decode_strict(data, "utf-16-le");
            }
            if (data.size() % 2) return std::nullopt;
            std::vector<uint16_t> units;
            units.reserve(data.size() / 2);
            for (size_t i = 0; i + 1 < data.size(); i += 2) {
                unsigned char b0 = (unsigned char)data[i], b1 = (unsigned char)data[i + 1];
                units.push_back(uint16_t(be ? ((b0 << 8) | b1) : (b0 | (b1 << 8))));
            }
            return decode_utf16_units(units);
        }
        if (data.size() % 4) return std::nullopt;
        U32Text out;
        for (size_t i = 0; i + 3 < data.size(); i += 4) {
            char32_t u = be ? ((char32_t((unsigned char)data[i]) << 24) | (char32_t((unsigned char)data[i+1]) << 16) |
                               (char32_t((unsigned char)data[i+2]) << 8) | (unsigned char)data[i+3])
                            : (char32_t((unsigned char)data[i]) | (char32_t((unsigned char)data[i+1]) << 8) |
                               (char32_t((unsigned char)data[i+2]) << 16) | (char32_t((unsigned char)data[i+3]) << 24));
            out.push_back(u);
        }
        return out;
    }
    if (e == "utf-8-sig") {
        std::string d = bytes;
        if (d.size() >= 3 && (unsigned char)d[0] == 0xEF && (unsigned char)d[1] == 0xBB &&
            (unsigned char)d[2] == 0xBF) d = d.substr(3);
        return decode_strict(d, "utf-8");
    }
    if (e == "utf-8") {
        // 严格 UTF-8 校验：逐字节手写，避免 MultiByteToWideChar 的宽松差异
        U32Text out;
        out.reserve(bytes.size());
        size_t i = 0, n = bytes.size();
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

// 乱度启发式：解码后"可疑字符"占比（控制字符、替换符、罕见区段）。
// 对齐 charset-normalizer 在本项目里的窄用途：中文文本不能被判成韩文/西文。
double chaos_of(const U32Text& t) {
    if (t.empty()) return 1.0;
    size_t bad = 0;
    for (char32_t c : t) {
        if (c == 0xFFFD) { bad++; continue; }
        if (c < 32 && c != U'\t' && c != U'\r' && c != U'\n' && c != U'\f' && c != U'\v') { bad++; continue; }
        // Hangul 音节出现在中文书里是判错编码的强信号
        if (c >= 0xAC00 && c <= 0xD7AF) bad++;
        // 半角片假名（SJIS 误读 GBK 的典型产物）
        if (c >= 0xFF61 && c <= 0xFF9F) bad += 0;
    }
    return bad / (double)t.size();
}

struct Candidate { std::string enc; double chaos; };

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
    // 中文与韩文乱度打平时选中文（对齐 Python 侧的收窄修正）。
    std::string sample = data.substr(0, 65536);
    // 纯 ASCII：charset-normalizer 会报 "ascii"，这里同样先判（否则 cp1252 也能解码，
    // 会把纯 ASCII 书标成 cp1252，与 Python 的 meta.encoding 不一致）。
    if (is_pure_ascii(sample)) return "ascii";
    std::vector<Candidate> cands;
    // cp949/euc-kr 必须在候选里：没有韩文编码时，cp949 字节流是合法 GB18030，
    // 整本韩文小说会被当中文解出乱码（Python 的 charset-normalizer 返回 cp949）。
    for (const char* enc : {"utf-8", "gb18030", "big5", "shift_jis", "cp949", "cp1252"}) {
        if (std::string(enc) == "utf-8") {
            // 采样在 64 KiB 处可能切断多字节字符：按"尾部允许不完整序列"校验并只解码
            // 完整部分，否则 utf-8 会因严格校验失败而落选（审查 B2）。
            size_t ok = utf8_valid_prefix_len(sample);
            if (ok == SIZE_MAX) continue;
            auto t = decode_strict(sample.substr(0, ok), enc);
            if (t) cands.push_back({enc, chaos_of(*t)});
            continue;
        }
        auto t = decode_strict(sample, enc);
        if (t) cands.push_back({enc, chaos_of(*t)});
    }
    if (!cands.empty()) {
        // 乱度相同时的优先级（越小越优先）：与 Python 侧的收窄修正同向 ——
        // 打平的中文/韩文候选选中文，utf-8 有效时优先 utf-8。
        auto rank = [](const std::string& e) {
            if (e == "utf-8") return 0;
            if (e == "gb18030") return 1;
            if (e == "big5") return 2;
            if (e == "cp949") return 3;
            return 4;
        };
        auto best = std::min_element(cands.begin(), cands.end(),
            [&](const Candidate& a, const Candidate& b) {
                if (std::fabs(a.chaos - b.chaos) > 1e-9) return a.chaos < b.chaos;
                return rank(a.enc) < rank(b.enc);
            });
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
        // 消费可能残留的 BOM
        while (!t->empty() && t->front() == 0xFEFF) t->erase(t->begin());
        return {*t, enc};
    }
    // 最后兜底：UTF-8 replace
    U32Text t = utf8_to_u32(bytes);
    while (!t.empty() && t.front() == 0xFEFF) t.erase(t.begin());
    return {t, "utf-8/replace"};
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
// Python str.strip()/正则 \s 的 Unicode 空白集合。只认 ' ' '\t' 与全角空格时，
// "\xa0第1章 测试"（网页粘贴常见的 NBSP 前导）在 Python 是章节标题、在 C++ 是普通
// 段落 → 章节列表、每章 total_chars 与全部页锚点都不同。
bool is_space(char32_t c) {
    switch (c) {
        case U'\t': case U'\n': case U'\v': case U'\f': case U'\r':
        case U' ': case U'\x85': case U'\xA0': case U'\x1680':
        case U'\x2028': case U'\x2029': case U'\x202F': case U'\x205F': case U'\x3000':
            return true;
        default:
            return c >= 0x2000 && c <= 0x200A;
    }
}
}

std::optional<U32Text> match_chapter_title(const U32Text& line_in) {
    // strip
    U32Text line = line_in;
    size_t b = 0, e = line.size();
    while (b < e && is_space(line[b])) b++;
    while (e > b && is_space(line[e - 1])) e--;
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
                while (j < line.size() && is_space(line[j])) j++;
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
            while (i < line.size() && is_space(line[i])) i++;
            if (i > sp) {
                size_t d0 = i;
                while (i < line.size() && line[i] >= U'0' && line[i] <= U'9') i++;
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
