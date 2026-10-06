"""压缩包内条目作为书 / 前缀图集的适配机制（设计 7.6）。

职责（只依赖 formats 层与配置，不依赖 services，避免循环导入）：

- **链式容器**：``/书库/a.zip`` 里还有 ``inner.zip``，再里面才是书。解析嵌套书时
  从顶层真实压缩包出发，把链上每一层压缩包条目**解压到缓存**后继续打开，最终
  得到直接容纳目标条目的容器；
- **解压缓存**：``cache/extracted/<nested_id>/<安全化文件名>``，按嵌套书 id
  内容寻址。条目存在且尺寸一致时直接复用；写入走 tmp + ``os.replace`` 原子替换；
- **前缀容器**：包内目录 → 图集（该前缀下的图片条目即页）、单张图片 → 单页书，
  都是套在真实容器外面的过滤视图，不落盘；
- **适配器工厂**：``registry.create_adapter`` 检测到 ``ref.extra["archive_entry"]``
  时委托到这里：book 类条目解压后走常规适配器链，image/dir 类套前缀容器。

嵌套书 id / 链的推导在 ``services.archive``（需要访问书库与注册表）；本模块只
消费 ``ref.extra`` 里已解析好的 ``chain`` / ``chain_ids`` / ``root_path``。
"""

from __future__ import annotations

import hashlib
import os
import posixpath
import re
import time
from pathlib import Path
from typing import Dict, List, Sequence, Tuple

from ..util.natural import natural_key
from ..util.paths import is_safe_id
from .base import AdapterEnv, BookAdapter, BookRef, ParseError
from .comics import (
    ComicAdapter,
    ComicContainer,
    is_image_name,
    open_container,
)

# 解压文件名长度上限（留出路径余量）
_NAME_MAX = 120

_WINDOWS_RESERVED = {
    "con",
    "prn",
    "aux",
    "nul",
    *(f"com{i}" for i in range(1, 10)),
    *(f"lpt{i}" for i in range(1, 10)),
}

_ILLEGAL_FS_CHARS = re.compile(r'[<>:"/\\|?*\x00-\x1f]')


def nested_book_id(parent_book_id: str, entry: str) -> str:
    """嵌套书 id = sha1("arc:<父id>:<规范化条目路径>")[:16]（设计 7.6）。

    由父 id 与条目路径确定性推导：父压缩包内容不变则 id 稳定（进度保留）；
    父内容变化 → 旧父 id 失效 → 嵌套 id 无法解析（与"文件被替换"同语义）。
    """
    raw = f"arc:{parent_book_id}:{posixpath.normpath(str(entry).replace(chr(92), '/').strip('/'))}"
    return hashlib.sha1(raw.encode("utf-8")).hexdigest()[:16]


def nested_revision(nested_id: str, parent_revision: str) -> str:
    """嵌套书修订号：父压缩包 rev 变化 → 全部嵌套书派生缓存失效。"""
    return hashlib.sha1(f"{nested_id}:{parent_revision}".encode("utf-8")).hexdigest()[:12]


def sanitize_fs_name(name: str) -> str:
    """条目名 → 安全的缓存文件名（Windows 保留名 / 非法字符 / 长度）。"""
    base = posixpath.basename(str(name).replace("\\", "/").rstrip("/"))
    base = base.strip().rstrip(".") or "file"
    base = _ILLEGAL_FS_CHARS.sub("_", base)
    if base.split(".")[0].lower() in _WINDOWS_RESERVED:
        base = "_" + base
    base = base[:_NAME_MAX] or "file"
    return base


