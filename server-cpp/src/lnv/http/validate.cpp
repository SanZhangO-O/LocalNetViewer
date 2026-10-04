// pydantic v2 校验等价物实现（文案与 loc/type 见 .e2e/review/probe_pydantic.py 实测）。
#include "lnv/http/validate.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace lnv {
namespace {

const char* kMsgInvalid = "请求参数缺失或非法";

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// 完整消费的 strtod（Python float() 的近似：允许 nan/inf，拒绝尾部垃圾）
bool full_strtod(const std::string& s, double& out) {
    if (s.empty()) return false;
    errno = 0;
    const char* begin = s.c_str();
    char* end = nullptr;
    double v = std::strtod(begin, &end);
    if (end == begin || *end != '\0') return false;
    out = v;
    return true;
}

// 字符串 → 整数。pydantic v2 的 lax 规则（实测，见 .e2e/review/probe-report.txt）：
//   合法：'10'、' 10'、'+10'、'-10'、'10.0'、'1_0'（下划线分隔）
//   非法：'10.5'、'1e3'、'0x10'、'1.'、''、'abc'
// 此前走 strtod 兜底，把 '1e3'/'0x10'/'1.' 也当成合法整数，并且 strtoll 溢出时
// 静默饱和成 2^63-1（审查 M4）。
bool str_to_int(const std::string& raw, long long& out) {
    std::string s = trim(raw);
    if (s.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (s[i] == '+' || s[i] == '-') {
        neg = s[i] == '-';
        i++;
    }
    if (i >= s.size()) return false;
    // 整数部分：数字与下划线（下划线不能是首字符，Python int('_1') 非法）
    if (!is_digit(s[i])) return false;
    std::string digits;
    for (; i < s.size(); i++) {
        if (is_digit(s[i])) { digits.push_back(s[i]); continue; }
        if (s[i] == '_' && i + 1 < s.size() && is_digit(s[i + 1])) continue;
        break;
    }
    if (digits.empty()) return false;
    // 可选小数部分：'.' 后至少一位数字，且**小数部分必须为 0**（10.0 合法、10.5 非法）
    if (i < s.size() && s[i] == '.') {
        i++;
        std::string frac;
        for (; i < s.size(); i++) {
            if (is_digit(s[i])) { frac.push_back(s[i]); continue; }
            if (s[i] == '_' && i + 1 < s.size() && is_digit(s[i + 1])) continue;
            break;
        }
        if (frac.empty()) return false;
        for (char c : frac)
            if (c != '0') return false;
    }
    if (i != s.size()) return false;
    // 任意精度→long long：溢出视为非法（pydantic 会用 Python 大整数接受，但我们的
    // 校验器只用于分页/偏移等 int 语义；饱和会静默改变语义，不如显式拒绝）
    unsigned long long acc = 0;
    for (char c : digits) {
        unsigned d = unsigned(c - '0');
        if (acc > (0x7FFFFFFFFFFFFFFFull - d) / 10) return false;
        acc = acc * 10 + d;
    }
    out = neg ? -(long long)acc : (long long)acc;
    return true;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// pydantic lax 布尔字符串集合
bool str_to_bool(const std::string& raw, bool& out) {
    std::string s = lower(trim(raw));
    if (s == "1" || s == "true" || s == "yes" || s == "on" || s == "t" || s == "y") {
        out = true;
        return true;
    }
    if (s == "0" || s == "false" || s == "no" || s == "off" || s == "f" || s == "n") {
        out = false;
        return true;
    }
    return false;
}

void check_int_bound(const Validator& v, long long value, const IntBound& b,
                     const std::vector<std::string>& loc) {
    if (b.lo_strict ? !(value > b.lo) : (value < b.lo)) {
        v.add(loc,
              std::string(b.lo_strict ? "Input should be greater than "
                                      : "Input should be greater than or equal to ") +
                  std::to_string(b.lo),
              b.lo_strict ? "greater_than" : "greater_than_equal");
        return;
    }
    if (b.hi_strict ? !(value < b.hi) : (value > b.hi))
        v.add(loc,
              std::string(b.hi_strict ? "Input should be less than "
                                      : "Input should be less than or equal to ") +
                  std::to_string(b.hi),
              b.hi_strict ? "less_than" : "less_than_equal");
}

void check_num_bound(const Validator& v, double value, const NumBound& b,
                     const std::vector<std::string>& loc) {
    // NaN 一律判定为越界（pydantic：nan 不满足 le/ge）
    if (b.lo_strict ? !(value > b.lo) : !(value >= b.lo)) {
        v.add(loc,
              std::string(b.lo_strict ? "Input should be greater than "
                                      : "Input should be greater than or equal to ") +
                  pv_fmt_num(b.lo),
              b.lo_strict ? "greater_than" : "greater_than_equal");
        return;
    }
    if (b.hi_strict ? !(value < b.hi) : !(value <= b.hi))
        v.add(loc,
              std::string(b.hi_strict ? "Input should be less than "
                                      : "Input should be less than or equal to ") +
                  pv_fmt_num(b.hi),
              b.hi_strict ? "less_than" : "less_than_equal");
}

// ------------------------------------------------------- Python json 位置扫描
size_t skip_ws(const std::string& s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    return i;
}

bool scan_string(const std::string& s, size_t& i, size_t& err) {
    // s[i] == '"'
    size_t p = i + 1;
    while (p < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[p]);
        if (c == '"') {
            i = p + 1;
            return true;
        }
        if (c == '\\') {
            p += 2;
            continue;
        }
        if (c < 0x20) {
            err = p;
            return false;
        }
        p++;
    }
    err = s.size();
    return false;
}

bool scan_number(const std::string& s, size_t& i, size_t& err) {
    size_t p = i;
    if (p < s.size() && s[p] == '-') p++;
    if (p >= s.size() || !is_digit(s[p])) {
        err = p;
        return false;
    }
    if (s[p] == '0') {
        p++;
    } else {
        while (p < s.size() && is_digit(s[p])) p++;
    }
    if (p < s.size() && s[p] == '.') {
        p++;
        if (p >= s.size() || !is_digit(s[p])) {
            err = p;
            return false;
        }
        while (p < s.size() && is_digit(s[p])) p++;
    }
    if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
        p++;
        if (p < s.size() && (s[p] == '+' || s[p] == '-')) p++;
        if (p >= s.size() || !is_digit(s[p])) {
            err = p;
            return false;
        }
        while (p < s.size() && is_digit(s[p])) p++;
    }
    i = p;
    return true;
}

bool scan_value(const std::string& s, size_t& i, size_t& err);

bool scan_container(const std::string& s, size_t& i, size_t& err, char open, char close) {
    size_t p = i + 1;
    p = skip_ws(s, p);
    if (open == '{') {
        if (p < s.size() && s[p] == close) {
            i = p + 1;
            return true;
        }
        for (;;) {
            p = skip_ws(s, p);
            if (p >= s.size() || s[p] != '"') {
                err = p;
                return false;
            }
            if (!scan_string(s, p, err)) return false;
            p = skip_ws(s, p);
            if (p >= s.size() || s[p] != ':') {
                err = p;
                return false;
            }
            p++;
            if (!scan_value(s, p, err)) return false;
            p = skip_ws(s, p);
            if (p < s.size() && s[p] == ',') {
                p++;
                continue;
            }
            if (p < s.size() && s[p] == close) {
                i = p + 1;
                return true;
            }
            err = p;
            return false;
        }
    }
    if (p < s.size() && s[p] == close) {
        i = p + 1;
        return true;
    }
    for (;;) {
        if (!scan_value(s, p, err)) return false;
        p = skip_ws(s, p);
        if (p < s.size() && s[p] == ',') {
            p++;
            continue;
        }
        if (p < s.size() && s[p] == close) {
            i = p + 1;
            return true;
        }
        err = p;
        return false;
    }
}

bool scan_value(const std::string& s, size_t& i, size_t& err) {
    size_t p = skip_ws(s, i);
    if (p >= s.size()) {
        err = p;
        return false;
    }
    char c = s[p];
    auto literal = [&](const char* lit) {
        size_t n = std::strlen(lit);
        if (s.compare(p, n, lit) == 0) {
            i = p + n;
            return true;
        }
        err = p;  // Python 对字面量失败报"值起点"
        return false;
    };
    switch (c) {
        case 'n': return literal("null");
        case 't': return literal("true");
        case 'f': return literal("false");
        case '"': return scan_string(s, i, err);
        case '[': return scan_container(s, i, err, '[', ']');
        case '{': return scan_container(s, i, err, '{', '}');
        default:
            if (c == '-' || is_digit(c)) return scan_number(s, i, err);
            err = p;
            return false;
    }
}

}  // namespace

