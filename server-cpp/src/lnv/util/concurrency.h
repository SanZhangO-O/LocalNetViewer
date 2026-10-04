#pragma once
// 并发小工具：解析去重门（ParseGate）+ 超时等待 helper
// （对应 Python app/util/concurrency.py 的 _TimedRunner 语义）
#include <chrono>
#include <future>
#include <mutex>
#include <set>
#include <string>

#include "lnv/formats/adapter.h"

namespace lnv {

// 同时在跑的"带超时任务"上限（与 Python MAX_CONCURRENT_TIMED_TASKS 一致）。
// 超时并不会真的中断线程（C++ 同样无法安全强杀），必须有上限兜住
// "不断超时 → 不断堆积重活"的情况。
constexpr size_t kMaxConcurrentParseTasks = 8;

// 有界并发 + 按 key 去重的解析门。
// - try_acquire(key)：同一 key 已在跑 → 抛 ServerBusyError（同一任务去重）；
//   并发已达上限 → 抛 ServerBusyError（任务卡住提示）。成功则登记并占用一个槽位。
// - release(key)：任务结束（含异常路径）必须调用，Guard 供 RAII 使用。
class ParseGate {
public:
    // 成功返回 true 并占用；失败抛 ServerBusyError 且不占用
    bool try_acquire(const std::string& key);
    void release(const std::string& key);

    // RAII 守卫：析构自动 release
    struct Guard {
        ParseGate* gate = nullptr;
        std::string key;
        Guard() = default;
        Guard(ParseGate& g, std::string k) : gate(&g), key(std::move(k)) {}
        Guard(Guard&& o) noexcept : gate(o.gate), key(std::move(o.key)) { o.gate = nullptr; }
        Guard& operator=(Guard&& o) noexcept {
            if (this != &o) {
                reset();
                gate = o.gate;
                key = std::move(o.key);
                o.gate = nullptr;
            }
            return *this;
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        ~Guard() { reset(); }
        void reset() {
            if (gate) {
                gate->release(key);
                gate = nullptr;
            }
        }
    };

    size_t active() const;

private:
    mutable std::mutex mu_;
    std::set<std::string> inflight_;  // 正在跑的 key
    size_t slots_ = 0;                // 已占用的并发槽位数
};

// 等待 future 完成；超时返回 false（不中断线程，由调用方决定抛 ParseTimeoutError）。
template <typename T>
bool wait_for(std::future<T>& fut, std::chrono::seconds timeout) {
    return fut.wait_for(timeout) == std::future_status::ready;
}

}  // namespace lnv
