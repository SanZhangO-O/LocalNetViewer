#include "lnv/config.h"
#include <yaml-cpp/yaml.h>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <windows.h>
#include "lnv/util/paths.h"

namespace lnv {

const char* const DEFAULT_INCLUDE_EXT[] = {".txt", ".epub", ".pdf", ".cbz", ".cbr",
    ".cb7", ".cbt", ".fb2", ".mobi", ".azw3"};
const size_t DEFAULT_INCLUDE_EXT_COUNT = 10;
const char* const IMAGE_EXT[] = {".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp",
    ".avif", ".jfif", ".tif", ".tiff"};
const size_t IMAGE_EXT_COUNT = 10;

std::filesystem::path AppConfig::resolve(const std::string& value) const {
    std::filesystem::path p = std::filesystem::u8path(value);
    return p.is_absolute() ? p : (base_dir / p);
}

bool AppConfig::auth_required() const {
    for (char c : server.auth_token)
        if (!std::isspace((unsigned char)c)) return true;
    return false;
}

const LibraryRootCfg* AppConfig::root_by_name(const std::string& name) const {
    for (const auto& r : library.roots)
        if (r.name == name) return &r;
    return nullptr;
}

std::optional<std::string> AppConfig::encoding_override(const std::string& vpath) const {
    const auto& ov = library.encoding_overrides;
    if (ov.empty()) return std::nullopt;
    if (auto it = ov.find(vpath); it != ov.end()) return it->second;
    std::string name = vpath;
    size_t pos = name.rfind('/');
    if (pos != std::string::npos) name = name.substr(pos + 1);
    if (auto it = ov.find(name); it != ov.end()) return it->second;
    for (const auto& [key, enc] : ov) {
        if (!key.empty()) {
            if (vpath.size() >= key.size() &&
                vpath.compare(vpath.size() - key.size(), key.size(), key) == 0)
                return enc;
            if (name == key) return enc;
        }
    }
    return std::nullopt;
}

namespace {

std::string expand_env(const std::string& raw) {
    // 对应 config.py:50-53 `os.path.expandvars(os.path.expanduser(v.strip()))`：
    // 支持 ~、$VAR、${VAR} 与 Windows 的 %VAR%。只认 %VAR% 时，
    // `path: ~/books` 会被当成字面量 → 书库解析失败、启动后空空如也。
    std::string s = raw;
    {   // strip
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        s = b == std::string::npos ? "" : s.substr(b, e - b + 1);
    }
    if (!s.empty() && s[0] == '~' && (s.size() == 1 || s[1] == '/' || s[1] == '\\')) {
        const char* home = std::getenv("USERPROFILE");
        if (!home || !*home) home = std::getenv("HOME");
        if (home && *home) s = std::string(home) + s.substr(1);
    }
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '%') {
            size_t j = s.find('%', i + 1);
            if (j != std::string::npos && j > i + 1) {
                std::string var = s.substr(i + 1, j - i - 1);
                char* v = nullptr;
                size_t sz = 0;
                if (!_dupenv_s(&v, &sz, var.c_str()) && v) {
                    out += v;
                    free(v);
                    i = j + 1;
                    continue;
                }
            }
        } else if (s[i] == '$' && i + 1 < s.size()) {
            size_t b = i + 1;
            size_t e = b;
            if (s[b] == '{') {
                size_t close = s.find('}', b + 1);
                if (close != std::string::npos) {
                    std::string var = s.substr(b + 1, close - b - 1);
                    const char* v = std::getenv(var.c_str());
                    if (v && *v) {
                        out += v;
                        i = close + 1;
                        continue;
                    }
                }
                out.push_back(s[i++]);
                continue;
            }
            while (e < s.size() && (std::isalnum((unsigned char)s[e]) || s[e] == '_')) ++e;
            if (e > b) {
                std::string var = s.substr(b, e - b);
                const char* v = std::getenv(var.c_str());
                if (v && *v) {
                    out += v;
                    i = e;
                    continue;
                }
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

std::string as_string(const YAML::Node& n, const std::string& def = "") {
    if (!n || n.IsNull()) return def;
    return n.as<std::string>();
}

std::vector<std::string> string_list(const YAML::Node& n) {
    std::vector<std::string> out;
    if (n && n.IsSequence())
        for (const auto& e : n) out.push_back(e.as<std::string>());
    return out;
}

LibraryCfg parse_library(const YAML::Node& n) {
    LibraryCfg c;
    // 默认扩展名表：Python 的 Field(default_factory=list(DEFAULT_INCLUDE_EXT)) 只在
    // **键缺失**时生效；显式写 [] 仍是空表（见下面 include_ext 分支）。
    // 少了这一步，只写 library.roots 的极简配置会得到空书库（审查 H5）。
    if (!n || !n["include_ext"]) {
        for (size_t i = 0; i < DEFAULT_INCLUDE_EXT_COUNT; i++)
            c.include_ext.push_back(DEFAULT_INCLUDE_EXT[i]);
    }
    if (!n) return c;
    if (n["roots"] && n["roots"].IsSequence()) {
        for (const auto& r : n["roots"]) {
            LibraryRootCfg root;
            root.name = as_string(r["name"]);
            std::string p = as_string(r["path"]);
            p = expand_env(p);
            // strip
            size_t b = p.find_first_not_of(" \t");
            size_t e = p.find_last_not_of(" \t");
            root.path = b == std::string::npos ? "" : p.substr(b, e - b + 1);
            c.roots.push_back(std::move(root));
        }
    }
    if (n["include_ext"]) {
        for (auto& e0 : string_list(n["include_ext"])) {
            std::string e = e0;
            size_t b = e.find_first_not_of(" \t");
            size_t e2 = e.find_last_not_of(" \t");
            e = b == std::string::npos ? "" : e.substr(b, e2 - b + 1);
            for (auto& ch : e) ch = char(::tolower((unsigned char)ch));
            if (e.empty()) continue;
            if (e[0] != '.') e.insert(e.begin(), '.');
            if (std::find(c.include_ext.begin(), c.include_ext.end(), e) == c.include_ext.end())
                c.include_ext.push_back(e);
        }
    }
    if (n["show_all_files"]) c.show_all_files = n["show_all_files"].as<bool>();
    if (n["sort"]) {
        // Python 是 Literal["natural","name"]：拼错会在加载期报错退出（exit 2），
        // 静默接受会让排序/行为按错误值走且没有任何提示（审查 L16）
        std::string sv = n["sort"].as<std::string>();
        if (sv != "natural" && sv != "name")
            throw ConfigError("library.sort 只支持 natural/name: " + sv);
        c.sort = sv;
    }
    if (n["encoding_overrides"] && n["encoding_overrides"].IsMap())
        for (const auto& kv : n["encoding_overrides"])
            c.encoding_overrides[kv.first.as<std::string>()] = kv.second.as<std::string>();
    return c;
}

}  // namespace

AppConfig load_config(const std::optional<std::string>& path) {
    namespace fs = std::filesystem;
    AppConfig cfg;
    fs::path cfg_path;
    if (path && !path->empty()) {
        cfg_path = fs::weakly_canonical(fs::u8path(*path));
    } else {
        // 默认：<repo>/server/config.yaml（对应 config.py:224-226 的
        // Path(__file__).parent.parent/"config.yaml"，与 cwd 无关）。
        // exe 在 server-cpp/build/ 下 → 上溯三级才是仓库根，上溯两级会得到
        // server-cpp/server/config.yaml（不存在）→ 静默以空书库启动。
        fs::path exe = [] {
            char buf[MAX_PATH];
            GetModuleFileNameA(nullptr, buf, MAX_PATH);
            return fs::path(buf);
        }();
        std::vector<fs::path> candidates = {
            exe.parent_path().parent_path().parent_path() / "server" / "config.yaml",
            exe.parent_path().parent_path() / "server" / "config.yaml",
            fs::current_path() / "server" / "config.yaml",
            fs::current_path() / "config.yaml",
        };
        cfg_path = candidates.front();
        for (const auto& c : candidates) {
            std::error_code ec;
            if (fs::exists(c, ec)) { cfg_path = c; break; }
        }
    }
    cfg.base_dir = cfg_path.parent_path();
    if (!fs::exists(cfg_path)) {
        cfg.config_path.clear();
        return cfg;
    }
    YAML::Node data;
    try {
        std::ifstream in(cfg_path, std::ios::binary);
        if (!in) throw ConfigError("配置文件无法读取: " + cfg_path.string());
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        data = YAML::Load(text);
    } catch (const YAML::Exception& e) {
        throw ConfigError("配置文件 YAML 解析失败: " + cfg_path.string() + "\n" + e.what());
    }
    if (data && !data.IsMap()) throw ConfigError("配置文件顶层必须是映射: " + cfg_path.string());

    try {
        if (data["server"]) {
            auto s = data["server"];
            if (s["name"]) cfg.server.name = s["name"].as<std::string>();
            if (s["host"]) cfg.server.host = s["host"].as<std::string>();
            if (s["port"]) {
                int p = s["port"].as<int>();
                if (p < 1 || p > 65535) throw std::runtime_error("port 越界");
                cfg.server.port = p;
            }
            if (s["auth_token"]) cfg.server.auth_token = s["auth_token"].as<std::string>();
        }
        cfg.library = parse_library(data["library"]);
        // 注意：include_ext 显式写 [] 时保持空（Python 的默认值只在键缺失时生效）。
        // 早期实现把空列表重新填成全部默认扩展名，等于无视用户的显式配置。
        if (data["gallery"]) {
            auto g = data["gallery"];
            if (g["folder_as_book"]) cfg.gallery.folder_as_book = g["folder_as_book"].as<bool>();
            if (g["min_images_in_folder"]) cfg.gallery.min_images_in_folder = g["min_images_in_folder"].as<int>();
            if (g["recursive"]) cfg.gallery.recursive = g["recursive"].as<bool>();
            if (g["show_image_files"]) cfg.gallery.show_image_files = g["show_image_files"].as<bool>();
        }
        if (data["comic"]) {
            auto c = data["comic"];
            if (c["skip_entries"]) cfg.comic.skip_entries = string_list(c["skip_entries"]);
            if (c["default_fit"]) {
                // Python 是 Literal["viewport","original"]（审查 L16）
                std::string fv = c["default_fit"].as<std::string>();
                if (fv != "viewport" && fv != "original")
                    throw ConfigError("comic.default_fit 只支持 viewport/original: " + fv);
                cfg.comic.default_fit = fv;
            }
            if (c["jpeg_quality"]) cfg.comic.jpeg_quality = c["jpeg_quality"].as<int>();
            if (c["strip_min_aspect"]) cfg.comic.strip_min_aspect = c["strip_min_aspect"].as<double>();
            if (c["strip_overlap_px"]) cfg.comic.strip_overlap_px = c["strip_overlap_px"].as<int>();
        }
        if (data["render"]) {
            auto r = data["render"];
            if (r["fonts_dir"]) cfg.render.fonts_dir = r["fonts_dir"].as<std::string>();
            if (r["default_font"]) cfg.render.default_font = r["default_font"].as<std::string>();
            if (r["max_page_pixels"]) cfg.render.max_page_pixels = r["max_page_pixels"].as<long long>();
            if (r["jpeg_for_photo_pages"]) cfg.render.jpeg_for_photo_pages = r["jpeg_for_photo_pages"].as<bool>();
        }
        if (data["cache"]) {
            auto c = data["cache"];
            if (c["dir"]) cfg.cache.dir = c["dir"].as<std::string>();
            if (c["parsed_max_mb"]) cfg.cache.parsed_max_mb = c["parsed_max_mb"].as<long long>();
            if (c["page_lru_bytes"]) cfg.cache.page_lru_bytes = c["page_lru_bytes"].as<long long>();
            if (c["page_lru_pages"]) cfg.cache.page_lru_pages = c["page_lru_pages"].as<long long>();
            if (c["layout_memory_max"]) cfg.cache.layout_memory_max = c["layout_memory_max"].as<int>();
            if (c["layouts_max_mb"]) cfg.cache.layouts_max_mb = c["layouts_max_mb"].as<long long>();
            if (c["doc_memory_max"]) cfg.cache.doc_memory_max = c["doc_memory_max"].as<int>();
        }
        if (data["limits"]) {
            auto l = data["limits"];
            if (l["max_book_mb"]) cfg.limits.max_book_mb = l["max_book_mb"].as<long long>();
            if (l["max_page_bytes"]) cfg.limits.max_page_bytes = l["max_page_bytes"].as<long long>();
            if (l["max_source_pixels"]) cfg.limits.max_source_pixels = l["max_source_pixels"].as<long long>();
            if (l["parse_timeout_s"]) cfg.limits.parse_timeout_s = l["parse_timeout_s"].as<double>();
            if (l["max_archive_entry_mb"]) cfg.limits.max_archive_entry_mb = l["max_archive_entry_mb"].as<long long>();
            if (l["max_archive_total_mb"]) cfg.limits.max_archive_total_mb = l["max_archive_total_mb"].as<long long>();
        }
    } catch (const YAML::Exception& e) {
        throw ConfigError("配置校验失败: " + cfg_path.string() + "\n" + e.what());
    }

    // 校验书库名
    std::set<std::string> names;
    for (const auto& r : cfg.library.roots) {
        if (!names.insert(r.name).second)
            throw ConfigError("library.roots 存在重名书库（虚拟路径第一段必须唯一）");
        if (r.name.find('/') != std::string::npos || r.name.find('\\') != std::string::npos)
            throw ConfigError("书库名不能包含路径分隔符: " + r.name);
    }
    // 数值范围校验（对应 config.py 的 pydantic Field 约束；Python 侧越界会
    // ConfigError → run.py 退出码 2，C++ 早期只校验 port，越界值被照单全收）
    auto check = [&](bool ok, const char* what) {
        if (!ok) throw ConfigError(std::string("配置校验失败: ") + what);
    };
    check(cfg.gallery.min_images_in_folder >= 0, "gallery.min_images_in_folder >= 0");
    check(cfg.comic.jpeg_quality >= 1 && cfg.comic.jpeg_quality <= 100,
          "comic.jpeg_quality 1..100");
    check(cfg.comic.strip_min_aspect > 0.0, "comic.strip_min_aspect > 0");
    check(cfg.comic.strip_overlap_px >= 0, "comic.strip_overlap_px >= 0");
    check(cfg.render.max_page_pixels >= 10000, "render.max_page_pixels >= 10000");
    check(cfg.cache.parsed_max_mb >= 0, "cache.parsed_max_mb >= 0");
    check(cfg.cache.page_lru_bytes >= 0, "cache.page_lru_bytes >= 0");
    check(cfg.cache.page_lru_pages >= 0, "cache.page_lru_pages >= 0");
    check(cfg.cache.layout_memory_max >= 1, "cache.layout_memory_max >= 1");
    check(cfg.cache.layouts_max_mb >= 0, "cache.layouts_max_mb >= 0");
    check(cfg.cache.doc_memory_max >= 1, "cache.doc_memory_max >= 1");
    check(cfg.limits.max_book_mb >= 0, "limits.max_book_mb >= 0");
    check(cfg.limits.max_page_bytes >= 0, "limits.max_page_bytes >= 0");
    check(cfg.limits.max_source_pixels >= 0, "limits.max_source_pixels >= 0");
    check(cfg.limits.parse_timeout_s > 0.0, "limits.parse_timeout_s > 0");
    check(cfg.limits.max_archive_entry_mb >= 0, "limits.max_archive_entry_mb >= 0");
    check(cfg.limits.max_archive_total_mb >= 0, "limits.max_archive_total_mb >= 0");
    cfg.config_path = cfg_path;
    return cfg;
}

void ensure_dirs(const AppConfig& cfg) {
    std::error_code ec;
    for (const auto& d : {cfg.cache_dir(), cfg.parsed_cache_dir(), cfg.layouts_cache_dir(),
                          cfg.fonts_dir()})
        std::filesystem::create_directories(d, ec);
}

}  // namespace lnv
