// 磁盘缓存实现：gzip 容器 / 原子写 / 配额清理 / 解析缓存 / 布局缓存
// 对应 Python cache/parsed.py、cache/store.py（services/layouts.py 的磁盘层）。
// 落盘格式与 Python 逐字节兼容（gzip 内为紧凑 UTF-8 JSON，ensure_ascii=false）。
#include "lnv/cache/store.h"

#include <miniz.h>
#include <process.h>  // _getpid

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // MoveFileExW（原子覆盖改名）

namespace lnv {

namespace {

// 解析器版本（Python cache/parsed.py PARSER_VERSION = "1"）
constexpr const char* kParserVersion = "1";

// file_time → epoch 秒（浮点），用于配额清理的 mtime 排序
double file_time_to_epoch(const std::filesystem::file_time_type& t) {
    auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
    return std::chrono::duration_cast<std::chrono::duration<double>>(sys.time_since_epoch())
        .count();
}

// 读取整个文件（二进制）；失败返回 false
bool read_file_bytes(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// 跳过 gzip 头（RFC 1952）：魔术字 + CM + FLG 的 FEXTRA/FNAME/FCOMMENT/FHCRC。
// Python gzip.open(path, "wt") 会写入 FNAME（文件基名），必须正确跳过。
bool gzip_skip_header(const std::string& d, size_t& pos) {
    if (d.size() < 18) return false;  // 10 字节头 + 8 字节尾最少
    if ((unsigned char)d[0] != 0x1F || (unsigned char)d[1] != 0x8B) return false;
    if (d[2] != 0x08) return false;  // CM = deflate
    unsigned char flg = (unsigned char)d[3];
    pos = 10;
    if (flg & 0x04) {  // FEXTRA
        if (pos + 2 > d.size()) return false;
        unsigned xlen = (unsigned char)d[pos] | ((unsigned)(unsigned char)d[pos + 1] << 8);
        pos += 2 + xlen;
        if (pos > d.size()) return false;
    }
    if (flg & 0x08) {  // FNAME（零结尾）
        while (pos < d.size() && d[pos] != '\0') pos++;
        if (pos >= d.size()) return false;
        pos++;
    }
    if (flg & 0x10) {  // FCOMMENT（零结尾）
        while (pos < d.size() && d[pos] != '\0') pos++;
        if (pos >= d.size()) return false;
        pos++;
    }
    if (flg & 0x02) {  // FHCRC
        if (pos + 2 > d.size()) return false;
        pos += 2;
    }
    return true;
}

}  // namespace

// ------------------------------------------------------------------ gzip

std::string gzip_compress(const std::string& data, int level) {
    // raw deflate（无 zlib 包装），与 Python gzip 模块使用同一原始流
    std::string out;
    out.reserve(data.size() + data.size() / 8 + 64);
    // 头：1F 8B 08 00 00 00 00 00 00 FF（无 FLG、mtime=0 保证输出确定性、OS=unknown）
    const unsigned char hdr[10] = {0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};
    out.append(reinterpret_cast<const char*>(hdr), sizeof(hdr));

    if (!data.empty()) {
        size_t deflated_len = 0;
        // window_bits 取负 → raw deflate（无 zlib 78 9C 包装，与 gzip 载荷一致）
        int flags = tdefl_create_comp_flags_from_zip_params(level, -15, MZ_DEFAULT_STRATEGY);
        void* deflated =
            tdefl_compress_mem_to_heap(data.data(), data.size(), &deflated_len, flags);
        if (!deflated) return std::string();  // 压缩失败：空串交由调用方放弃写盘
        out.append(static_cast<const char*>(deflated), deflated_len);
        mz_free(deflated);
    } else {
        // 空输入：手写"最终存储块（长度 0）"，保证仍是合法 raw deflate 流
        const unsigned char empty_block[5] = {0x01, 0x00, 0x00, 0xFF, 0xFF};
        out.append(reinterpret_cast<const char*>(empty_block), sizeof(empty_block));
    }

    // 尾：CRC32（mz_crc32 与 zlib crc32 同多项式）+ ISIZE，均小端
    mz_ulong crc = mz_crc32(0, reinterpret_cast<const mz_uint8*>(data.data()), data.size());
    mz_uint32 isize = static_cast<mz_uint32>(data.size() & 0xFFFFFFFFu);
    unsigned char tail[8];
    for (int i = 0; i < 4; i++) tail[i] = static_cast<unsigned char>((crc >> (8 * i)) & 0xFF);
    for (int i = 0; i < 4; i++)
        tail[4 + i] = static_cast<unsigned char>((isize >> (8 * i)) & 0xFF);
    out.append(reinterpret_cast<const char*>(tail), sizeof(tail));
    return out;
}

bool gzip_decompress(const std::string& data, std::string& out) {
    size_t pos = 0;
    if (!gzip_skip_header(data, pos)) return false;
    if (data.size() < pos + 8) return false;
    // 尾部 8 字节：CRC32 + ISIZE（小端）；ISIZE 给出解压后大小的低 32 位
    size_t payload_len = data.size() - pos - 8;
    mz_uint32 isize = 0;
    for (int i = 0; i < 4; i++)
        isize |= static_cast<mz_uint32>((unsigned char)data[data.size() - 4 + i]) << (8 * i);
    // 防御上限（512 MiB）：正常文档缓存远小于此
    constexpr mz_uint32 kMaxOut = 512u * 1024 * 1024;
    if (isize > kMaxOut) return false;
    std::string result;
    try {
        result.resize(isize);
    } catch (...) {
        return false;
    }
    size_t out_len = 0;
    if (isize > 0) {
        out_len = tinfl_decompress_mem_to_mem(result.data(), result.size(), data.data() + pos,
                                              payload_len,
                                              TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (out_len == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) return false;
    }
    if (out_len != isize) return false;  // 与 ISIZE 不符 → 视为损坏
    // CRC32 校验
    mz_ulong crc =
        mz_crc32(0, reinterpret_cast<const mz_uint8*>(result.data()), result.size());
    mz_uint32 crc_stored = 0;
    for (int i = 0; i < 4; i++)
        crc_stored |= static_cast<mz_uint32>((unsigned char)data[data.size() - 8 + i]) << (8 * i);
    if (static_cast<mz_uint32>(crc) != crc_stored) return false;
    out = std::move(result);
    return true;
}

// ------------------------------------------------------------------ 文件工具

bool atomic_write(const std::filesystem::path& path, const std::string& bytes) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        ec.clear();
    }
    // .tmp 名带进程号 + 单调计数：Python 侧同样是
    // `{path}.tmp{pid}.{time_ns()}` / `.tmp{pid}.{generation}`。
    // 固定 ".tmp" 会让两个并发请求（12 线程池 + 每次 /files 都可能触发 maybe_save）
    // 同时截断同一个临时文件，互相覆盖对方的字节 → index.json / galleries.json /
    // progress.json 变成半截 JSON，下次启动直接丢失整个索引/进度。
    static std::atomic<unsigned long long> seq{0};
    static const unsigned long long pid = static_cast<unsigned long long>(::_getpid());
    std::filesystem::path tmp = path;
    tmp += ".tmp" + std::to_string(pid) + "." + std::to_string(seq.fetch_add(1));
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    // 原子覆盖：Windows 的 std::filesystem::rename 不覆盖已存在目标，所以走
    // MoveFileExW(MOVEFILE_REPLACE_EXISTING)。Python 用 os.replace，语义相同。
    // 早期实现是 remove + rename：两步之间文件**不存在**，此刻崩溃/被杀就会丢掉
    // 整个 index.json / progress.json（缓存目录还与 Python 版共用）。
    if (!MoveFileExW(tmp.wstring().c_str(), path.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

void enforce_dir_quota(const std::filesystem::path& dir, long long max_bytes,
                       const std::filesystem::path* keep) {
    if (max_bytes <= 0) return;  // 0 = 不限
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return;
    // 递归收集文件（mtime, size, path），超限从最旧删到 80%
    struct Item {
        double mtime;
        long long size;
        std::filesystem::path path;
    };
    std::vector<Item> items;
    long long total = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) { ec.clear(); continue; }
        auto t = std::filesystem::last_write_time(it->path(), ec);
        if (ec) { ec.clear(); continue; }
        long long sz = static_cast<long long>(it->file_size(ec));
        if (ec) { ec.clear(); continue; }
        items.push_back({file_time_to_epoch(t), sz, it->path()});
        total += sz;
    }
    if (total <= max_bytes) return;
    long long target = static_cast<long long>(static_cast<double>(max_bytes) * 0.8);
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.mtime < b.mtime; });
    for (const auto& item : items) {
        if (total <= target) break;
        if (keep && item.path == *keep) continue;
        std::filesystem::remove(item.path, ec);
        if (!ec) total -= item.size;
        ec.clear();
    }
}