std::string pv_fmt_num(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::abs(v) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        return buf;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

size_t pv_json_error_pos(const std::string& s) {
    size_t i = 0, err = 0;
    if (scan_value(s, i, err)) {
        size_t j = skip_ws(s, i);
        if (j < s.size()) return j;  // Extra data：Python 报多余内容起点
        return 0;
    }
    return err;
}

// ---------------------------------------------------------------- Validator
void Validator::add(std::vector<std::string> loc, std::string msg, std::string type) const {
    if (errs_.size() >= 20) return;  // pydantic/FastAPI 侧同样截断
    errs_.push_back({std::move(loc), std::move(msg), std::move(type)});
}

void Validator::done() const {
    if (errs_.empty()) return;
    nlohmann::json errors = nlohmann::json::array();
    for (const auto& e : errs_) {
        nlohmann::json loc = nlohmann::json::array();
        for (const auto& p : e.loc) loc.push_back(p);
        errors.push_back({{"loc", loc}, {"msg", e.msg}, {"type", e.type}});
    }
    throw AppError(ErrorCode::BadRequest, kMsgInvalid, nlohmann::json{{"errors", errors}});
}

// ---------------------------------------------------------------- query/path
long long pv_int(const Validator& v, const std::optional<std::string>& raw,
                 std::vector<std::string> loc, long long def, IntBound b) {
    if (!raw) return def;
    long long value = 0;
    if (!str_to_int(*raw, value)) {
        v.add(std::move(loc), "Input should be a valid integer, unable to parse string as an integer",
              "int_parsing");
        return def;
    }
    check_int_bound(v, value, b, loc);
    return value;
}

double pv_num(const Validator& v, const std::optional<std::string>& raw,
              std::vector<std::string> loc, double def, NumBound b) {
    if (!raw) return def;
    double value = 0;
    if (!full_strtod(trim(*raw), value)) {
        v.add(std::move(loc), "Input should be a valid number, unable to parse string as a number",
              "float_parsing");
        return def;
    }
    check_num_bound(v, value, b, loc);
    return value;
}

std::string pv_str(const Validator& v, const std::optional<std::string>& raw,
                   std::vector<std::string> loc, const std::string& def, bool required,
                   size_t min_len) {
    if (!raw) {
        if (required) v.add(std::move(loc), "Field required", "missing");
        return def;
    }
    if (min_len > 0 && raw->size() < min_len) {
        v.add(std::move(loc), "String should have at least " + std::to_string(min_len) + " character",
              "string_too_short");
        return def;
    }
    return *raw;
}

// ---------------------------------------------------------------- JSON body
const nlohmann::json* jv_object(const Validator& v, const nlohmann::json& obj, const char* key,
                               std::vector<std::string> loc) {
    if (!obj.is_object()) return nullptr;
    auto it = obj.find(key);
    if (it == obj.end()) return nullptr;  // 缺失 → 用默认值
    if (!it->is_object()) {
        // 显式 null 或类型不对：pydantic 对"有默认值的子模型"同样报错
        v.add(std::move(loc), "Input should be a valid dictionary or object to extract fields from",
              "model_attributes_type");
        return nullptr;
    }
    return &*it;
}

const nlohmann::json* jv_object_required(const Validator& v, const nlohmann::json& obj,
                                         const char* key, std::vector<std::string> loc) {
    if (!obj.is_object()) return nullptr;
    auto it = obj.find(key);
    if (it == obj.end()) {
        v.add(std::move(loc), "Field required", "missing");
        return nullptr;
    }
    if (!it->is_object()) {
        v.add(std::move(loc), "Input should be a valid dictionary or object to extract fields from",
              "model_attributes_type");
        return nullptr;
    }
    return &*it;
}

long long jv_int(const Validator& v, const nlohmann::json& obj, const char* key,
                 std::vector<std::string> loc, long long def, IntBound b, bool required,
                 bool nullable) {
    if (!obj.is_object()) return def;
    auto it = obj.find(key);
    if (it == obj.end()) {
        if (required) v.add(std::move(loc), "Field required", "missing");
        return def;
    }
    if (it->is_null()) {
        if (nullable) return def;
        v.add(std::move(loc), "Input should be a valid integer", "int_type");
        return def;
    }
    long long value = def;
    if (it->is_boolean()) {
        value = it->get<bool>() ? 1 : 0;
    } else if (it->is_number_integer() || it->is_number_unsigned()) {
        value = it->get<long long>();
    } else if (it->is_number_float()) {
        double d = it->get<double>();
        if (d != std::floor(d) || !std::isfinite(d)) {
            v.add(std::move(loc), "Input should be a valid integer, got a number with a fractional part",
                  "int_from_float");
            return def;
        }
        value = static_cast<long long>(d);
    } else if (it->is_string()) {
        if (!str_to_int(it->get<std::string>(), value)) {
            v.add(std::move(loc),
                  "Input should be a valid integer, unable to parse string as an integer",
                  "int_parsing");
            return def;
        }
    } else {
        v.add(std::move(loc), "Input should be a valid integer", "int_type");
        return def;
    }
    check_int_bound(v, value, b, loc);
    return value;
}

double jv_num(const Validator& v, const nlohmann::json& obj, const char* key,
              std::vector<std::string> loc, double def, NumBound b, bool required, bool nullable) {
    if (!obj.is_object()) return def;
    auto it = obj.find(key);
    if (it == obj.end()) {
        if (required) v.add(std::move(loc), "Field required", "missing");
        return def;
    }
    if (it->is_null()) {
        if (nullable) return def;
        v.add(std::move(loc), "Input should be a valid number", "float_type");
        return def;
    }
    double value = def;
    if (it->is_boolean()) {
        value = it->get<bool>() ? 1.0 : 0.0;
    } else if (it->is_number()) {
        value = it->get<double>();
    } else if (it->is_string()) {
        if (!full_strtod(trim(it->get<std::string>()), value)) {
            v.add(std::move(loc), "Input should be a valid number, unable to parse string as a number",
                  "float_parsing");
            return def;
        }
    } else {
        v.add(std::move(loc), "Input should be a valid number", "float_type");
        return def;
    }
    check_num_bound(v, value, b, loc);
    return value;
}

bool jv_bool(const Validator& v, const nlohmann::json& obj, const char* key,
             std::vector<std::string> loc, bool def) {
    if (!obj.is_object()) return def;
    auto it = obj.find(key);
    if (it == obj.end()) return def;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number_integer() || it->is_number_unsigned()) {
        long long n = it->get<long long>();
        if (n == 0) return false;
        if (n == 1) return true;
    } else if (it->is_number_float()) {
        double d = it->get<double>();
        if (d == 0.0) return false;
        if (d == 1.0) return true;
    } else if (it->is_string()) {
        bool out = def;
        if (str_to_bool(it->get<std::string>(), out)) return out;
    } else if (it->is_null()) {
        // 非 Optional[bool]：null → bool_type（与 bool_parsing 文案不同）
        v.add(std::move(loc), "Input should be a valid boolean", "bool_type");
        return def;
    }
    v.add(std::move(loc), "Input should be a valid boolean, unable to interpret input",
          "bool_parsing");
    return def;
}

