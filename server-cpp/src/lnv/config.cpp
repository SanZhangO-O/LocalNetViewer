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
    // 精确匹配：Python 的 dict 查找。键不重复（重复键在解析时已按 dict 语义合并），
    // 所以线性查找与哈希查找结果一致。
    auto find_key = [&ov](const std::string& key) -> const std::string* {
        for (const auto& [k, enc] : ov)
            if (k == key) return &enc;
        return nullptr;
    };
    if (const std::string* enc = find_key(vpath)) return *enc;
    std::string name = vpath;
    size_t pos = name.rfind('/');
    if (pos != std::string::npos) name = name.substr(pos + 1);
    if (const std::string* enc = find_key(name)) return *enc;
    // 后缀/文件名匹配按插入顺序（config.py:200-202 的 dict 遍历顺序）
    for (const auto& [key, enc] : ov) {
        if (!key.empty() && vpath.size() >= key.size() &&
            vpath.compare(vpath.size() - key.size(), key.size(), key) == 0)
            return enc;
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
    if (n["roots"]) {
        // Python：roots 是 List[LibraryRootCfg]，name/path 均为必填 str。此前缺字段
        // 时静默补空串，`naem:` 之类拼写错误被吞掉，启动后只表现为"书库不见了"，
        // 没有任何报错；这里按 config.py 的校验语义抛 ConfigError（与其它配置错误
        // 同一条异常路径：run.py/ main 统一打印并 exit 2）。
        if (n["roots"].IsNull() || !n["roots"].IsSequence())
            throw ConfigError("配置校验失败: library.roots 必须是列表");
        size_t idx = 0;
        for (const auto& r : n["roots"]) {
            const std::string at = "library.roots[" + std::to_string(idx) + "]";
            if (!r.IsMap())
                throw ConfigError("配置校验失败: " + at + " 必须是映射（name/path）");
            const YAML::Node name_node = r["name"];
            const YAML::Node path_node = r["path"];
            if (!name_node || name_node.IsNull())
                throw ConfigError("配置校验失败: " + at + ".name 缺失或为 null");
            if (!path_node || path_node.IsNull())
                throw ConfigError("配置校验失败: " + at + ".path 缺失或为 null");
            LibraryRootCfg root;
            root.name = name_node.as<std::string>();
            std::string p = path_node.as<std::string>();
            p = expand_env(p);
            // strip
            size_t b = p.find_first_not_of(" \t");
            size_t e = p.find_last_not_of(" \t");
            root.path = b == std::string::npos ? "" : p.substr(b, e - b + 1);
            c.roots.push_back(std::move(root));
            idx++;
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
    if (n["scan_mode"]) {
        // 与 Python 共用同名键：仅 lazy/eager，拼错启动即报错（同 sort 的处理）
        std::string sm = n["scan_mode"].as<std::string>();
        if (sm != "lazy" && sm != "eager")
            throw ConfigError("library.scan_mode 只支持 lazy/eager: " + sm);
        c.scan_mode = sm;
    }
    if (n["encoding_overrides"] && n["encoding_overrides"].IsMap()) {
        for (const auto& kv : n["encoding_overrides"]) {
            const std::string key = kv.first.as<std::string>();
            const std::string val = kv.second.as<std::string>();
            // PyYAML + dict：重复键保留"最后值 + 首次出现的位置"
            bool replaced = false;
            for (auto& [k, v] : c.encoding_overrides)
                if (k == key) { v = val; replaced = true; break; }
            if (!replaced) c.encoding_overrides.emplace_back(key, val);
        }
    }
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
        // GetModuleFileNameA 按 ANSI 代码页编码：中文用户名/安装路径会被截断成 '?'
        // 或乱码，默认配置根本找不到；改用宽字符 API + 动态扩容，直接得到原生
        // wchar_t 的 fs::path（fs::path 在 Windows 上本就是宽路径，无需再转换）。
        fs::path exe = [] {
            std::wstring buf(MAX_PATH, L'\0');
            for (;;) {
                const DWORD n = GetModuleFileNameW(nullptr, buf.data(),
                                                   static_cast<DWORD>(buf.size()));
                if (n == 0) return fs::path();
                // Vista+ 截断时返回 size 并置 ERROR_INSUFFICIENT_BUFFER → 翻倍重试
                if (n < buf.size()) {
                    buf.resize(n);
                    return fs::path(buf);
                }
                buf.resize(buf.size() * 2);
            }
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
