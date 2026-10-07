"""配置加载与校验（设计文档 4.2）。

设计约定：
- 单文件 ``config.yaml``；所有相对路径相对该文件所在目录解析；
- 校验失败直接抛 ``ConfigError``，启动阶段快速失败。
"""

from __future__ import annotations

import logging
import os
from pathlib import Path
from typing import Any, Dict, List, Literal, Optional

import yaml
from pydantic import BaseModel, Field, field_validator, model_validator

from .formats.specs import default_include_exts

logger = logging.getLogger("localnetviewer.config")

# 默认书库列表显示的扩展名：由 formats.specs 的 default_list 派生（M5）。
# 通用压缩包（zip/rar/7z/tar/cab/iso…）默认不列出，避免每个压缩包都算一本书。
DEFAULT_INCLUDE_EXT = default_include_exts()


class ConfigError(RuntimeError):
    """配置文件不存在或非法。"""


class ServerCfg(BaseModel):
    name: str = "LocalNetViewer"
    # 安全默认：只监听本机。要给手机用必须在 config.yaml / --host 显式写 0.0.0.0
    # （或具体网卡地址），此时 run.py 会在启动横幅里检查"未配令牌 + 对外监听"并告警。
    host: str = "127.0.0.1"
    port: int = Field(default=8765, ge=1, le=65535)
    auth_token: str = ""


class LibraryRootCfg(BaseModel):
    name: str
    path: str
    # 访问密码：留空 = 公开书库；填写后客户端需携带对应 X-Auth-Token 才能看到该书库。
    # 多个书库可共用同一密码（输入该密码即可同时看到它们）。
    password: str = ""

    @field_validator("path")
    @classmethod
    def _expand(cls, v: str) -> str:
        return os.path.expandvars(os.path.expanduser(v.strip()))

    @field_validator("password", mode="before")
    @classmethod
    def _coerce_password(cls, v: object) -> object:
        # YAML 未加引号的数字密码（password: 123456）按字符串收下；
        # 显式 null 原样透传，由 str 类型校验按配置错误拒绝。
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            return str(v)
        return v

    @field_validator("password")
    @classmethod
    def _strip_password(cls, v: str) -> str:
        return (v or "").strip()


class LibraryCfg(BaseModel):
    roots: List[LibraryRootCfg] = Field(default_factory=list)
    include_ext: List[str] = Field(default_factory=lambda: list(DEFAULT_INCLUDE_EXT))
    show_all_files: bool = False
    sort: Literal["natural", "name"] = "natural"
    # 扫描模式：lazy=不做启动全量扫描，用户列出哪个目录就只扫描/指纹该目录；
    # eager=启动时全量扫描一次（旧行为）。默认 lazy（设计 4.8）。
    scan_mode: Literal["lazy", "eager"] = "lazy"
    # 目录变化监听（设计 4.8）：OS 原生通知，事件在防抖窗口内合并为一次增量重扫。
    # 需可选依赖 watchfiles（pip install -e ".[watch]"）；未安装或目录不可监听时
    # 自动降级为"仅启动扫描"，不影响服务。lazy 模式下不启用（无全量索引可增量重扫）。
    watch: bool = True
    watch_debounce_s: float = Field(default=2.0, ge=0.1, le=60.0)
    # 书级编码覆盖：虚拟路径 / 文件名 / 纯文件名 → 编码名
    encoding_overrides: Dict[str, str] = Field(default_factory=dict)
    # 未知扩展名内容嗅探（M5）：ext 不在 include_ext 时读文件头判断
    # 文本/图片/文档容器，能读的就作为书列出（关掉可让列表只显示已知格式）
    sniff_unknown: bool = True
    # 扩展名 → 格式 id 覆盖（M5），如 {".mybook": "epub"}；
    # 用于给私有/错扩展名的文件手动指定解析器
    ext_overrides: Dict[str, str] = Field(default_factory=dict)

    @field_validator("include_ext")
    @classmethod
    def _lower_ext(cls, v: List[str]) -> List[str]:
        out: List[str] = []
        for e in v:
            e = e.strip().lower()
            if not e:
                continue
            if not e.startswith("."):
                e = "." + e
            if e not in out:
                out.append(e)
        return out


class GalleryCfg(BaseModel):
    folder_as_book: bool = True
    min_images_in_folder: int = Field(default=1, ge=0)
    recursive: bool = False
    # 目录列表直接列出单张图片文件：点按即按"单页图集"打开（设计 7.5）。
    # 服务器与客户端的打开链路（register → FolderComicContainer 单文件模式 →
    # 阅读器 fixed 模式）早已支持，这里只决定图片是否出现在列表里。
    show_image_files: bool = True


