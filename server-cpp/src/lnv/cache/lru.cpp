#include "lnv/cache/lru.h"

namespace lnv {

LruCache::LruCache(long long max_bytes, long long max_items)
    : max_bytes_(max_bytes), max_items_(max_items) {}

std::optional<std::string> LruCache::get(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    // 关闭缓存（双上限都 ≤0）时 put 不入表，get 恒空
    auto it = index_.find(key);
    if (it == index_.end()) return std::nullopt;
    items_.splice(items_.begin(), items_, it->second);
    return it->second->value;
}

void LruCache::put(const std::string& key, std::string value, long long value_size) {
    std::lock_guard<std::mutex> lk(mu_);
    if (max_bytes_ <= 0 && max_items_ <= 0) return;  // 完全关闭
    if (max_bytes_ > 0 && value_size > max_bytes_) return;  // 超单条上限拒绝
    auto it = index_.find(key);
    if (it != index_.end()) {
        cur_bytes_ -= it->second->size;
        it->second->value = std::move(value);
        it->second->size = value_size;
        cur_bytes_ += value_size;
        items_.splice(items_.begin(), items_, it->second);
    } else {
        items_.push_front({key, std::move(value), value_size});
        index_[key] = items_.begin();
        cur_bytes_ += value_size;
    }
    // 逐出（条数 + 字节双控）
    while (max_items_ > 0 && (long long)items_.size() > max_items_) {
        cur_bytes_ -= items_.back().size;
        index_.erase(items_.back().key);
        items_.pop_back();
    }
    while (max_bytes_ > 0 && cur_bytes_ > max_bytes_ && items_.size() > 1) {
        cur_bytes_ -= items_.back().size;
        index_.erase(items_.back().key);
        items_.pop_back();
    }
}

void LruCache::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    items_.clear();
    index_.clear();
    cur_bytes_ = 0;
}

void LruCache::remove_if(const std::function<bool(const std::string& key)>& pred) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = items_.begin(); it != items_.end();) {
        if (pred(it->key)) {
            cur_bytes_ -= it->size;
            index_.erase(it->key);
            it = items_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace lnv
