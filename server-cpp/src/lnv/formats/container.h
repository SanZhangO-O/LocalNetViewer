#pragma once
// 容器抽象（ZIP/TAR/RAR/7z/目录）+ 漫画/EPUB 共用的条目枚举
#include "lnv/config.h"
#include "lnv/formats/adapter.h"

namespace lnv {

struct ContainerEntry {
    std::string name;       // 容器内路径（规范化后）
    unsigned long long size = 0;      // 声明尺寸
    unsigned long long compressed_size = 0;
    double mtime = 0;
};

class Container {
public:
    virtual ~Container() = default;
    virtual const std::vector<ContainerEntry>& entries() const = 0;
    // 读取条目（限制 entry_max_bytes；超出抛 ParseError）。失败抛 ParseError/AssetNotFoundError
    virtual std::string read(const std::string& name, long long entry_max_bytes) = 0;
};

// ZIP（miniz）。entry_names 已做 cp437 还原 + 规范化。
std::unique_ptr<Container> open_zip(const std::filesystem::path& path);
// 目录容器（entries = 目录内图片相对路径）。recursive=true 时按 os.walk 语义下钻
// （gallery.recursive / POST open-folder 的 recursive 参数）
std::unique_ptr<Container> open_dir(const std::filesystem::path& path, bool recursive = false);
// TAR（自写 ustar 读取）
std::unique_ptr<Container> open_tar(const std::filesystem::path& path);
// RAR：unrar/bsdtar 子进程；工具缺失 → UnsupportedFormatError("需要 RAR 后端","需安装 unrar")
std::unique_ptr<Container> open_rar(const std::filesystem::path& path);
// 7z：7z 子进程；缺失 → UnsupportedFormatError("需要 7z 后端","需安装 7-Zip")
std::unique_ptr<Container> open_7z(const std::filesystem::path& path);

// 可选后端可用性探测（进程内缓存；对应 Python registry.backend_status/openable_reason）
bool rar_backend_available();    // unrar 或 bsdtar 在 PATH 上
bool seven_backend_available();  // 7z.exe 在 PATH 上

// ---- 容器条目筛选（§4.3，EPUB/漫画共用语义）----
// should_skip_entry：隐藏段、skip_entries glob（三重匹配）
bool should_skip_entry(const std::string& norm_name, const std::vector<std::string>& skip_globs);
// is_unsafe_entry_name：空/./../`../`开头/`/`开头/首段含 ':'
bool is_unsafe_entry_name(const std::string& norm_name);
// ZIP 条目名解码：flag 0x800 → 原名；否则 cp437 字节 → utf-8/gbk/big5/shift_jis 择优
std::string decode_zip_name(const std::string& raw_name, unsigned flag_bits);

// 图片扩展判断（IMAGE_EXT 表）
bool is_image_ext(const std::string& ext);
// natural_key 比较（文本段 casefold / 数字段 int）——用于容器页序
bool natural_key_less(const std::string& a, const std::string& b);

}  // namespace lnv