// ------------------------------------------------------------------ ParsedCache

std::optional<DocumentModel> ParsedCache::get(const std::string& book_id) const {
    std::filesystem::path path = dir_ / (book_id + "." + kParserVersion + ".json.gz");
    std::string raw;
    // 文件读取不加锁：mu_ 保护的是"配额清理"这段临界区（读-算-删），
    // 不是文件 IO。此前把 gzip 读取也塞进锁里，一本大书冷读会阻塞所有其他书的
    // 页请求与 /layout（审查 M7）。同名文件的并发读本身是安全的。
    if (!read_file_bytes(path, raw)) return std::nullopt;
    std::string plain;
    DocumentModel doc;
    // 解压 / JSON / from_dict 任一失败、或 book_id 不符 → 删除文件（损坏缓存）
    bool ok = gzip_decompress(raw, plain);
    if (ok) {
        try {
            doc = DocumentModel::from_dict(nlohmann::json::parse(plain));
        } catch (...) {
            ok = false;
        }
    }
    if (ok && doc.book_id.empty()) doc.book_id = book_id;  // 兼容 Python 的空 id 回填
    if (ok && !doc.book_id.empty() && doc.book_id != book_id) ok = false;
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return std::nullopt;
    }
    return doc;
}

void ParsedCache::put(const DocumentModel& doc) {
    if (doc.book_id.empty()) return;
    std::filesystem::path path = dir_ / (doc.book_id + "." + kParserVersion + ".json.gz");
    std::string plain = doc.to_dict().dump();  // 紧凑分隔符，非 ASCII 原样输出
    std::string gz = gzip_compress(plain, 6);
    if (gz.empty()) return;
    // 写入不加锁（atomic_write 用唯一临时名 + 原子改名，天然并发安全）；
    // 只有配额清理需要串行化 —— 它要读全目录大小再删文件（审查 M7）
    if (!atomic_write(path, gz)) return;
    std::lock_guard<std::mutex> lk(mu_);
    enforce_dir_quota(dir_, max_bytes_, &path);
}

