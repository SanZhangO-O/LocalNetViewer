#pragma once
// 配置加载与校验（对应 Python app/config.py，共用 server/config.yaml）
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <filesystem>

namespace lnv {

struct ConfigError : std::runtime_error { using std::runtime_error::runtime_error; };

struct LibraryRootCfg {
    std::string name;
    std::string path;
};

struct LibraryCfg {
    std::vector<LibraryRootCfg> roots;
    std::vector<std::string> include_ext;   // 小写、带点
    bool show_all_files = false;
    std::string sort = "natural";           // natural | name
    // 用有序表而非 std::map：Python 的 dict 保留 YAML 插入顺序，多后缀命中时
    // 取"最先插入且匹配"的一条；std::map 按字典序迭代会让命中项与 Python 不同
    // （如同时配了 ".txt" 与 "book.txt" 时）。重复键按 dict 语义"保留最后值"。
    std::vector<std::pair<std::string, std::string>> encoding_overrides;
};

struct GalleryCfg {
    bool folder_as_book = true;
    int min_images_in_folder = 1;
    bool recursive = false;
    bool show_image_files = false;
};

struct ComicCfg {
    std::vector<std::string> skip_entries = {"__MACOSX/*", "*/.*", "Thumbs.db", "desktop.ini"};
    std::string default_fit = "viewport";   // viewport | original
    int jpeg_quality = 85;
    double strip_min_aspect = 1.5;
    int strip_overlap_px = 0;
};

struct RenderCfg {
    std::string fonts_dir = "assets/fonts";
    std::string default_font = "NotoSansSC-Regular";
    long long max_page_pixels = 4000000;
    bool jpeg_for_photo_pages = true;
};

struct CacheCfg {
    std::string dir = "cache";
    long long parsed_max_mb = 4096;
    long long page_lru_bytes = 536870912LL;
    long long page_lru_pages = 512;
    int layout_memory_max = 32;
    long long layouts_max_mb = 2048;
    int doc_memory_max = 3;
};

struct LimitsCfg {
    long long max_book_mb = 2048;
    long long max_page_bytes = 52428800LL;
    long long max_source_pixels = 30000000LL;
    double parse_timeout_s = 60.0;
    long long max_archive_entry_mb = 512;
    long long max_archive_total_mb = 4096;
};

struct ServerCfg {
    std::string name = "LocalNetViewer";
    std::string host = "127.0.0.1";
    int port = 8765;
    std::string auth_token;
};

struct AppConfig {
    ServerCfg server;
    LibraryCfg library;
    GalleryCfg gallery;
    ComicCfg comic;
    RenderCfg render;
    CacheCfg cache;
    LimitsCfg limits;

    std::filesystem::path config_path;  // 可能为空（默认配置）
    std::filesystem::path base_dir;     // config.yaml 所在目录（相对路径基准）

    std::filesystem::path resolve(const std::string& value) const;
    std::filesystem::path cache_dir() const { return resolve(cache.dir); }
    std::filesystem::path fonts_dir() const { return resolve(render.fonts_dir); }
    std::filesystem::path parsed_cache_dir() const { return cache_dir() / "parsed"; }
    std::filesystem::path layouts_cache_dir() const { return cache_dir() / "layouts"; }
    std::filesystem::path index_path() const { return cache_dir() / "index.json"; }
    std::filesystem::path gallery_registry_path() const { return cache_dir() / "galleries.json"; }
    bool auth_required() const;

    const LibraryRootCfg* root_by_name(const std::string& name) const;
    std::optional<std::string> encoding_override(const std::string& vpath) const;
};

extern const char* const DEFAULT_INCLUDE_EXT[];
extern const size_t DEFAULT_INCLUDE_EXT_COUNT;
extern const char* const IMAGE_EXT[];
extern const size_t IMAGE_EXT_COUNT;

// path 为空时默认 exe 同级 ../server/config.yaml；不存在则返回内置默认值
AppConfig load_config(const std::optional<std::string>& path);
void ensure_dirs(const AppConfig& cfg);

}  // namespace lnv
