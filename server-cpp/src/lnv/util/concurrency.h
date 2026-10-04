#pragma once
// 并发小工具：解析闸门（ParseGate）+ 有界超时执行器（run_with_timeout）+ 按 key 阻塞锁
// （对应 Python app/util/concurrency.py 的 _TimedRunner / KeyedLocks 语义）
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "lnv/formats/adapter.h"

namespace lnv {

// 同时在跑的"带超时任务"上限（与 Python MAX_CONCURRENT_TIMED_TASKS 一致）。
// 超时并不会真的中断线程（C++ 同样无法安全强杀），必须有上限兜住
// "不断超时 → 不断堆积重活"的情况。
constexpr size_t kMaxConcurrentParseTasks = 8;

// 槽位等待窗口上限：Python _TimedRunner.run 的 wait = min(5.0, max(0.5, timeout_s))。
// 槽位被卡死任务占满时，新任务先等 5s 再报 503，而不是立即拒绝。
constexpr double kMaxSlotWaitS = 5.0;

// 重活闸门等待上限：超过就抛 ServerBusyError（HTTP 503），而不是无限期占住请求线程。
constexpr double kHeavyGateWaitS = 5.0;

// 有界并发 + 按 key 去重的解析门。
// - acquire(key, wait, what)：同一 key 已在跑 → 立即抛 ServerBusyError（同一任务去重）；
//   槽位满时最多等 wait 毫秒，仍满则抛 ServerBusyError（Python 语义）。
// - release(key)：任务结束（含异常路径）必须调用，Guard 供 RAII 使用。
class ParseGate {
public:
    // 等待版：同 key 在途 → 立即抛 ServerBusyError（去重必须在等槽位之前完成，
    // 否则多个同 key 调用会一起排队、拿到槽位后重复跑同一份重活）；槽位满 →
    // 等满 wait 后抛 ServerBusyError（消息含 what，与 Python 文案对齐）。
    // 若等待期间被异常打断，已登记的 key 会被清掉。
    void acquire(const std::string& key, std::chrono::milliseconds wait,
                 const std::string& what);
    void release(const std::string& key);

    // 关停用：等待所有槽位释放（即所有在跑的超时任务真正结束）；超时返回 false。
    // 超时后任务仍在跑（无法强杀），调用方自行决定是否继续关停。
    bool wait_idle(std::chrono::milliseconds timeout);

    // RAII 守卫：析构自动 release。
    // 占用在构造体内、成员 key 就位之后进行：这样 key 拷贝时的 bad_alloc
    // 发生在占用之前，不会留下无人释放的槽位（审查：Guard 构造抛异常泄漏槽位）。
    struct Guard {
        ParseGate* gate = nullptr;
        std::string key;
        Guard() = default;
        Guard(ParseGate& g, std::string k, std::chrono::milliseconds wait, std::string what)
            : gate(&g), key(std::move(k)) {
            gate->acquire(key, wait, what);
        }
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
    std::condition_variable cv_;      // 槽位释放唤醒等待者
    std::set<std::string> inflight_;  // 正在跑的 key
    size_t slots_ = 0;                // 已占用的并发槽位数
};

// 按 key 的阻塞互斥锁（对应 Python KeyedLocks）。
// 同一 key 串行、不同 key 并发；锁对象用 shared_ptr 保活：等待者即使遇到
// 锁表清理，也仍持有同一把锁，不会出现"A 刚取到、清理后 C 又建一把新锁"的双进入。
class KeyedLocks {
public:
    explicit KeyedLocks(size_t max_keys = 4096)
        : max_keys_(std::max<size_t>(64, max_keys)) {}

    class Guard {
    public:
        Guard() = default;
        Guard(KeyedLocks* owner, std::shared_ptr<std::mutex> m)
            : owner_(owner), mu_(std::move(m)) {
            lock_ = std::unique_lock<std::mutex>(*mu_);
        }
        Guard(Guard&& o) noexcept
            : owner_(o.owner_), mu_(std::move(o.mu_)), lock_(std::move(o.lock_)) {
            o.owner_ = nullptr;
        }
        Guard& operator=(Guard&& o) noexcept {
            if (this != &o) {
                reset();
                owner_ = o.owner_;
                mu_ = std::move(o.mu_);
                lock_ = std::move(o.lock_);
                o.owner_ = nullptr;
            }
            return *this;
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        ~Guard() { reset(); }
        void reset() {
            if (owner_) {
                lock_.unlock();
                owner_ = nullptr;
                mu_.reset();
            }
        }

    private:
        KeyedLocks* owner_ = nullptr;
        std::shared_ptr<std::mutex> mu_;
        std::unique_lock<std::mutex> lock_;
    };

    // 阻塞直到取得 key 的锁；RAII Guard 析构释放
    Guard acquire(const std::string& key) {
        std::shared_ptr<std::mutex> m;
        {
            std::lock_guard<std::mutex> g(mu_);
            auto& slot = table_[key];
            m = slot.lock();
            if (!m) {
                m = std::make_shared<std::mutex>();
                slot = m;
            }
            if (table_.size() > max_keys_) cleanup_locked();
        }
        return Guard(this, std::move(m));
    }

private:
    // 只回收已无人持有的锁（weak_ptr 过期 = 引用计数为 0）；每次最多 128 把，
    // 避免遍历成本随 key 数无限增长。
    void cleanup_locked() {
        size_t removed = 0;
        for (auto it = table_.begin(); it != table_.end() && removed < 128;) {
            if (it->second.expired()) {
                it = table_.erase(it);
                removed++;
            } else {
                ++it;
            }
        }
    }

