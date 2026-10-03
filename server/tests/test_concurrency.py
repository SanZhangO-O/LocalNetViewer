"""并发工具与进度存储（回归：线程泄漏、锁表竞态、记录无上限）。"""

from __future__ import annotations

import threading
import time
from concurrent.futures import ThreadPoolExecutor

import pytest

from app.formats.base import ParseTimeoutError
from app.services.progress import MAX_DEVICE_ID_LEN, normalize_device_id
from app.util.concurrency import KeyedLocks, run_with_timeout


# --------------------------------------------------------------- run_with_timeout
def test_returns_value():
    assert run_with_timeout(lambda: 42, 5, "计算") == 42


def test_zero_timeout_runs_inline():
    assert run_with_timeout(lambda: "x", 0, "计算") == "x"


def test_propagates_exception():
    def boom():
        raise ValueError("nope")

    with pytest.raises(ValueError):
        run_with_timeout(boom, 5, "计算")


def test_times_out():
    with pytest.raises(ParseTimeoutError):
        run_with_timeout(lambda: time.sleep(3), 0.2, "解析")


def test_timed_out_worker_is_a_daemon_thread():
    """超时后仍在跑的任务必须是守护线程。

    非守护线程会被 ``concurrent.futures.thread`` 的 atexit 钩子 join，
    表现成"进程退不掉"；反复超时还会无上限地堆积线程。
    """
    started = threading.Event()
    release = threading.Event()

    def slow():
        started.set()
        release.wait(5)

    with pytest.raises(ParseTimeoutError):
        run_with_timeout(slow, 0.2, "解析")
    assert started.wait(2)

    names = [t for t in threading.enumerate() if t.name == "lnv-timeout"]
    assert names, "工作线程应当仍在运行（超时无法中断线程）"
    assert all(t.daemon for t in names), "超时任务的线程必须是守护线程"
    release.set()


def test_same_key_is_not_run_twice_concurrently():
    """上一次超时后仍在跑时，同名任务必须直接失败，而不是重复做同一份重活。"""
    entered = threading.Event()
    release = threading.Event()
    calls = []

    def slow():
        calls.append(time.monotonic())
        entered.set()
        release.wait(5)
        return "done"

    with pytest.raises(ParseTimeoutError):
        run_with_timeout(slow, 0.2, "解析", key=("parse", "book-1"))
    assert entered.wait(2)

    with pytest.raises(ParseTimeoutError) as excinfo:
        run_with_timeout(slow, 5, "解析", key=("parse", "book-1"))
    assert "仍在进行中" in str(excinfo.value)
    assert len(calls) == 1, "同一 key 不应并发跑第二遍"
    release.set()


def test_different_keys_run_independently():
    results = []
    with ThreadPoolExecutor(max_workers=2) as pool:
        futures = [
            pool.submit(run_with_timeout, lambda i=i: i * 2, 5, "计算", key=("k", i))
            for i in range(2)
        ]
        results = [f.result() for f in futures]
    assert sorted(results) == [0, 2]


def test_concurrency_is_bounded():
    """并发上限必须真的生效：超时后仍在跑的任务也占着名额。"""
    from app.util import concurrency as mod

    runner = mod._TimedRunner(max_concurrent=2)
    release = threading.Event()
    started = threading.Semaphore(0)

    def slow():
        started.release()
        release.wait(30)  # 必须比 acquire 的等待窗口长得多

    try:
        for _ in range(2):
            with pytest.raises(ParseTimeoutError):
                runner.run(slow, 0.2, "解析")
        assert started.acquire(timeout=1) and started.acquire(timeout=1)

        # 两个名额都被"跑不完的任务"占着 → 第三个应当快速失败而不是无限等待
        with pytest.raises(ParseTimeoutError) as excinfo:
            runner.run(lambda: "x", 0.5, "解析")
        assert "繁忙" in str(excinfo.value)
    finally:
        release.set()


