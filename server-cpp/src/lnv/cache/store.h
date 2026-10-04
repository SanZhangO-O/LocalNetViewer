#pragma once
// 解析缓存 / 布局缓存（gzip + JSON 落盘；对应 Python cache/parsed.py、services/layouts.py 的磁盘层）
// gzip 容器：miniz raw deflate + 手写 gzip 头尾。
#include <mutex>
#include "lnv/formats/adapter.h"

namespace lnv {

// gzip 压缩/解压（level 默认 6；解压失败返回 false）
std::string gzip_compress(const std::string& data, int level = 6);
bool gzip_decompress(const std::string& data, std::string& out);

// 原子写文件（唯一 .tmp 名 + 原子覆盖改名）
bool atomic_write(const std::filesystem::path& path, const std::string& bytes);
// 目录配额：超限时按 mtime LRU 删除到 80%（skip 指定文件）
void enforce_dir_quota(const std::filesystem::path& dir, long long max_bytes,
                       const std::filesystem::path* keep = nullptr);

// MB → 字节：Python 侧是 int(parsed_max_mb) * 1024 * 1024（MiB）。用 1000*1000
// 会让共享同一缓存目录的两个实现在配额上差 4.6%，C++ 先删掉 Python 还能用的文件。
constexpr long long kMbToBytes = 1024LL * 1024LL;

// 解析器版本（Python cache/parsed.py PARSER_VERSION = "1"）。缓存文件名
// {book_id}.{version}.json.gz 的读写与 context 的失效路径必须共用同一常量，
// 否则版本升级后一处改了另一处没改，失效/命中会错位。
inline constexpr const char* kParsedCacheVersion = "1";

class ParsedCache {
public:
    ParsedCache(std::filesystem::path dir, long long max_mb)
        : dir_(std::move(dir)), max_bytes_(max_mb * kMbToBytes) {}

    // 未命中/损坏 → nullopt（损坏文件删除）
    std::optional<DocumentModel> get(const std::string& book_id) const;
    void put(const DocumentModel& doc);

private:
    std::filesystem::path dir_;
    long long max_bytes_;
    mutable std::mutex mu_;
};

// 布局缓存条目
struct LayoutEntry {
    LayoutProfile profile;
    PageMap pagemap;
};

class LayoutsDiskCache {
public:
    LayoutsDiskCache(std::filesystem::path dir, long long max_mb)
        : dir_(std::move(dir)), max_bytes_(max_mb * kMbToBytes) {}

    // {book_id}/{layout_id}.json.gz；未命中/损坏 → nullopt
    std::optional<LayoutEntry> get(const std::string& book_id,
                                   const std::string& layout_id) const;
    void put(const std::string& book_id, const LayoutEntry& entry);
    // 书籍指纹变化时清空该书全部派生缓存（layout 子目录）
    void invalidate_book(const std::string& book_id);

private:
    std::filesystem::path dir_;
    long long max_bytes_;
    mutable std::mutex mu_;
};

}  // namespace lnv