def extract_entry_to_cache(
    env: AdapterEnv, container: ComicContainer, entry: str, nested_id: str
) -> Path:
    """把压缩包条目解压到缓存并返回本地路径（存在且尺寸一致则复用）。

    单条目尺寸上限由 ``container.read_entry`` 强制（max_archive_entry_mb）；
    嵌套 id 必须通过 ``is_safe_id``（它会被拼进缓存路径）。
    """
    if not is_safe_id(nested_id):
        raise ParseError(f"非法的嵌套书 id: {nested_id}")
    target_dir = env.config.extract_cache_dir / nested_id
    target = target_dir / sanitize_fs_name(posixpath.basename(entry))
    expected = int(container.entry_size(entry) or 0)
    try:
        if target.is_file() and (not expected or target.stat().st_size == expected):
            return target
    except OSError:
        pass
    data = container.read_entry(entry)
    try:
        target_dir.mkdir(parents=True, exist_ok=True)
        tmp = target.with_suffix(f".tmp{os.getpid()}.{time.time_ns()}")
        tmp.write_bytes(data)
        os.replace(tmp, target)
    except OSError as exc:
        raise ParseError(f"压缩包条目解压失败 {entry}: {exc}") from exc
    return target


def open_chain_container(
    env: AdapterEnv,
    root_path: Path,
    entries: Sequence[str],
    ids: Sequence[str],
) -> ComicContainer:
    """从顶层真实压缩包沿嵌套链下钻，返回**最内层**压缩包的容器。

    ``entries[i]`` 是 ``entries[i-1]`` 压缩包内的一个压缩包条目（``ids[i]`` 是
    它的嵌套书 id，用作解压缓存键）。中间层容器在解压完成、子容器打开后立即
    关闭（句柄不叠加）；返回的容器由调用方负责 close。
    """
    container = open_container(env, Path(root_path))
    for entry, nid in zip(entries, ids):
        if container.entry_size(entry) == 0 and not _entry_exists(container, entry):
            container.close()
            raise ParseError(f"压缩包内不存在条目: {entry}")
        try:
            extracted = extract_entry_to_cache(env, container, entry, str(nid))
            child = open_container(env, extracted)
        except Exception:
            container.close()
            raise
        # 中间层容器用完即关：子容器已自持句柄，留着只会叠加文件句柄
        container.close()
        container = child
    return container


def _entry_exists(container: ComicContainer, entry: str) -> bool:
    key = posixpath.normpath(str(entry).replace("\\", "/"))
    try:
        names = {e.name for e in container.browse()}
    except Exception:
        return False
    return key in names


class PrefixedArchiveContainer(ComicContainer):
    """压缩包内条目的前缀/单条目过滤视图（不落盘）。

    - ``exact``：单张图片 → 单页书（与文件夹"单图即书"一致）；
    - ``prefix``：目录 → 图集，该前缀**直接子级**的图片条目即页，自然排序。
    """

    browsable = False

    def __init__(self, env: AdapterEnv, inner: ComicContainer, *, prefix: str = "", exact: str = "") -> None:
        super().__init__(env)
        self._inner = inner
        self._prefix = str(prefix or "").strip("/")
        self._exact = posixpath.normpath(str(exact or "").replace("\\", "/")) if exact else ""
        names: List[str] = []
        mapping: Dict[str, str] = {}
        sizes: Dict[str, int] = {}
        try:
            entries = inner.browse()
        except Exception:
            entries = []
        if self._exact:
            target = next(
                (e for e in entries if e.name == self._exact and not e.is_dir), None
            )
            if target is not None and is_image_name(target.name):
                rel = posixpath.basename(self._exact)
                names.append(rel)
                mapping[rel] = self._exact
                sizes[rel] = int(target.size or 0)
        else:
            pref = self._prefix + "/" if self._prefix else ""
            for e in entries:
                if e.is_dir or not e.name.startswith(pref):
                    continue
                rel = e.name[len(pref) :]
                if not rel or "/" in rel:
                    continue
                if not is_image_name(rel):
                    continue
                names.append(rel)
                mapping[rel] = e.name
                sizes[rel] = int(e.size or 0)
        self._map = mapping
        self._sizes = sizes
        self._names = sorted(names, key=natural_key)

    @property
    def names(self) -> List[str]:
        return self._names

    def read(self, name: str) -> bytes:
        key = posixpath.normpath(name)
        full = self._map.get(key)
        if full is None:
            raise ParseError(f"压缩包内不存在条目: {name}")
        return self._inner.read_entry(full)

    def size_of(self, name: str) -> int:
        return self._sizes.get(posixpath.normpath(name), 0)

    def _read_by_real(self, real: str, key: str) -> bytes:
        return self.read(key)

    def close(self) -> None:
        try:
            self._inner.close()
        except Exception:
            pass