# --------------------------------------------------------------- KeyedLocks
def test_keyed_locks_are_mutually_exclusive():
    locks = KeyedLocks()
    inside = 0
    max_inside = 0
    guard = threading.Lock()

    def worker():
        nonlocal inside, max_inside
        for _ in range(40):
            with locks.acquire("same-key"):
                with guard:
                    inside += 1
                    max_inside = max(max_inside, inside)
                time.sleep(0)
                with guard:
                    inside -= 1

    threads = [threading.Thread(target=worker) for _ in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert max_inside == 1, f"同一 key 出现了 {max_inside} 个并发进入者"


def test_keyed_locks_eviction_does_not_break_exclusion():
    """锁表回收必须按引用计数判断。

    回归测试：旧实现回收"未被持有"的锁时，会误删一个"已被某线程取到、但还没
    acquire()"的锁对象；随后第三个线程为同一 key 建了新锁，于是两个线程同时进入
    临界区（同一本书会被并发分页两次）。

    注意要**按 key 分别计数** —— 不同 key 本来就应该能并发进入。
    """
    locks = KeyedLocks(max_keys=64)
    inside: dict = {}
    max_inside: dict = {}
    guard = threading.Lock()
    stop = threading.Event()

    def worker(key: str):
        while not stop.is_set():
            with locks.acquire(key):
                with guard:
                    inside[key] = inside.get(key, 0) + 1
                    max_inside[key] = max(max_inside.get(key, 0), inside[key])
                time.sleep(0)
                with guard:
                    inside[key] -= 1

    # 制造大量不同 key 以触发回收
    threads = [threading.Thread(target=worker, args=(f"key-{i % 4}",)) for i in range(12)]
    churn = [threading.Thread(target=worker, args=(f"churn-{i}",)) for i in range(6)]
    for t in threads + churn:
        t.start()
    time.sleep(0.6)
    stop.set()
    for t in threads + churn:
        t.join()

    assert max_inside, "测试没有真正并发跑起来"
    worst = max(max_inside.values())
    assert worst == 1, f"锁表回收后互斥被破坏（同一 key 出现 {worst} 个并发进入者）"


def test_keyed_locks_releases_on_exception():
    locks = KeyedLocks()
    with pytest.raises(RuntimeError):
        with locks.acquire("k"):
            raise RuntimeError("boom")
    # 仍然可以再次取得
    with locks.acquire("k"):
        pass


# --------------------------------------------------------------- 进度存储
def test_device_id_normalization():
    assert normalize_device_id(None) == "default"
    assert normalize_device_id("") == "default"
    assert normalize_device_id("   ") == "default"
    assert normalize_device_id("phone-a") == "phone-a"
    assert len(normalize_device_id("x" * 500)) == MAX_DEVICE_ID_LEN
    # 冒号会破坏 "book:device" 键结构，必须剥掉
    assert ":" not in normalize_device_id("a:b")
    assert normalize_device_id("a\nb") == "ab"
    # 全部被剥光时回落到 default，不能产出空键
    assert normalize_device_id(":::") == "default"


def test_progress_records_are_capped(context, monkeypatch):
    """记录数必须有上限：device_id 来自未校验的查询参数，不限量就能撑爆 progress.json。"""
    from app.services import progress as mod

    monkeypatch.setattr(mod, "MAX_RECORDS", 50)
    store = context.progress
    for i in range(80):
        store.put("book-x", f"dev{i}", {"chapter": 0, "block": 0, "char": 0}, page=i)
    assert store.count() <= 50


def test_progress_eviction_keeps_newest(context, monkeypatch):
    from app.services import progress as mod

    monkeypatch.setattr(mod, "MAX_RECORDS", 20)
    store = context.progress
    for i in range(30):
        store.put("book-y", f"dev{i}", {"chapter": 0, "block": 0, "char": 0}, page=i)
    newest = store.get("book-y", "dev29")
    assert newest is not None, "最新的记录不能被淘汰"
    assert newest["page"] == 29
    assert store.get("book-y", "dev0") is None, "最旧的记录应当被淘汰"


def test_progress_writes_are_coalesced(context, monkeypatch):
    """连续 PUT 不应每次都整文件重写（progress.json 是 O(n) 写）。"""
    from app.services import progress as mod

    store = context.progress
    written = []
    real_save = store.save

    def counting_save(*, force: bool = False):
        wrote = real_save(force=force)
        if wrote:
            written.append(force)
        return wrote

    monkeypatch.setattr(store, "save", counting_save)
    monkeypatch.setattr(mod, "SAVE_MIN_INTERVAL_S", 60.0)

    for i in range(20):
        store.put("book-c", "dev", {"chapter": 0, "block": 0, "char": 0}, page=i)

    assert len(written) <= 2, f"20 次 PUT 触发了 {len(written)} 次整文件写入"
    # 但 flush 必须把最后的状态真的写下去
    store.flush()
    assert written[-1] is True
    assert store.get("book-c", "dev")["page"] == 19
    # 落盘内容也必须是最后的状态
    import json

    on_disk = json.loads(store.path.read_text(encoding="utf-8"))
    assert on_disk["book-c:dev"]["page"] == 19


def test_progress_flush_is_noop_when_clean(context):
    store = context.progress
    store.save(force=True)
    assert store.flush() is None  # 不抛异常即可


def test_progress_get_any_returns_latest(context):
    store = context.progress
    store.put("book-z", "a", {"chapter": 0, "block": 0, "char": 0}, page=1)
    time.sleep(0.01)
    store.put("book-z", "b", {"chapter": 1, "block": 0, "char": 0}, page=5)
    latest = store.get_any("book-z")
    assert latest is not None and latest["page"] == 5
