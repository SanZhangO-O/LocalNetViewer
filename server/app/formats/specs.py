"""格式能力注册表：全部支持类型的单一事实源（M5）。

本模块**不导入任何其它 app 模块**（config / registry / 适配器都不导入），
避免循环依赖：config 在这里取默认 include_ext，registry 在这里建扩展名索引，
``/server/formats`` 能力接口也由这里生成。

字段约定：

- ``kind``：text / doc / comic / archive / image / audio / video / font；
- ``adapter``：registry 的适配器工厂键（不是类名）；
- ``mode``：flow / fixed / ""（空 = 不可作为书打开，如音视频）；
- ``backend``：``registry.backend_status()`` 的键；为空 = 纯内置、总是可用；
- ``default_list``：是否出现在默认 ``library.include_ext``（通用压缩包默认不列，
  避免书库里每个 zip 都被当成一本书；需要时配置里显式加）。
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List, Tuple

TEXT_EXTS: Tuple[str, ...] = (".txt", ".text", ".log")

# 源码 / 结构化文本 / 字幕 / 数据文件：按纯文本阅读（保留行内空白）
CODE_EXTS: Tuple[str, ...] = (
    ".py", ".pyw", ".pyi",
    ".js", ".mjs", ".cjs", ".jsx", ".ts", ".tsx",
    ".java", ".kt", ".kts", ".scala", ".groovy", ".gradle",
    ".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", ".hxx",
    ".cs", ".go", ".rs", ".rb", ".rake", ".php", ".phtml",
    ".swift", ".m", ".mm", ".lua", ".pl", ".pm",
    ".sh", ".bash", ".zsh", ".fish", ".ps1", ".psm1", ".bat", ".cmd",
    ".sql", ".css", ".scss", ".sass", ".less", ".styl", ".vue", ".svelte",
    ".dart", ".r", ".jl", ".clj", ".cljs", ".cljc", ".ex", ".exs",
    ".erl", ".hrl", ".hs", ".lhs", ".ml", ".mli", ".fs", ".fsx", ".vb", ".vbs",
    ".asm", ".lisp", ".el", ".scm", ".tcl", ".awk", ".sed",
    ".json", ".jsonc", ".json5", ".yaml", ".yml", ".toml", ".ini", ".cfg",
    ".conf", ".properties", ".xml", ".xsd", ".xsl", ".xslt", ".plist", ".webmanifest",
    ".csv", ".tsv",
    ".srt", ".vtt", ".ass", ".ssa", ".sub", ".nfo", ".diz",
    ".tex", ".sty", ".cls", ".bib", ".org", ".rst", ".adoc",
    ".patch", ".diff",
)

MEDIA_AUDIO_EXTS: Tuple[str, ...] = (
    ".mp3", ".m4a", ".m4b", ".aac", ".flac", ".ogg", ".oga", ".opus", ".wav", ".wma",
)
MEDIA_VIDEO_EXTS: Tuple[str, ...] = (
    ".mp4", ".m4v", ".mkv", ".webm", ".mov", ".avi", ".3gp",
)

# 单图基础扩展名（可被 Pillow 直接解码，无需插件）。TIFF 单独成"多页"格式。
BASE_IMAGE_EXTS: Tuple[str, ...] = (
    ".jpg", ".jpeg", ".jfif", ".png", ".gif", ".bmp", ".webp", ".avif",
    ".tga", ".pcx", ".ico", ".dds", ".pnm", ".ppm", ".pgm", ".pbm", ".xbm", ".xpm",
)


@dataclass(frozen=True)
class FormatSpec:
    id: str
    label: str
    kind: str
    exts: Tuple[str, ...]
    adapter: str
    mode: str
    backend: str = ""
    default_list: bool = True


FORMAT_SPECS: Tuple[FormatSpec, ...] = (
    # ---------------------------------------------------------------- 文本
    FormatSpec("txt", "纯文本", "text", TEXT_EXTS, "txt", "flow"),
    FormatSpec("code", "源码/结构化文本", "text", CODE_EXTS, "code", "flow"),
    FormatSpec("html", "HTML 网页", "doc", (".html", ".htm", ".xhtml", ".xht"), "html", "flow"),
    FormatSpec("markdown", "Markdown", "doc", (".md", ".markdown", ".mdown", ".mkd"), "markdown", "flow"),
    FormatSpec("rtf", "RTF 富文本", "doc", (".rtf",), "rtf", "flow"),
    # ---------------------------------------------------------------- 文档
    FormatSpec("docx", "Word 文档", "doc", (".docx", ".docm"), "docx", "flow"),
    FormatSpec("odt", "OpenDocument 文本", "doc", (".odt", ".fodt"), "odt", "flow"),
    FormatSpec("pptx", "PowerPoint 演示", "doc", (".pptx", ".pptm"), "pptx", "flow"),
    FormatSpec("xlsx", "Excel 表格", "doc", (".xlsx", ".xlsm"), "xlsx", "flow"),
    FormatSpec("legacy_doc", "旧版 Word（转换）", "doc", (".doc", ".wps"), "office_legacy_doc", "flow", "libreoffice"),
    FormatSpec("legacy_xls", "旧版 Excel（转换）", "doc", (".xls",), "office_legacy_xls", "flow", "libreoffice"),
    FormatSpec("legacy_ppt", "旧版 PPT（转换）", "doc", (".ppt", ".pps"), "office_legacy_ppt", "flow", "libreoffice"),
    # ---------------------------------------------------------------- 电子书
    FormatSpec("epub", "EPUB", "doc", (".epub",), "epub", "flow", "epub"),
    FormatSpec("fb2", "FictionBook", "doc", (".fb2",), "fb2", "flow"),
    FormatSpec("fbz", "FictionBook（压缩）", "doc", (".fbz", ".fb2.zip"), "fbz", "flow"),
    FormatSpec("mobi", "MOBI / AZW3（Calibre 转换）", "doc", (".mobi", ".azw3", ".azw"), "mobi", "flow", "calibre"),
    FormatSpec(
        "calibre",
        "Calibre 支持（转换）",
        "doc",
        # 注意：不含 .rb —— 它已是 Ruby 源码（code 格式，更常见）；
        # Rocket eBook 用户可用 library.ext_overrides: {".rb": "calibre"} 指回来。
        (".chm", ".lit", ".pdb", ".prc", ".pml", ".snb", ".tcr", ".lrf", ".azw4", ".txtz", ".htmlz"),
        "mobi", "flow", "calibre",
    ),
    FormatSpec("pdf", "PDF", "doc", (".pdf",), "pdf", "fixed", "pdf"),
    # ---------------------------------------------------------------- 漫画 / 压缩包
    FormatSpec("cbz", "CBZ 漫画", "comic", (".cbz", ".cbz3"), "comic", "fixed", "zip"),
    FormatSpec("zip", "ZIP 压缩包", "archive", (".zip",), "comic", "fixed", "zip", default_list=False),
    FormatSpec("cbr", "CBR 漫画", "comic", (".cbr",), "comic", "fixed", "rar"),
    FormatSpec("rar", "RAR 压缩包", "archive", (".rar",), "comic", "fixed", "rar", default_list=False),
    FormatSpec("cb7", "CB7 漫画", "comic", (".cb7",), "comic", "fixed", "7z"),
    FormatSpec("7z", "7z 压缩包", "archive", (".7z",), "comic", "fixed", "7z", default_list=False),    FormatSpec("cbt", "CBT 漫画", "comic", (".cbt",), "comic", "fixed", "tar"),
    FormatSpec(
        "tar",
        "TAR 压缩包",
        "archive",
        (".tar", ".tgz", ".tar.gz", ".tbz2", ".tar.bz2", ".txz", ".tar.xz"),
        "comic", "fixed", "tar", default_list=False,
    ),
    FormatSpec(
        "external",
        "CAB/LZH 压缩包",
        "archive",
        # 不含 .ace：libarchive（bsdtar）不支持 ACE，列成可打开会误导；
        # 有 ACE 工具的用户可用 ext_overrides 指到 "external" 自行承担结果。
        (".cab", ".lzh", ".lha"),
        "comic", "fixed", "bsdtar", default_list=False,
    ),
    FormatSpec("iso", "ISO 光盘镜像", "archive", (".iso",), "comic", "fixed", "iso", default_list=False),
    # ---------------------------------------------------------------- 固定版式
    FormatSpec("tiff", "TIFF 图像（多页）", "image", (".tif", ".tiff"), "multipage", "fixed"),
    FormatSpec("djvu", "DjVu 文档", "doc", (".djvu", ".djv"), "djvu", "fixed", "djvu"),
    FormatSpec("font", "字体（字样预览）", "font", (".ttf", ".otf", ".ttc"), "font", "fixed"),
    # ---------------------------------------------------------------- 媒体
    FormatSpec("audio", "音频", "audio", MEDIA_AUDIO_EXTS, "", "", default_list=True),
    FormatSpec("video", "视频", "video", MEDIA_VIDEO_EXTS, "", "", default_list=True),
)

SPEC_BY_ID: Dict[str, FormatSpec] = {spec.id: spec for spec in FORMAT_SPECS}


def default_include_exts() -> List[str]:
    """默认 ``library.include_ext``：全部 default_list 格式的扩展名（去重、排序）。"""
    seen: List[str] = []
    for spec in FORMAT_SPECS:
        if not spec.default_list:
            continue
        for ext in spec.exts:
            if ext not in seen:
                seen.append(ext)
    return seen


def all_exts() -> List[str]:
    return sorted({ext for spec in FORMAT_SPECS for ext in spec.exts})


def _validate() -> None:
    """程序员错误快速失败：扩展名不得跨格式重复（registry 索引会静默覆盖）。"""
    owners: Dict[str, str] = {}
    for spec in FORMAT_SPECS:
        if not spec.exts:
            raise ValueError(f"格式 {spec.id} 没有声明任何扩展名")
        if spec.mode not in ("", "flow", "fixed", "strip"):
            raise ValueError(f"格式 {spec.id} 的 mode 非法: {spec.mode!r}")
        for ext in spec.exts:
            if not ext.startswith(".") or ext != ext.lower():
                raise ValueError(f"格式 {spec.id} 的扩展名须为小写带点形式: {ext!r}")
            if ext in owners:
                raise ValueError(
                    f"扩展名 {ext} 同时声明于 {owners[ext]} 与 {spec.id}；"
                    "请合并到一种格式，或用 ext_overrides 让用户自行选择"
                )
            owners[ext] = spec.id


_validate()
