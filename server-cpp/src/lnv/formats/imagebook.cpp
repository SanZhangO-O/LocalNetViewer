// 图片型书籍检测与固定版式适配实现（对应 Python formats/imagebook.py）。
#include "lnv/formats/imagebook.h"

#include <filesystem>
#include <list>
#include <mutex>
#include <unordered_map>

#include "lnv/formats/comics.h"
#include "lnv/formats/container.h"
#include "lnv/formats/epub.h"
#include "lnv/util/paths.h"

namespace lnv {
namespace {

// ---------------------------------------------------------------- 检测缓存
struct CachedInfo {
    long long ftime = 0;
    long long size = 0;
    ImageBookInfo info;
    std::list<std::string>::iterator pos;
};

struct InfoCache {
    std::mutex mu;
    std::list<std::string> mru;  // 最近使用在前
    std::unordered_map<std::string, CachedInfo> map;
};

constexpr size_t kDetectCacheLimit = 32;

InfoCache& info_cache() {
    static InfoCache* cache = new InfoCache();  // 故意不析构：超时线程可能仍在用
    return *cache;
}

// ---------------------------------------------------------------- 漫画容器
// 把"图片型 EPUB"的 spine 图片暴露成容器（条目名 = EPUB 内路径）。
// 接管 EpubAdapter 生命周期（zip 句柄随之关闭）。
class EpubImageContainer : public Container {
public:
    EpubImageContainer(std::unique_ptr<EpubAdapter> epub, std::vector<std::string> names)
        : epub_(std::move(epub)) {
        entries_.reserve(names.size());
        for (std::string& n : names) {
            ContainerEntry e;
            e.name = std::move(n);
            // size 留 0：EPUB 读取路径自带单条目守卫（max_archive_entry_mb），
            // 总量守卫在 Python/C++ 的 EPUB 路径同样不适用外部容器
            entries_.push_back(std::move(e));
        }
    }

    const std::vector<ContainerEntry>& entries() const override { return entries_; }

    std::string read(const std::string& name, long long /*entry_max_bytes*/) override {
        return epub_->read_entry_pub(name);
    }

private:
    std::unique_ptr<EpubAdapter> epub_;
    std::vector<ContainerEntry> entries_;
};

}  // namespace

ImageBookInfo image_book_info(EpubAdapter& epub) {
    const std::filesystem::path& path = epub.book_ref().path;
    const std::string key = path_to_utf8(path);
    std::error_code ec;
    auto ft = std::filesystem::last_write_time(path, ec);
    long long ftime = ec ? 0 : (long long)ft.time_since_epoch().count();
    ec.clear();
    auto fs = std::filesystem::file_size(path, ec);
    long long fsize = ec ? 0 : (long long)fs;

    InfoCache& cache = info_cache();
    {
        std::lock_guard<std::mutex> lk(cache.mu);
        auto it = cache.map.find(key);
        if (it != cache.map.end() && it->second.ftime == ftime && it->second.size == fsize) {
            cache.mru.splice(cache.mru.begin(), cache.mru, it->second.pos);
            return it->second.info;
        }
    }

    ImageBookInfo info = epub.scan_image_book();
    {
        std::lock_guard<std::mutex> lk(cache.mu);
        auto it = cache.map.find(key);
        if (it != cache.map.end()) cache.mru.erase(it->second.pos);
        cache.mru.push_front(key);
        cache.map[key] = CachedInfo{ftime, fsize, info, cache.mru.begin()};
        while (cache.mru.size() > kDetectCacheLimit) {
            const std::string& oldest = cache.mru.back();
            cache.map.erase(oldest);
            cache.mru.pop_back();
        }
    }
    return info;
}

std::unique_ptr<BookAdapter> make_epub_comic_adapter(const AppConfig& cfg, const BookRef& ref,
                                                     std::unique_ptr<EpubAdapter> epub,
                                                     const ImageBookInfo& info) {
    return std::make_unique<ComicAdapter>(
        cfg, ref,
        std::make_unique<EpubImageContainer>(std::move(epub), info.pages),
        info.title);
}

}  // namespace lnv
