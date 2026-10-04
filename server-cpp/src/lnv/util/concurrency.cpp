// ParseGate 实现（对应 Python util/concurrency.py 的去重 + 有界并发语义）
#include "lnv/util/concurrency.h"

namespace lnv {

void ParseGate::acquire(const std::string& key, std::chrono::milliseconds wait,
                        const std::string& what) {
    // 与 Python _TimedRunner.run 完全同序：**先**登记 in-flight（去重），
    // **再**等槽位。反过来的话，槽位被卡死任务占满时多个同 key 调用会一起
    // 排队，拿到槽位后重复跑同一份重活。
    std::unique_lock<std::mutex> lk(mu_);
    if (!key.empty()) {
        if (inflight_.count(key)) {
            throw ServerBusyError(what + "正在进行中（同一任务去重），请稍后重试");
        }
        inflight_.insert(key);
    }
    bool ok = false;
    try {
        ok = cv_.wait_for(lk, wait, [&] { return slots_ < kMaxConcurrentParseTasks; });
    } catch (...) {
        // wait_for 极端情况下抛系统异常：撤销已登记的 key，不能留下永久去重
        if (!key.empty()) inflight_.erase(key);
        throw;
    }
    if (!ok) {
        if (!key.empty()) inflight_.erase(key);
        throw ServerBusyError(what + "繁忙（并发任务已达上限 " +
                              std::to_string(kMaxConcurrentParseTasks) +
                              "，可能有任务卡住），请稍后重试");
    }
    slots_++;
}

void ParseGate::release(const std::string& key) {
    // 先清 key 再放槽位：反过来的话，若中途出异常，这个 key 会永远
    // 停在"上一次仍在进行中"，那本书再也打不开。
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!key.empty()) inflight_.erase(key);
        if (slots_ > 0) slots_--;
    }
    cv_.notify_one();  // 唤醒等在 acquire() 上的任务
}

size_t ParseGate::active() const {
    std::lock_guard<std::mutex> lk(mu_);
    return slots_;
}

bool ParseGate::wait_idle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, timeout, [&] { return slots_ == 0; });
}

}  // namespace lnv
