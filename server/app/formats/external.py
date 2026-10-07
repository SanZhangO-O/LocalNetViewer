"""CAB / LZH / LHA 容器（M5 Phase 4）：借用系统 libarchive（bsdtar）。

- 列表：``bsdtar -tf <file>``；读取：``bsdtar -xOf <file> -- <entry>``；
- Windows 10+ 自带的 ``tar.exe`` 就是 bsdtar（探测时校验 --version 输出）；
- 单条目读取按 ``limits.max_archive_entry_mb`` 流式限流（Popen 读取后即杀进程）；
- ACE 有名无实：libarchive 不支持 ACE，列表阶段会明确报错（保持置灰/失败可见）。
"""

from __future__ import annotations

import shutil
import subprocess
from functools import lru_cache
from pathlib import Path
from typing import List, Optional, Tuple

from .base import AdapterEnv, ParseError
from .comics import ComicContainer


@lru_cache(maxsize=1)
def bsdtar_tool() -> Optional[str]:
    """bsdtar/libarchive 可执行文件（进程级缓存；registry 的后端探测复用同一结果）。"""
    exe = shutil.which("bsdtar") or shutil.which("bsdtar.exe")
    if exe:
        return exe
    tar = shutil.which("tar")
    if not tar:
        return None
    try:
        proc = subprocess.run([tar, "--version"], capture_output=True, timeout=5)
        out = (proc.stdout or b"") + (proc.stderr or b"")
        if b"bsdtar" in out or b"libarchive" in out:
            return tar
    except Exception:
        return None
    return None


class BsdtarComicContainer(ComicContainer):
    browsable = True

    def __init__(self, env: AdapterEnv, path: Path) -> None:
        super().__init__(env)
        tool = bsdtar_tool()
        if not tool:
            raise ParseError("需要 bsdtar（libarchive）才能打开该压缩包")
        self._tool = tool
        self._path = str(path)
        self._timeout = max(30.0, float(env.config.limits.parse_timeout_s) * 2)
        raw: List[Tuple[str, int, bool]] = []
        try:
            proc = subprocess.run(
                [tool, "-tf", self._path],
                capture_output=True,
                timeout=self._timeout,
            )
        except subprocess.TimeoutExpired as exc:
            raise ParseError("压缩包列表超时") from exc
        except OSError as exc:
            raise ParseError(f"bsdtar 无法执行: {exc}") from exc
        if proc.returncode != 0:
            detail = (proc.stderr or b"").decode("utf-8", errors="replace").strip()[-300:]
            raise ParseError(f"压缩包无法打开: {detail or 'bsdtar 返回非零退出码'}")
        text = _decode_names(proc.stdout or b"")
        for line in text.splitlines():
            name = line.replace("\\", "/").strip()
            if not name:
                continue
            is_dir = name.endswith("/")
            raw.append((name, 0, is_dir))
        self._raw_entries = raw
        self._finish_init()

    @property
    def names(self) -> List[str]:
        return self._names

    def _read_by_real(self, real: str, key: str) -> bytes:
        limit = self.max_entry_bytes
        args = [self._tool, "-xOf", self._path, "--", real]
        data = self._popen_read(args, limit)
        if data is None:
            data = self._popen_read([self._tool, "-xOf", self._path, real], limit)
        if data is None:
            raise ParseError(f"压缩包条目读取失败: {key}")
        return self._check_size(key, data)

    def _popen_read(self, args: List[str], limit: int) -> Optional[bytes]:
        try:
            proc = subprocess.Popen(
                args,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except OSError:
            return None
        try:
            if limit:
                data = proc.stdout.read(limit + 1) if proc.stdout else b""
                if len(data) > limit:
                    proc.kill()
                    proc.wait(timeout=10)
                    raise ParseError(
                        f"压缩包条目解压后超过上限（{limit // 1048576}MB）"
                    )
            else:
                data = proc.stdout.read() if proc.stdout else b""
            proc.wait(timeout=self._timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            return None
        finally:
            try:
                if proc.stdout:
                    proc.stdout.close()
                if proc.stderr:
                    proc.stderr.close()
            except Exception:
                pass
        if proc.returncode != 0:
            return None
        return data

    def close(self) -> None:
        return None


def _decode_names(raw: bytes) -> str:
    for enc in ("utf-8", "gbk", "big5", "cp437"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", errors="replace")
