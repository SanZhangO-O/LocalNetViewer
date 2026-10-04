#pragma once
// 虚拟路径与文件系统路径安全换算（对应 Python app/util/paths.py）
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <filesystem>

namespace lnv {

struct PathError : std::runtime_error { using std::runtime_error::runtime_error; };
struct TraversalError : std::runtime_error { using std::runtime_error::runtime_error; };

// URL 解码（%XX，UTF-8 字节序列原样还原）
std::string url_unquote(const std::string& s);

// normalize_vpath：反斜杠统一、折叠 . 段、拒绝 .. 与 ':' 与 Windows 保留名。
// allow_empty=true 时空串返回 "/"。
std::string normalize_vpath(const std::string& raw, bool allow_empty = false);

std::vector<std::string> vpath_parts(const std::string& vpath);
std::optional<std::string> parent_vpath(const std::string& vpath);
std::string join_vpath(const std::string& base, const std::vector<std::string>& names);
std::string file_name_of(const std::string& vpath);
std::string ext_of(const std::string& path_or_name);   // 含点、小写，如 ".txt"
std::string stem_of(const std::string& name);

// 把书库根 + 相对段解析为真实路径并校验仍在根内（防 symlink 逃逸）。
std::filesystem::path resolve_in_root(const std::filesystem::path& root,
                                      const std::vector<std::string>& rel_parts);

bool is_hidden_name(const std::string& name);

// 书内资源路径规范化（/asset?path= 用）
std::string safe_asset_path(const std::string& raw);  // 失败抛 PathError
std::string normalize_asset_key(const std::string& path);

}  // namespace lnv
