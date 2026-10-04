#pragma once
// pydantic v2 校验等价物（lax 模式）。
//
// 契约（docs/PORT_SPEC.md §1.2）：校验失败 → 400 BAD_REQUEST，
// message "请求参数缺失或非法"，detail {"errors":[{"loc","msg","type"}, …]}（≤20 条）。
//
// loc / msg / type 必须与 pydantic 逐字一致：Android 客户端按 code 分支，
// 而 loc/type 是排障与测试断言的依据。本文件的文案全部来自
// `.e2e/review/probe_pydantic.py` 对 Python 参照版的实测输出，改动前请先重跑该脚本。
#include <limits>
#include <string>
#include <vector>

#include "lnv/types.h"

namespace lnv {

// 单条校验错误（= pydantic ValidationError.errors() 的一项）
struct FieldError {
    std::vector<std::string> loc;  // {"query","limit"} / {"body","font","size_px"}
    std::string msg;
    std::string type;
};

// 整数/浮点边界（默认无约束）
struct IntBound {
    long long lo = std::numeric_limits<long long>::min();
    long long hi = std::numeric_limits<long long>::max();
    bool lo_strict = false;
    bool hi_strict = false;
};
struct NumBound {
    double lo = -std::numeric_limits<double>::infinity();
    double hi = std::numeric_limits<double>::infinity();
    bool lo_strict = false;
    bool hi_strict = false;
};

class Validator {
public:
    // const：pv_*/jv_* 辅助函数只拿到 const 引用，仍需要往里记错误
    void add(std::vector<std::string> loc, std::string msg, std::string type) const;
    bool ok() const { return errs_.empty(); }
    // 有错误 → 抛 AppError(BadRequest, "请求参数缺失或非法", {"errors":[…]})；无错误直接返回
    void done() const;

private:
    mutable std::vector<FieldError> errs_;
};

// ---------------------------------------------------------------- 便捷构造
inline IntBound int_ge(long long lo) { return IntBound{lo, std::numeric_limits<long long>::max(), false, false}; }
inline IntBound int_le(long long hi) { return IntBound{std::numeric_limits<long long>::min(), hi, false, false}; }
inline IntBound int_range(long long lo, long long hi) { return IntBound{lo, hi, false, false}; }
inline NumBound num_range(double lo, double hi) { return NumBound{lo, hi, false, false}; }
inline NumBound num_gt(double lo) { return NumBound{lo, std::numeric_limits<double>::infinity(), true, false}; }

// ------------------------------------------------- query / path（字符串来源）
// raw 为 nullopt 表示参数缺失。返回默认值表示"缺失或非法"（非法时已记录错误）。
long long pv_int(const Validator& v, const std::optional<std::string>& raw,
                 std::vector<std::string> loc, long long def, IntBound b);
double pv_num(const Validator& v, const std::optional<std::string>& raw,
              std::vector<std::string> loc, double def, NumBound b);
// min_len=0 表示无长度约束；缺失且 required 时记 missing
std::string pv_str(const Validator& v, const std::optional<std::string>& raw,
                   std::vector<std::string> loc, const std::string& def, bool required,
                   size_t min_len);

// ---------------------------------------------------------- JSON body（对象字段）
// 字段缺失：required → missing 错误；否则取 def。
// nullable：字段为 null 时取 def 且不报错（对应 Optional[...]）。
// 超出 long long 范围的 JSON 数值（无符号整数 ≥2^63 或整数值浮点）饱和到边界值
// （绝不环绕/UB）。需要"Python 无界 int 原样回显"的字段用 jv_int_ex 拿原始数值。
long long jv_int(const Validator& v, const nlohmann::json& obj, const char* key,
                 std::vector<std::string> loc, long long def, IntBound b, bool required = false,
                 bool nullable = false);
// 与 jv_int 完全相同的校验路径，但成功时把原始 JSON 数值写入 raw_out（可空）：
// number_unsigned 保留 uint64（2^63..2^64-1 不饱和）、number_integer 原样、
// 布尔/字符串/整数值浮点归一为等值 JSON 整数。raw_out 仅在成功路径写入。
long long jv_int_ex(const Validator& v, const nlohmann::json& obj, const char* key,
                    std::vector<std::string> loc, long long def, IntBound b, bool required,
                    bool nullable, nlohmann::json* raw_out);
double jv_num(const Validator& v, const nlohmann::json& obj, const char* key,
              std::vector<std::string> loc, double def, NumBound b, bool required = false,
              bool nullable = false);
bool jv_bool(const Validator& v, const nlohmann::json& obj, const char* key,
             std::vector<std::string> loc, bool def);
std::string jv_str(const Validator& v, const nlohmann::json& obj, const char* key,
                   std::vector<std::string> loc, const std::string& def, bool required = false);
// 取子对象：缺失 → 返回 nullptr（调用方用自己的默认值）；
// 存在但不是对象（含显式 null）→ model_attributes_type，同样返回 nullptr
const nlohmann::json* jv_object(const Validator& v, const nlohmann::json& obj, const char* key,
                                std::vector<std::string> loc);
// 必填子对象：缺失 → missing；非对象 → model_attributes_type
const nlohmann::json* jv_object_required(const Validator& v, const nlohmann::json& obj,
                                         const char* key, std::vector<std::string> loc);
// Literal["flow","fixed","strip"]；非法 → literal_error
std::string jv_mode(const Validator& v, const nlohmann::json& obj, const char* key,
                    std::vector<std::string> loc, const std::string& def);

// ---------------------------------------------------------------- 其它工具
// 解析请求体；失败抛 400 + detail.errors[{loc:["body",<pos>],msg:"JSON decode error",type:"json_invalid"}]
nlohmann::json pv_parse_body(const std::string& raw);
// body 不是对象 → model_attributes_type（loc 见参数）
void pv_require_object(const Validator& v, const nlohmann::json& j, std::vector<std::string> loc);

// Python json 模块风格的"第一个错误位置"（pydantic 用它做 loc 末项）
size_t pv_json_error_pos(const std::string& s);

// 数字的 pydantic 文案形式：2.0 → "2"，0.5 → "0.5"
std::string pv_fmt_num(double v);

}  // namespace lnv
