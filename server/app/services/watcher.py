"""书库目录变化监听（设计 4.8 / pyproject 可选依赖 ``watch``）。

- 用 watchfiles 监听全部书库根：它是 OS 原生通知的封装（Windows
  ``ReadDirectoryChangesW`` / Linux inotify / macOS FSEvents），内核推送事件，
  空闲时零 CPU，开销只随事件频率涨、不随书库大小涨；
- 两个线程分工：消费线程跑 watchfiles 事件循环，**只累计**待处理事件数（不做
  任何 IO）；扫描线程在"有待处理事件且越过节流间隔"时合并触发一次
  :class:`LibraryService` 增量重扫（按 mtime/size 跳过未变文件）。持续写入的
  大文件拷贝由此被限制为每个节流间隔最多一次重扫，队列里的事件合并进下一轮，
  既无事件风暴也不丢事件；
- ``scan_blocking`` 在拿不到扫描锁时会静默跳过，扫描线程据此重试直到扫描真正
  执行——watchfiles 不会重新投递已消费的事件，放弃重试等于丢一批变化；
- watchfiles 未安装、根目录不存在或监听异常时自动降级为"仅启动扫描"，只记录
  日志，不影响服务运行。
"""

from __future__ import annotations

import logging
import os
import threading
import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

from ..config import AppConfig
from ..formats.comics import should_skip_entry
from .library import LibraryService

logger = logging.getLogger("localnetviewer")

try:  # 可选依赖：pip install -e ".[watch]"
    from watchfiles import Change, DefaultFilter, watch
except ImportError:  # pragma: no cover - 环境相关，安装 watch 后即覆盖
    Change = None  # type: ignore[assignment,misc]
    DefaultFilter = None  # type: ignore[assignment,misc]
    watch = None  # type: ignore[assignment]

# 等既有扫描结束 / 重试拿锁的总上限（启动全量扫描大书库可能要几分钟）
_WAIT_IDLE_TIMEOUT_S = 300.0
# stop() 等待线程退出的上限：超时不强杀，由关停流程的 wait_idle 兜底
_STOP_JOIN_TIMEOUT_S = 10.0
# 持续变化期间的扫描间隔下限：窗口内事件只累计、合并进下一次扫描
_DEFAULT_MIN_INTERVAL_S = 10.0


class _LibraryFilter:
    """watchfiles 事件过滤器：默认过滤（隐藏文件、.git 等）+ comic.skip_entries。

    只影响"是否触发重扫"，不影响扫描本身的跳过规则；误放行的事件最多多一次
    增量扫描，误过滤的事件才需要警惕（所以这里宁可宽、不可严）。
    """

    def __init__(self, skip_patterns: List[str]) -> None:
        self._patterns = list(skip_patterns)
        self._default = DefaultFilter() if DefaultFilter is not None else None

    def __call__(self, change: "Change", path: str) -> bool:
        if self._default is not None and not self._default(change, path):
            return False
        return not should_skip_entry(os.path.basename(path), self._patterns)