    std::mutex mu_;
    std::unordered_map<std::string, std::weak_ptr<std::mutex>> table_;
    size_t max_keys_;
};

// 进程级"重活闸门"：限制同时占用请求线程的重活数量（默认 4 槽），避免 12 个
// 请求线程被 PDF 渲染、外部解压、MOBI 转换等慢操作全部占满而饿死轻量端点。
// - 有界等待（kHeavyGateWaitS = 5s）：槽位被卡死任务占满时不再无限排队，超时抛
//   ServerBusyError → HTTP 层映射为 503 SERVER_BUSY（与解析闸门同一错误语义）。
// - 可重入：同一线程嵌套获取不重复占槽（适配器构造 → MOBI 转换 / 容器列表）。
// - 锁序：持重活闸门时绝不等待解析闸门（ParseGate）；需要两者时先取重活闸门。
class HeavyGate {
public:
    explicit HeavyGate(size_t slots = 4) : slots_(slots ? slots : 1) {}

    class Guard {
    public:
        Guard() = default;
        explicit Guard(HeavyGate& gate) : gate_(&gate) { gate_->enter(); }
        Guard(Guard&& o) noexcept : gate_(o.gate_) { o.gate_ = nullptr; }
        Guard& operator=(Guard&& o) noexcept {
            if (this != &o) {
                reset();
                gate_ = o.gate_;
                o.gate_ = nullptr;
            }
            return *this;
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        ~Guard() { reset(); }
        void reset() {
            if (gate_) {
                gate_->leave();
                gate_ = nullptr;
            }
        }

    private:
        HeavyGate* gate_ = nullptr;
    };

    Guard acquire() { return Guard(*this); }

private:
    struct Tls {
        const HeavyGate* owner = nullptr;
        int depth = 0;
    };
    static Tls& tls() {
        thread_local Tls t;
        return t;
    }

    void enter() {
        Tls& t = tls();
        if (t.owner == this && t.depth > 0) {  // 同线程重入：不重复占槽
            t.depth++;
            return;
        }
        std::unique_lock<std::mutex> lk(mu_);
        // 有界等待：重活可达分钟级，但无限期占住请求线程会让所有端点（含轻量
        // 探测）一起排队、且永远拿不到 503。超时抛与解析闸门相同的 ServerBusyError。
        const auto wait_ms =
            std::chrono::milliseconds(static_cast<long long>(kHeavyGateWaitS * 1000.0));
        if (!cv_.wait_for(lk, wait_ms, [&] { return active_ < slots_; })) {
            throw ServerBusyError("服务器重活繁忙（并发重活已达上限 " +
                                  std::to_string(slots_) + "，可能有任务卡住），请稍后重试");
        }
        active_++;
        t.owner = this;
        t.depth = 1;
    }
    void leave() {
        Tls& t = tls();
        if (--t.depth > 0) return;
        t.owner = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (active_ > 0) active_--;
        }
        cv_.notify_one();
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    size_t slots_;
    size_t active_ = 0;
};

// 进程级单例（与 parse_gate 同理故意不析构：持有/等待者可能活到进程退出）
HeavyGate& heavy_gate();

namespace detail {
// 工作线程与等待方共享的结果槽：done 通过 mutex 发布 value/err
template <typename T>
struct TimedTaskState {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::exception_ptr err;
    std::optional<T> value;
};

template <>
struct TimedTaskState<void> {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::exception_ptr err;
};
}  // namespace detail

// 对应 Python run_with_timeout：
// - timeout_s <= 0 → 调用方线程直接执行（不占槽位、不去重）；
// - 否则在独立线程执行 fn：超时抛 ParseTimeoutError（任务继续在后台跑，槽位由
//   任务真正结束时释放，不强杀线程）；fn 的异常原样带回调用方；
// - key 非空时同 key 在途直接 ServerBusyError，槽位满等 kMaxSlotWaitS 后 503。
// 调用方必须保证 fn 捕获的对象在后台继续运行期间仍然有效（例如 DocumentModel
// 用 shared_ptr、适配器由 worker 自行打开）。
template <typename F>
auto run_with_timeout(ParseGate& gate, F&& fn, double timeout_s, const std::string& what,
                      const std::string& key = std::string())
    -> decltype(std::forward<F>(fn)()) {
    using T = decltype(std::forward<F>(fn)());
    if (timeout_s <= 0) return fn();
    const double wait_s = std::min(kMaxSlotWaitS, std::max(0.5, timeout_s));
    const auto wait_ms =
        std::chrono::milliseconds(static_cast<long long>(wait_s * 1000.0));
    // 去重/占槽在 Guard 构造体内完成；失败（ServerBusyError/异常）时 Guard
    // 未构造、不持有任何资源
    ParseGate::Guard guard(gate, key, wait_ms, what);
    auto state = std::make_shared<detail::TimedTaskState<T>>();
    std::thread([state, fn = std::forward<F>(fn), guard = std::move(guard)]() mutable {
        try {
            if constexpr (std::is_void_v<T>) {
                fn();
            } else {
                state->value.emplace(fn());
            }
        } catch (...) {
            state->err = std::current_exception();
        }
        {
            std::lock_guard<std::mutex> lk(state->m);
            state->done = true;
        }
        state->cv.notify_all();
        // guard 析构：done 之后先清 key 再放槽位（与 Python finally 顺序一致）
    }).detach();
    std::unique_lock<std::mutex> lk(state->m);
    if (!state->cv.wait_for(lk, std::chrono::duration<double>(timeout_s),
                            [&] { return state->done; })) {
        throw ParseTimeoutError(what + "超时（>" +
                                std::to_string(std::llround(timeout_s)) + "s）");
    }
    if (state->err) std::rethrow_exception(state->err);
    if constexpr (!std::is_void_v<T>) {
        return std::move(*state->value);
    }
}

}  // namespace lnv