// ------------------------------------------------------------------ LayoutsDiskCache

std::optional<LayoutEntry> LayoutsDiskCache::get(const std::string& book_id,
                                                 const std::string& layout_id) const {
    std::filesystem::path path = dir_ / book_id / (layout_id + ".json.gz");
    std::string raw;
    // 文件读取不加锁（同 ParsedCache::get，审查 M7）
    if (!read_file_bytes(path, raw)) return std::nullopt;
    // 损坏缓存（解压失败 / JSON 错 / 校验失败）直接删除
    auto fail = [&]() -> std::optional<LayoutEntry> {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return std::nullopt;
    };
    std::string plain;
    if (!gzip_decompress(raw, plain)) return fail();
    nlohmann::json data;
    try {
        data = nlohmann::json::parse(plain);
    } catch (...) {
        return fail();
    }
    // 身份校验：拿到别的书 / 别的 layout 的 PageMap 比没有缓存更糟
    std::string bid = data.value("book_id", std::string());
    std::string lid = data.value("layout_id", std::string());
    if (!bid.empty() && bid != book_id) return fail();
    if (!lid.empty() && lid != layout_id) return fail();
    LayoutEntry entry;
    try {
        entry.profile = LayoutProfile::from_dict(data.value("profile", nlohmann::json::object()));
        entry.pagemap = PageMap::from_dict(data.value("pagemap", nlohmann::json::object()));
    } catch (...) {
        return fail();
    }
    if (entry.pagemap.layout_id.empty()) entry.pagemap.layout_id = layout_id;
    return entry;
}

void LayoutsDiskCache::put(const std::string& book_id, const LayoutEntry& entry) {
    // layout_id 以 pagemap 内记录为准（与 Python 写入 payload["layout_id"] 一致）
    std::string layout_id = entry.pagemap.layout_id;
    if (layout_id.empty()) return;
    nlohmann::json payload = {
        {"book_id", book_id},
        {"layout_id", layout_id},
        {"profile", entry.profile.to_json()},
        {"pagemap", entry.pagemap.to_dict()},
    };
    std::string gz = gzip_compress(payload.dump(), 6);
    if (gz.empty()) return;
    std::filesystem::path path = dir_ / book_id / (layout_id + ".json.gz");
    // 写入不加锁（atomic_write 并发安全）；只有配额清理串行化（审查 M7）
    if (!atomic_write(path, gz)) return;
    std::lock_guard<std::mutex> lk(mu_);
    enforce_dir_quota(dir_, max_bytes_, &path);
}

void LayoutsDiskCache::invalidate_book(const std::string& book_id) {
    std::error_code ec;
    std::filesystem::remove_all(dir_ / book_id, ec);
}

}  // namespace lnv