class LibraryWatcher:
    """监听书库根目录变化 → 合并节流 → 触发增量重扫（消费 + 扫描双线程）。"""

    def __init__(
        self,
        config: AppConfig,
        library: LibraryService,
        on_change: Optional[Callable[[], bool]] = None,
        *,
        min_rescan_interval: Optional[float] = None,
    ) -> None:
        self.config = config
        self.library = library
        # 约定：回调返回"是否真正执行了扫描"（scan_blocking 的语义），
        # 返回 False 时扫描线程会重试
        self._on_change = on_change or library.scan_blocking
        self._filter = _LibraryFilter(list(config.comic.skip_entries))
        self._paths: List[Path] = []
        self._threads: List[threading.Thread] = []
        self._stop_event = threading.Event()
        # 待处理事件数（消费线程累加、扫描线程一次性取走）与唤醒位
        self._pending_lock = threading.Lock()
        self._pending_count = 0
        self._pending_event = threading.Event()
        self._ready_event = threading.Event()  # 事件循环即将开始（测试等写入方据此避免早于注册的变更）
        self._last_scan_mono = 0.0  # 上次重扫完成时刻（monotonic）
        # 持续变化期间的扫描节奏下限；测试可传小值加速
        self._min_interval_s = (
            float(min_rescan_interval)
            if min_rescan_interval is not None
            else max(
                2.0 * float(config.library.watch_debounce_s),
                _DEFAULT_MIN_INTERVAL_S,
            )
        )
        self._failed: Optional[str] = None  # 未启用/异常退出的原因（诊断用）

    # ------------------------------------------------------------ 生命周期
    def start(self) -> None:
        """启动监听；任何前置条件不满足都只记日志并降级，绝不抛出。"""
        if not bool(self.config.library.watch):
            logger.debug("书库监控未启用（library.watch=false）")
            self._failed = "library.watch=false"
            return
        if watch is None:
            logger.warning(
                "library.watch=true 但未安装 watchfiles，书库变化监听未启用"
                "（可选安装: pip install -e \".[watch]\"）"
            )
            self._failed = "未安装 watchfiles"
            return
        roots = [Path(r.path) for r in self.config.library.roots]
        for path in roots:
            if not path.is_dir():
                logger.warning("书库根目录不存在，无法监听: %s", path)
        paths = [p for p in roots if p.is_dir()]
        if not paths:
            self._failed = "没有可监听的书库根目录"
            logger.info("书库监控未启动：%s", self._failed)
            return
        self._paths = paths
        self._stop_event.clear()
        self._threads = [
            threading.Thread(target=self._consume, name="library-watch", daemon=True),
            threading.Thread(target=self._scan_loop, name="library-rescan", daemon=True),
        ]
        for thread in self._threads:
            thread.start()

    def stop(self) -> None:
        """请求停止并等待线程退出；幂等，可安全地在未启动时调用。"""
        self._stop_event.set()
        self._pending_event.set()  # 唤醒可能停在 wait() 上的扫描线程
        for thread in self._threads:
            if thread.is_alive():
                thread.join(timeout=_STOP_JOIN_TIMEOUT_S)
                if thread.is_alive():
                    # 线程可能正在执行一次重扫：cancel_scan 之后扫描会协作式
                    # 退出，关停流程的 wait_idle 会等它真正结束，这里只记录
                    logger.warning("书库监听线程未在 %.0fs 内退出", _STOP_JOIN_TIMEOUT_S)
        self._threads = []

    @property
    def active(self) -> bool:
        return any(t.is_alive() for t in self._threads)

    @property
    def ready(self) -> bool:
        """消费线程已进入事件循环（watch 注册完成前后毫秒级，测试用）。"""
        return self._ready_event.is_set()

    def status(self) -> Dict[str, Any]:
        cfg = self.config.library
        with self._pending_lock:
            pending = self._pending_count
        return {
            "enabled": bool(cfg.watch),
            "available": watch is not None,
            "active": self.active,
            "debounce_s": float(cfg.watch_debounce_s),
            "min_interval_s": self._min_interval_s,
            "pending_changes": pending,
            "paths": [str(p) for p in self._paths],
            "error": self._failed,
        }

    # ------------------------------------------------------------ 消费线程
    def _consume(self) -> None:
        debounce_ms = int(max(0.1, float(self.config.library.watch_debounce_s)) * 1000)
        logger.info(
            "书库监控已启动（%d 个根，防抖 %.1fs，节流 %.1fs）",
            len(self._paths),
            debounce_ms / 1000.0,
            self._min_interval_s,
        )
        self._ready_event.set()
        try:
            for changes in watch(
                *(str(p) for p in self._paths),
                watch_filter=self._filter,
                stop_event=self._stop_event,
                # debounce=step：静默满防抖窗口即产出一批（正常零星改动）；
                # 持续变化达到窗口上限也产出一批（大拷贝不会憋事件）。
                # 批次频率本身无害——这里只累计计数，扫描节奏由扫描线程控制
                debounce=debounce_ms,
                step=debounce_ms,
                # 网络盘/受限目录个别子树拒访时降级而不是整个监听失败
                ignore_permission_denied=True,
                # 非主线程收不到 Ctrl+C，抑制 signal 分支的 KeyboardInterrupt
                raise_interrupt=False,
            ):
                if self._stop_event.is_set():
                    break
                with self._pending_lock:
                    self._pending_count += len(changes)
                self._pending_event.set()
        except Exception as exc:  # noqa: BLE001 - 监听失败只降级，不拖垮服务
            self._failed = str(exc)
            if not self._stop_event.is_set():
                logger.warning("书库监控异常退出，退化为仅启动扫描: %s", exc)
        else:
            logger.info("书库监控已停止")

    # ------------------------------------------------------------ 扫描线程
    def _scan_loop(self) -> None:
        # 引导重扫：消费线程进入事件循环（watch 注册）之前发生的变更不会产生
        # 事件，实测高负载下这个窗口可达秒级；启动后先做一次增量扫描兜底，
        # 把窗口内错过的一切补进索引（无变化时不落盘，代价只是一次目录遍历）
        if self._stop_event.wait(
            max(1.0, float(self.config.library.watch_debounce_s))
        ):
            return
        logger.info("书库监听引导扫描（覆盖注册窗口内错过的变更）")
        if self._rescan_until_ran(0):
            self._last_scan_mono = time.monotonic()
        while not self._stop_event.is_set():
            if not self._pending_event.wait(0.5):
                continue
            if self._stop_event.is_set():
                break
            # 节流：距上次重扫不足间隔时先等够（期间新事件只累计）。
            # 长时间空闲后的单次零星改动不受影响——last_scan 已是很久以前
            elapsed = time.monotonic() - self._last_scan_mono
            remaining = self._min_interval_s - elapsed
            if remaining > 0 and self._stop_event.wait(remaining):
                break
            with self._pending_lock:
                count = self._pending_count
                self._pending_count = 0
                self._pending_event.clear()
            if count <= 0 or self._stop_event.is_set():
                continue
            if self._rescan_until_ran(count):
                self._last_scan_mono = time.monotonic()
            # 失败时不更新 last_scan：下一批事件到达即尽快重试

    def _rescan_until_ran(self, change_count: int) -> bool:
        """触发重扫并保证它真正执行；返回是否执行成功。"""
        deadline = time.monotonic() + _WAIT_IDLE_TIMEOUT_S
        while not self._stop_event.is_set() and time.monotonic() < deadline:
            if self.library.scanning:
                # 有扫描在跑（启动全量扫描等）：scan_blocking 会静默跳过，
                # 等它结束再触发
                if self._stop_event.wait(0.2):
                    return False
                continue
            if change_count > 0:
                logger.info("书库变化（%d 项），触发增量重扫", change_count)
            try:
                ran = bool(self._on_change())
            except Exception as exc:  # noqa: BLE001 - 单次失败退避后重试
                logger.warning("书库重扫失败: %s", exc)
                ran = False
            if ran:
                return True
            # TOCTOU 窗口：scanning 刚转 False 但 _scan_lock 尚未释放。
            # 必须重试而不是放弃：watchfiles 不会重新投递这批事件
            if self._stop_event.wait(0.2):
                return False
        if not self._stop_event.is_set():
            logger.warning(
                "书库重扫在 %.0fs 内未获得执行机会，等待下一批事件重试",
                _WAIT_IDLE_TIMEOUT_S,
            )
        return False
