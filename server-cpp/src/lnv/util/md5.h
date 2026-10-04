#pragma once
// MD5（Windows BCrypt）。仅用于对齐 Starlette FileResponse 的 ETag 公式：
//   md5(f"{st_mtime}-{st_size}").hexdigest()
// 不用于任何安全用途。
#include <string>

namespace lnv {

// 十六进制小写；失败返回空串
std::string md5_hex(const std::string& data);

}  // namespace lnv
