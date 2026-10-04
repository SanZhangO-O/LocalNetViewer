#pragma once
// 格式注册表（对应 Python formats/registry.py create_adapter）
#include <optional>
#include "lnv/config.h"
#include "lnv/formats/adapter.h"

namespace lnv {

// 按扩展名/虚拟书类型创建适配器；不支持 → UnsupportedFormatError
std::unique_ptr<BookAdapter> create_adapter(const AppConfig& cfg, const BookRef& ref);

// 该格式当前是否可打开；不可打开时返回原因（对应 registry.openable_reason）。
// /files 的 openable / reason 字段据此置灰，缺失后端时不该谎报可打开。
std::optional<std::string> openable_reason(const std::string& fmt);

}  // namespace lnv
