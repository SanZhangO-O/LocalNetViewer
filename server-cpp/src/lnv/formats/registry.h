#pragma once
// 格式注册表（对应 Python formats/registry.py create_adapter）
#include <optional>
#include "lnv/config.h"
#include "lnv/formats/adapter.h"

namespace lnv {

// 按扩展名/虚拟书类型创建适配器；不支持 → UnsupportedFormatError。
// env 提供适配器构造所需的共享资源（字体管理器；PDF/漫画适配器忽略）。
std::unique_ptr<BookAdapter> create_adapter(const AppConfig& cfg, const BookRef& ref,
                                            const AdapterEnv& env = {});

// 该格式当前是否可打开；不可打开时返回原因（对应 registry.openable_reason）。
// /files 的 openable / reason 字段据此置灰，缺失后端时不该谎报可打开。
std::optional<std::string> openable_reason(const std::string& fmt);

}  // namespace lnv