std::string jv_str(const Validator& v, const nlohmann::json& obj, const char* key,
                   std::vector<std::string> loc, const std::string& def, bool required) {
    if (!obj.is_object()) return def;
    auto it = obj.find(key);
    if (it == obj.end()) {
        if (required) v.add(std::move(loc), "Field required", "missing");
        return def;
    }
    // 显式 null 与其它类型一样报 string_type（Python 侧 family/path 都不是 Optional）
    if (!it->is_string()) {
        v.add(std::move(loc), "Input should be a valid string", "string_type");
        return def;
    }
    return it->get<std::string>();
}

std::string jv_mode(const Validator& v, const nlohmann::json& obj, const char* key,
                    std::vector<std::string> loc, const std::string& def) {
    if (!obj.is_object()) return def;
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return def;
    if (!it->is_string()) {
        v.add(std::move(loc), "Input should be 'flow', 'fixed' or 'strip'", "literal_error");
        return def;
    }
    std::string s = it->get<std::string>();
    if (s != "flow" && s != "fixed" && s != "strip") {
        v.add(std::move(loc), "Input should be 'flow', 'fixed' or 'strip'", "literal_error");
        return def;
    }
    return s;
}

// ---------------------------------------------------------------- body 工具
nlohmann::json pv_parse_body(const std::string& raw) {
    try {
        return nlohmann::json::parse(raw);
    } catch (const std::exception&) {
        size_t pos = pv_json_error_pos(raw);
        Validator v;
        v.add({"body", std::to_string(pos)}, "JSON decode error", "json_invalid");
        v.done();
        throw;  // 不可达
    }
}

void pv_require_object(const Validator& v, const nlohmann::json& j, std::vector<std::string> loc) {
    if (!j.is_object())
        v.add(std::move(loc), "Input should be a valid dictionary or object to extract fields from",
              "model_attributes_type");
}

}  // namespace lnv
