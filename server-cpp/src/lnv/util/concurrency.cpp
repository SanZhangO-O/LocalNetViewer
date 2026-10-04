// ParseGate 实现（对应 Python util/concurrency.py 的去重 + 有界并发语义）
#include "lnv/util/concurrency.h"

namespace lnv {

bool ParseGate::try_acquire(const std::string& key) {
    // 去重检查与登记必须在同一临界区内完成：否则同一 key 的多个调用
    // 会同时通过检查，把同一份重活跑好几遍。
    std::lock_guard<std::mutex> lk(mu_);
    if (!key.empty() && inflight_.count(key)) {
        throw ServerBusyError("解析正在进行中（同一任务去重），请稍后重试");
    }
    if (slots_ >= kMaxConcurrentParseTasks) {
        throw ServerBusyError("解析繁忙（并发任务已达上限 8，可能有任务卡住），请稍后重试");
    }
    slots_++;
    if (!key.empty()) inflight_.insert(key);
    return true;
}

void ParseGate::release(const std::string& key) {
    // 先清 key 再放槽位：反过来的话，若中途出异常，这个 key 会永远
    // 停在"上一次仍在进行中"，那本书再也打不开。
    std::lock_guard<std::mutex> lk(mu_);
    if (!key.empty()) inflight_.erase(key);
    if (slots_ > 0) slots_--;
}

size_t ParseGate::active() const {
    std::lock_guard<std::mutex> lk(mu_);
    return slots_;
}

}  // namespace lnv
