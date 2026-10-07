"""ISO 光盘镜像容器（M5 Phase 2）：pycdlib 只读浏览（页面 + 压缩包内容浏览）。"""

from __future__ import annotations

import io
import posixpath
from pathlib import Path
from typing import List, Tuple

from .base import AdapterEnv, ParseError, UnsupportedFormatError
from .comics import ComicContainer


class IsoComicContainer(ComicContainer):
    browsable = True

    def __init__(self, env: AdapterEnv, path: Path) -> None:
        super().__init__(env)
        try:
            import pycdlib  # noqa: PLC0415
        except Exception as exc:  # pragma: no cover - 依赖缺失
            raise UnsupportedFormatError(
                "ISO 支持需要 pycdlib，请执行 pip install pycdlib", reason="缺少 pycdlib"
            ) from exc
        self._iso = pycdlib.PyCdlib()
        try:
            self._iso.open(str(path))
        except Exception as exc:
            raise ParseError(f"ISO 无法打开: {exc}") from exc
        raw: List[Tuple[str, int, bool]] = []
        try:
            self._walk("/", raw, depth=0)
        except Exception as exc:
            self._iso.close()
            raise ParseError(f"ISO 目录读取失败: {exc}") from exc
        self._raw_entries = raw
        self._finish_init()

    def _walk(self, dir_path: str, raw: List[Tuple[str, int, bool]], depth: int) -> None:
        if depth > 24:
            return
        try:
            children = list(self._iso.list_children(iso_path=dir_path))
        except Exception:
            return
        for child, name, length in children:
            nm = _decode_name(name)
            if not nm or nm in (".", ".."):
                continue
            is_dir = bool(getattr(child, "is_dir", lambda: False)())
            if is_dir:
                full = posixpath.normpath(posixpath.join(dir_path, nm))
                raw.append((full.lstrip("/"), 0, True))
                self._walk(dir_path.rstrip("/") + "/" + nm, raw, depth + 1)
            else:
                base = nm.split(";", 1)[0]
                full = posixpath.normpath(posixpath.join(dir_path, base))
                raw.append((full.lstrip("/"), int(length or 0), False))

    @property
    def names(self) -> List[str]:
        return self._names

    def _read_by_real(self, real: str, key: str) -> bytes:
        out = io.BytesIO()
        candidate_paths = [f"/{real}", real]
        for iso_path in candidate_paths:
            try:
                self._iso.get_file_from_iso_fp(out, iso_path=iso_path)
                data = out.getvalue()
                return self._check_size(key, data)
            except Exception:
                out = io.BytesIO()
                continue
        raise ParseError(f"ISO 条目读取失败: {key}")

    def close(self) -> None:
        try:
            self._iso.close()
        except Exception:
            pass


def _decode_name(name) -> str:
    if isinstance(name, (bytes, bytearray)):
        try:
            return bytes(name).decode("utf-8")
        except UnicodeDecodeError:
            return bytes(name).decode("latin-1", errors="replace")
    return str(name or "")