def create_archive_adapter(env: AdapterEnv, ref: BookRef) -> BookAdapter:
    """嵌套书引用 → 适配器（由 ``registry.create_adapter`` 委托调用）。"""
    extra = ref.extra or {}
    chain = [str(x) for x in (extra.get("chain") or [])]
    chain_ids = [str(x) for x in (extra.get("chain_ids") or [])]
    kind = str(extra.get("kind") or "book")
    if not chain or len(chain_ids) != len(chain):
        raise ParseError(f"嵌套书引用缺少链信息: {ref.book_id}")
    root_path = Path(str(extra.get("root_path") or ref.path))
    title = str(extra.get("title") or "") or ref.title_guess

    if kind in ("image", "dir"):
        holder = open_chain_container(env, root_path, chain[:-1], chain_ids[:-1])
        try:
            if kind == "image":
                container: ComicContainer = PrefixedArchiveContainer(
                    env, holder, exact=chain[-1]
                )
            else:
                container = PrefixedArchiveContainer(
                    env, holder, prefix=chain[-1]
                )
        except Exception:
            holder.close()
            raise
        return ComicAdapter(env, ref, container=container, title=title)

    # kind == "book"：解压到缓存后走常规适配器链（格式由扩展名决定）
    holder = open_chain_container(env, root_path, chain[:-1], chain_ids[:-1])
    try:
        extracted = extract_entry_to_cache(env, holder, chain[-1], ref.book_id)
    finally:
        if getattr(holder, "browsable", False):
            holder.close()
    ext = posixpath.splitext(chain[-1])[1].lower()
    try:
        size = int(extracted.stat().st_size)
        mtime = float(extracted.stat().st_mtime)
    except OSError:
        size, mtime = 0, 0.0
    synth = BookRef(
        book_id=ref.book_id,
        vpath="/" + chain[-1],
        path=extracted,
        format=ext.lstrip("."),
        size=size,
        mtime=mtime,
        virtual=False,
        revision=ref.revision,
    )
    from . import registry as registry_mod  # noqa: PLC0415 - 延迟导入避免循环

    return registry_mod.create_adapter(env, synth)


def sweep_extract_cache(config) -> int:
    """解压缓存总量超限时按访问时间淘汰（启动时调用一次，设计 7.6）。

    返回删除的目录数。目录粒度 = 嵌套书（一个 id 一个目录），整目录删除，
    避免半截缓存；正在使用的文件被删后下次访问会自动重新解压。
    """
    root = config.extract_cache_dir
    cap = int(getattr(config.archive, "extract_cache_mb", 0) or 0) * 1024 * 1024
    if not cap or not root.is_dir():
        return 0
    dirs: List[Tuple[float, int, Path]] = []  # (atime, size, path)
    total = 0
    try:
        children = [p for p in root.iterdir() if p.is_dir()]
    except OSError:
        return 0
    for d in children:
        size = 0
        atime = 0.0
        for f in d.rglob("*"):
            try:
                if f.is_file():
                    st = f.stat()
                    size += int(st.st_size)
                    atime = max(atime, float(st.st_atime))
            except OSError:
                continue
        dirs.append((atime, size, d))
        total += size
    if total <= cap:
        return 0
    removed = 0
    for atime, _size, d in sorted(dirs, key=lambda item: item[0]):
        if total <= cap:
            break
        total -= _size
        _rmtree_quiet(d)
        removed += 1
    return removed


def _rmtree_quiet(path: Path) -> None:
    import shutil  # noqa: PLC0415

    try:
        shutil.rmtree(path, ignore_errors=True)
    except Exception:
        pass
