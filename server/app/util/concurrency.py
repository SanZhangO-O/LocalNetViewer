"""并发小工具：超时执行与按 key 加锁。"""

from __future__ import annotations

import threading
from contextlib import contextmanager
from typing import Any, Callable, Dict, Hashable, Iterator, Optional

from ..formats.base import ParseTimeoutError

# 同时在跑的"带超时任务"上限。超时并不会真的中断线程（Python 无法安全强杀），
# 所以必须有一个上限来兜住"不断超时 → 不断堆积重活"的情况。
MAX_CONCURRENT_TIMED_TASKS = 8


class _TimedRunner:
    """有界并发 + 每 key 去重的超时执行器。

    为什么不用 ``ThreadPoolExecutor``：它的工作线程是非守护线程，而且
    ``concurrent.futures.thread`` 注册了 atexit 钩子会 join 所有工作线程。
    一旦某个解析超时后仍在跑，``shutdown(wait=False)`` 既不会取消它、也不会回收它，
    结果是"进程退不掉"；反复超时还会无上限地堆线程与内存。这里改用守护线程，
    并把并发数压在上限内。
    """

    def __init__(self, max_concurrent: int = MAX_CONCURRENT_TIMED_TASKS) -> None:
        self._slots = threading.BoundedSemaphore(max(1, int(max_concurrent)))
        self._inflight: Dict[Hashable, int] = {}
        self._lock = threading.Lock()

    # ------------------------------------------------------------ 状态
    def inflight(self, key: Hashable) -> int:
        with self._lock:
            return self._inflight.get(key, 0)

    def _leave(self, key: Hashable) -> None:
        with self._lock:
            left = self._inflight.get(key, 0) - 1
            if left > 0:
                self._inflight[key] = left
            else:
                self._inflight.pop(key, None)

    # ------------------------------------------------------------ 执行
    def run(
        self,
        fn: Callable[[], Any],
        timeout_s: float,
        what: str,
        key: Optional[Hashable] = None,
    ) -> Any:
        # 去重必须在**等槽位之前**完成：槽位被跑不完的任务占满时（正是这个模块要
        # 处理的场景），后到的调用方会长时间停在 acquire 上；如果此时才登记 in-flight，
        # 同一 key 的多个调用会同时通过检查、把同一份重活跑好几遍。
        if key is not None:
            with self._lock:
                if self._inflight.get(key, 0) > 0:
                    raise ParseTimeoutError(f"{what}超时（上一次执行仍在进行中）")
                self._inflight[key] = 1

        wait = min(5.0, max(0.5, float(timeout_s)))
        try:
            acquired = self._slots.acquire(timeout=wait)
        except BaseException:
            if key is not None:
                self._leave(key)
            raise
        if not acquired:
            if key is not None:
                self._leave(key)
            raise ParseTimeoutError(
                f"{what}繁忙（并发任务已达上限 {MAX_CONCURRENT_TIMED_TASKS}，可能有任务卡住）"
            )

        box: Dict[str, Any] = {}
        done = threading.Event()

        def _target() -> None:
            try:
                box["value"] = fn()
            except BaseException as exc:  # noqa: BLE001 - 原样带回调用方线程
                box["error"] = exc
            finally:
                done.set()
                # 先清 key 再放槽位：反过来的话，若 release() 抛异常，
                # 这个 key 会永远停在"上一次仍在进行中"，那本书再也打不开
                if key is not None:
                    self._leave(key)
                # 槽位由任务本身释放：超时后任务仍在占用资源，调用方提前释放会让
                # 上限形同虚设
                self._slots.release()

        try:
            thread = threading.Thread(target=_target, name="lnv-timeout", daemon=True)
            thread.start()
        except BaseException:
            if key is not None:
                self._leave(key)
            self._slots.release()
            raise

        if not done.wait(timeout_s):
            raise ParseTimeoutError(f"{what}超时（>{timeout_s:.0f}s）")
        if "error" in box:
            raise box["error"]
        return box.get("value")


_RUNNER = _TimedRunner()


def run_with_timeout(
    fn: Callable[[], Any],
    timeout_s: float,
    what: str = "解析",
    *,
    key: Optional[Hashable] = None,
) -> Any:
    """在守护线程中执行 fn，超时抛 :class:`ParseTimeoutError`（设计 4.6 / 风险 15）。

    超时后底层线程仍会跑完（无法安全中断），因此这里只做三件事：
    1. 让调用方**尽快失败**，不阻塞请求线程；
    2. 用守护线程 + 有界并发，保证"跑不完的任务"不会拖住进程退出、也不会无限堆积；
    3. 传了 ``key`` 时，上一次同名任务还在跑就直接失败，避免重复做同一份重活
       （例如同一本书被重复解析）。

    注意：调用方**不能**依赖"超时 = 任务已停止"来释放自己的互斥锁。``ParsedCache``
    与 ``LayoutService`` 都靠 ``key`` 参数来挡住重复执行。
    """
    if not timeout_s or timeout_s <= 0:
        return fn()
    return _RUNNER.run(fn, float(timeout_s), what, key)


class KeyedLocks:
    """按 key 串行化的锁表（同一本书同一 layout 的首次分页加锁，设计 4.9）。"""

    def __init__(self, max_keys: int = 4096) -> None:
        self._locks: Dict[Hashable, threading.Lock] = {}
        self._users: Dict[Hashable, int] = {}
        self._guard = threading.Lock()
        self._max_keys = max(64, int(max_keys))

    @contextmanager
    def acquire(self, key: Hashable) -> Iterator[None]:
        """取得 key 对应的锁。

        回收必须按**引用计数**判断，不能看 ``lock.locked()``：线程 A 可能刚从
        ``_locks`` 取到锁对象、还没调用 ``acquire()``，此时 B 看到它"未锁定"就把它
        从表里删掉，C 随即为同一 key 建了一把新锁 —— 于是 A 与 C 同时进入临界区，
        同一本书会被并发分页两次。
        """
        with self._guard:
            lock = self._locks.get(key)
            if lock is None:
                lock = threading.Lock()
                self._locks[key] = lock
            self._users[key] = self._users.get(key, 0) + 1
        try:
            lock.acquire()
            try:
                yield
            finally:
                lock.release()
        finally:
            with self._guard:
                left = self._users.get(key, 1) - 1
                if left > 0:
                    self._users[key] = left
                else:
                    self._users.pop(key, None)
                if len(self._locks) > self._max_keys:
                    # 只回收"没有任何线程持有引用"的锁
                    for stale in [k for k in self._locks if k not in self._users][:128]:
                        self._locks.pop(stale, None)