class ComicCfg(BaseModel):
    skip_entries: List[str] = Field(
        default_factory=lambda: ["__MACOSX/*", "*/.*", "Thumbs.db", "desktop.ini"]
    )
    default_fit: Literal["viewport", "original"] = "viewport"
    jpeg_quality: int = Field(default=85, ge=1, le=100)
    strip_min_aspect: float = Field(default=1.5, gt=0)
    strip_overlap_px: int = Field(default=0, ge=0)
    # 条漫（strip）切片需要探测每一张源图的原始高度：图集含上万张图时，单次
    # /layout 会变成数万次读图（实测 1.5 万张约 18s），客户端会等到超时。
    # 源图数超过该值时自动回退为 fixed（一图一页）—— 只枚举页集、不读图片，
    # 设备端只请求"当前页 + 预取页"。0 = 不限制（始终允许 strip）。
    strip_max_source_images: int = Field(default=1000, ge=0)


class RenderCfg(BaseModel):
    fonts_dir: str = "assets/fonts"
    default_font: str = "NotoSansSC-Regular"
    max_page_pixels: int = Field(default=4_000_000, ge=10_000)
    jpeg_for_photo_pages: bool = True


class CacheCfg(BaseModel):
    dir: str = "cache"
    parsed_max_mb: int = Field(default=4096, ge=0)
    page_lru_bytes: int = Field(default=512 * 1024 * 1024, ge=0)
    page_lru_pages: int = Field(default=512, ge=0)
    layout_memory_max: int = Field(default=32, ge=1)
    # 磁盘布局缓存上限（PageMap + LayoutProfile）；0 = 不限制
    layouts_max_mb: int = Field(default=2048, ge=0)
    # 内存中保留的解析结果（DocumentModel）数量：渲染页面需要它，太小会反复读盘
    doc_memory_max: int = Field(default=3, ge=1)


class ArchiveCfg(BaseModel):
    """压缩包内容浏览（设计 7.6）。

    - browse：允许把压缩包当虚拟目录浏览（/books/{id}/contents）；
    - max_depth：嵌套压缩包最多可下钻的层数（包中包）；
    - extract_cache_mb：包内条目解压缓存（cache/extracted）总量上限，
      按访问时间 LRU 淘汰；0 = 不限制。
    """

    browse: bool = True
    max_depth: int = Field(default=2, ge=1, le=8)
    extract_cache_mb: int = Field(default=512, ge=0)


class LimitsCfg(BaseModel):
    max_book_mb: int = Field(default=2048, ge=0)
    max_page_bytes: int = Field(default=50 * 1024 * 1024, ge=0)
    max_source_pixels: int = Field(default=30_000_000, ge=0)
    parse_timeout_s: float = Field(default=60.0, gt=0)
    max_archive_entry_mb: int = Field(default=512, ge=0)
    max_archive_total_mb: int = Field(default=4096, ge=0)


