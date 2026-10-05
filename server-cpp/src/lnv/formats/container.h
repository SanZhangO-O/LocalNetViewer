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
    // 可选：给按需解压的容器（RAR/7z）设置 max_archive_total_mb 累计上限（字节；0 = 不限）。
    // 默认空实现：ZIP/TAR/目录的声明尺寸总量在 comics.build_names 已校验。
    virtual void set_total_limit(long long /*bytes*/) {}
    // 条目是否已按自然顺序排好（目录容器由 list_dir_image_names 排好序）。
    // build_names 据此跳过一次 O(N log N) 重排 —— 上万张图时每请求都排一次很贵。
    virtual bool entries_presorted() const { return false; }
};

// 目录图片枚举（相对路径，'/' 分隔，自然排序）：跳过隐藏、skip_entries glob、
// 非图片、符号链接与越界条目。由 ``open_dir`` 的目录容器使用，结果按
// (目录, recursive, skip 规则) + 目录 mtime + TTL 缓存，避免图集的每个 /meta、
// /layout、取页请求都重扫目录；不逐文件 stat。
// ZIP（miniz）。entry_names 已做 cp437 还原 + 规范化。
std::unique_ptr<Container> open_zip(const std::filesystem::path& path);
// 目录容器（entries = 目录内图片相对路径）。recursive=true 时按 os.walk 语义下钻
// （gallery.recursive / POST open-folder 的 recursive 参数）
std::unique_ptr<Container> open_dir(const std::filesystem::path& path, bool recursive = false,
                                    const std::vector<std::string>& skip_globs = {});
// TAR（自写 ustar 读取）
std::unique_ptr<Container> open_tar(const std::filesystem::path& path);
// RAR：unrar/bsdtar 子进程，列表后按需单条目解到内存（unrar p / bsdtar -xO）；
// 工具缺失 → UnsupportedFormatError("需要 RAR 后端","需安装 unrar")
std::unique_ptr<Container> open_rar(const std::filesystem::path& path);
// 7z：7z 子进程，列表后按需单条目解到内存（7z e -so）；
// 缺失 → UnsupportedFormatError("需要 7z 后端","需安装 7-Zip")
std::unique_ptr<Container> open_7z(const std::filesystem::path& path);

// 可选后端可用性探测（进程内缓存；对应 Python registry.backend_status/openable_reason）
bool rar_backend_available();    // unrar 或 bsdtar 在 PATH 上
bool seven_backend_available();  // 7z.exe 在 PATH 上

// 解压总量超限的错误文案（container/epub/comics 共用；逐字对齐 Python）
std::string archive_total_limit_error(long long total_bytes);

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
