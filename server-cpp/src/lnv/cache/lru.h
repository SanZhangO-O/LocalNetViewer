#pragma once
// 内存 LRU 缓存（字节 + 条数双上限；对应 Python cache/lru.py 语义）
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace lnv {

class LruCache {
public:
    // max_bytes/max_items ≤0 = 不限该项；两者都 ≤0 = 关闭缓存（put 直接丢弃，get 恒空）
    LruCache(long long max_bytes, long long max_items);
    // 命中时把条目移到最新
    std::optional<std::string> get(const std::string& key);
    // value_size 超过字节上限的条目拒绝缓存
    void put(const std::string& key, std::string value, long long value_size);
    void clear();
    void remove_if(const std::function<bool(const std::string& key)>& pred);

private:
    struct Item {
        std::string key;
        std::string value;
        long long size;
    };
    mutable std::mutex mu_;
    std::list<Item> items_;  // front = 最新
    std::unordered_map<std::string, std::list<Item>::iterator> index_;
    long long max_bytes_, max_items_;
    long long cur_bytes_ = 0;
};

// 简单条目 LRU（任意值）
template <typename V>
class ObjectLru {
public:
    explicit ObjectLru(size_t max_items) : max_items_(max_items) {}
    std::optional<V> get(const std::string& key) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = index_.find(key);
        if (it == index_.end()) return std::nullopt;
        order_.splice(order_.begin(), order_, it->second.second);
        return it->second.first;
    }
    void put(const std::string& key, V value) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second.first = std::move(value);
            order_.splice(order_.begin(), order_, it->second.second);
            return;
        }
        if (max_items_ == 0) return;
        order_.push_front(key);
        index_[key] = {std::move(value), order_.begin()};
        if (order_.size() > max_items_) {
            index_.erase(order_.back());
            order_.pop_back();
        }
    }
    void remove_if(const std::function<bool(const std::string&)>& pred) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = order_.begin(); it != order_.end();) {
            if (pred(*it)) {
                index_.erase(*it);
                it = order_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    mutable std::mutex mu_;
    size_t max_items_;
    std::list<std::string> order_;
    std::unordered_map<std::string, std::pair<V, std::list<std::string>::iterator>> index_;
};

}  // namespace lnv