class AppConfig(BaseModel):
    """完整配置 + 解析后的绝对路径。"""

    server: ServerCfg = Field(default_factory=ServerCfg)
    library: LibraryCfg = Field(default_factory=LibraryCfg)
    gallery: GalleryCfg = Field(default_factory=GalleryCfg)
    comic: ComicCfg = Field(default_factory=ComicCfg)
    archive: ArchiveCfg = Field(default_factory=ArchiveCfg)
    render: RenderCfg = Field(default_factory=RenderCfg)
    cache: CacheCfg = Field(default_factory=CacheCfg)
    limits: LimitsCfg = Field(default_factory=LimitsCfg)

    # 运行期派生字段（不来自 yaml）
    config_path: Optional[Path] = None
    base_dir: Path = Field(default_factory=lambda: Path.cwd())

    model_config = {"arbitrary_types_allowed": True}

    # ---------------------------------------------------------------- 派生路径
    def _resolve(self, value: str) -> Path:
        p = Path(value)
        return p if p.is_absolute() else (self.base_dir / p)

    @property
    def cache_dir(self) -> Path:
        return self._resolve(self.cache.dir)

    @property
    def fonts_dir(self) -> Path:
        return self._resolve(self.render.fonts_dir)

    @property
    def parsed_cache_dir(self) -> Path:
        return self.cache_dir / "parsed"

    @property
    def layouts_cache_dir(self) -> Path:
        return self.cache_dir / "layouts"

    @property
    def index_path(self) -> Path:
        return self.cache_dir / "index.json"

    @property
    def gallery_registry_path(self) -> Path:
        return self.cache_dir / "galleries.json"

    @property
    def archive_registry_path(self) -> Path:
        return self.cache_dir / "archives.json"

    @property
    def extract_cache_dir(self) -> Path:
        return self.cache_dir / "extracted"

    @property
    def auth_required(self) -> bool:
        return bool(self.server.auth_token.strip())

    @property
    def public_root_names(self) -> set:
        """未设密码、任何人可见的书库名。"""
        return {r.name for r in self.library.roots if not (r.password or "").strip()}

    @property
    def credential_required(self) -> bool:
        """完全没有任何书库可见（管理员令牌启用，或全部书库都设了密码）时为真。"""
        if self.auth_required:
            return True
        return bool(self.library.roots) and not self.public_root_names

    @model_validator(mode="after")
    def _check_roots(self) -> "AppConfig":
        names = [r.name for r in self.library.roots]
        if len(set(names)) != len(names):
            raise ValueError("library.roots 存在重名书库（虚拟路径第一段必须唯一）")
        for r in self.library.roots:
            if "/" in r.name or "\\" in r.name:
                raise ValueError(f"书库名不能包含路径分隔符: {r.name!r}")
        return self

    def root_by_name(self, name: str) -> Optional[LibraryRootCfg]:
        for r in self.library.roots:
            if r.name == name:
                return r
        return None

    def encoding_override(self, vpath: str) -> Optional[str]:
        """按虚拟路径 / 文件名查编码覆盖（设计 7.2）。"""
        ov = self.library.encoding_overrides or {}
        if not ov:
            return None
        if vpath in ov:
            return ov[vpath]
        name = vpath.rsplit("/", 1)[-1]
        if name in ov:
            return ov[name]
        for key, enc in ov.items():
            if key and (vpath.endswith(key) or name == key):
                return enc
        return None


def _read_yaml(path: Path) -> Dict[str, Any]:
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError as exc:
        raise ConfigError(f"配置文件不存在: {path}") from exc
    except OSError as exc:
        raise ConfigError(f"配置文件无法读取: {path} ({exc})") from exc
    try:
        data = yaml.safe_load(text)
    except yaml.YAMLError as exc:
        raise ConfigError(f"配置文件 YAML 解析失败: {path}\n{exc}") from exc
    if data is None:
        return {}
    if not isinstance(data, dict):
        raise ConfigError(f"配置文件顶层必须是映射: {path}")
    return data


def default_config_path() -> Path:
    """未显式指定 --config 时的默认位置：server/config.yaml。"""
    return Path(__file__).resolve().parent.parent / "config.yaml"


def load_config(path: Optional[str | Path] = None) -> AppConfig:
    """加载并校验配置。

    ``path`` 为 None 时使用 ``server/config.yaml``；文件不存在则返回内置默认值
    （无书库，仅用于让服务器能起来并打印提示）。
    """
    cfg_path = Path(path).expanduser().resolve() if path else default_config_path()
    if not cfg_path.exists():
        cfg = AppConfig()
        cfg.base_dir = cfg_path.parent
        cfg.config_path = None
        return cfg

    data = _read_yaml(cfg_path)
    try:
        cfg = AppConfig.model_validate(data)
    except Exception as exc:  # pydantic ValidationError
        raise ConfigError(f"配置校验失败: {cfg_path}\n{exc}") from exc
    # M5：include_ext 一旦显式配置就是权威过滤，新格式默认不可见。旧配置
    # （0.7 的 10 项清单）升级后会"新功能好像没生效"，这里明确提示一次。
    library_raw = data.get("library") if isinstance(data, dict) else None
    if isinstance(library_raw, dict) and library_raw.get("include_ext"):
        logger.info(
            "library.include_ext 已显式配置：只列出清单内的扩展名。"
            "要启用 0.8 新增格式（Office/Markdown/源码/媒体等），"
            "请删除该键（使用内置默认全集）或把需要的扩展名加进去。"
        )
    cfg.base_dir = cfg_path.parent
    cfg.config_path = cfg_path
    return cfg


def ensure_dirs(cfg: AppConfig) -> None:
    """创建运行期需要的目录。"""
    for d in (cfg.cache_dir, cfg.parsed_cache_dir, cfg.layouts_cache_dir, cfg.fonts_dir):
        try:
            d.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            # 只读介质 / 权限问题不应阻止服务器启动，缓存层会自行降级，
            # 但必须留一条日志，否则用户只会看到"缓存不生效"而无线索
            logger.warning("目录无法创建（缓存/字体可能不可用）: %s (%s)", d, exc)
