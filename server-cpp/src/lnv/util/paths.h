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
// canonical 解析失败时抛 PathError（fail-closed），绝不退回纯词法比较。
std::filesystem::path resolve_in_root(const std::filesystem::path& root,
                                      const std::vector<std::string>& rel_parts);

// 是否需要按“链接”跳过：IO_REPARSE_TAG_SYMLINK（等价 Python Windows 的
// os.path.islink）或 IO_REPARSE_TAG_MOUNT_POINT（目录 junction）。
// CPython 只认前者，os.walk 会跟进 junction 成环（bpo-23407 仍未修）；这里把
// junction 一并跳过，消除书库内 junction 环导致的无限递归/栈溢出。
// 其他 reparse point（OneDrive 云占位符等）不跳过，与 Python 的扫描行为一致。
bool is_symlink_or_junction(const std::filesystem::path& p);

bool is_hidden_name(const std::string& name);

// ---- 外部工具 / 命令行（Win32）----
// 只在 PATH 的显式目录里查找 base_name + ".exe"（返回宽路径，找不到为空）。
// 不能用 SearchPathW(nullptr,...)：其搜索序含 exe 目录/进程 CWD，同名 exe 可被
// 种植劫持；语义对齐 Python shutil.which（只认 PATH）。container.cpp/mobi.cpp 共用。
std::wstring find_tool_in_path(const std::wstring& base_name);

// MSVCRT 命令行参数的引号转义（CreateProcess 解析规则）：只有紧邻引号的反斜杠
// 需要加倍，参数结尾的反斜杠也必须加倍，否则会转义闭合引号。
std::wstring quote_windows_arg(const std::wstring& s);

// std::filesystem::path → UTF-8（不抛异常）：path.string() 走 ANSI 代码页，不可
// 表示的字符可能抛异常，而这里要用于异常消息/外部工具参数；UTF-8 也与 Python 的
// str 一致。优先 WideCharToMultiByte(CP_UTF8)，失败退回 u8string。
std::string path_to_utf8(const std::filesystem::path& p);

// 书内资源路径规范化（/asset?path= 用）
std::string safe_asset_path(const std::string& raw);  // 失败抛 PathError
std::string normalize_asset_key(const std::string& path);

}  // namespace lnv
