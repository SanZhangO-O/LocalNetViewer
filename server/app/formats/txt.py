"""TXT 适配器：编码检测 + 章节切分（设计 7.2）。

- 编码：BOM 优先 → charset-normalizer → GB18030 兜底，支持配置书级覆盖；
- 章节：行匹配正则（行长受限避免误判）；未识别出章节且文件 > 512KB 时按约
  64KB 切分为"第 N 节"，保证目录可用；
- meta 阶段用轻量扫描（只匹配标题行 + 统计字数），document() 才构建块模型。
"""

from __future__ import annotations

from typing import Dict, List, Optional, Tuple

from ..util import textutil
from ..util.paths import stem_of
from .base import AdapterEnv, AdapterMeta, Block, BookRef, Chapter, DocumentModel, FlowAdapter, ParseError

SECTION_CHARS = 64 * 1024
NO_CHAPTER_SPLIT_THRESHOLD = 512 * 1024


def _should_section(byte_len: int, total_chars: int) -> bool:
    """是否需要把"没有章节的整本"按 64KB 切节（设计 7.2）。

    meta 与 document 必须用**同一个判据**，否则目录（meta）与实际分页
    （document 的"第 N 节"）会给出不同的章节数。
    """
    return byte_len > NO_CHAPTER_SPLIT_THRESHOLD and total_chars > SECTION_CHARS


class TxtAdapter(FlowAdapter):
    format = "txt"

    def __init__(self, env: AdapterEnv, ref: BookRef) -> None:
        super().__init__(env, ref)
        self._text: Optional[str] = None
        self._encoding: str = ""
        self._scan: Optional[List[Dict[str, object]]] = None

    # ------------------------------------------------------------ 读取
    def _load_text(self) -> Tuple[str, str]:
        if self._text is not None:
            return self._text, self._encoding
        max_mb = self.env.config.limits.max_book_mb
        size = self.ref.size or 0
        if max_mb and size > max_mb * 1024 * 1024:
            raise ParseError(f"文件超过上限 {max_mb}MB（{size / 1048576:.1f}MB）")
        try:
            data = self.ref.path.read_bytes()
        except OSError as exc:
            raise ParseError(f"文件无法读取: {exc}") from exc
        override = self.env.config.encoding_override(self.ref.vpath)
        text, enc = textutil.decode_text(data, override)
        self._text = text
        self._encoding = enc
        return text, enc

    @property
    def title_guess(self) -> str:
        return stem_of(self.ref.name)

    def _clean(self, raw: str) -> str:
        """行清理钩子：TXT 去掉段首缩进；CodeAdapter 覆写为保留缩进。"""
        return _clean_line(raw)

    # ------------------------------------------------------------ meta
    def _scan_chapters(self) -> List[Dict[str, object]]:
        """轻量扫描：只产出章标题与字数，不构建 Block。"""
        if self._scan is not None:
            return self._scan
        text, _ = self._load_text()
        chapters: List[Dict[str, object]] = []
        cur_title: Optional[str] = None
        cur_chars = 0
        heading_seen = False

        def flush() -> None:
            nonlocal cur_chars
            # 首个标题之前若没有正文，不要产生一个空章节
            if cur_title is None and cur_chars == 0:
                return
            chapters.append(
                {
                    "index": len(chapters),
                    "title": cur_title or self.title_guess,
                    "total_chars": cur_chars,
                }
            )
            cur_chars = 0

        for raw in textutil.split_lines(text):
            line = self._clean(raw)
            if not line:
                continue
            title = textutil.match_chapter_title(line)
            if title:
                flush()
                heading_seen = True
                cur_title = title
                continue
            cur_chars += len(line)

        flush()
        if not chapters:
            chapters.append({"index": 0, "title": self.title_guess, "total_chars": cur_chars})

        if not heading_seen:
            total = int(chapters[0]["total_chars"] or 0) if chapters else 0
            byte_len = len(text.encode("utf-8", errors="ignore"))
            if _should_section(byte_len, total):
                # 与 document() 的 _split_chapter_into_sections 同规则：
                # 按整行字符数累计到 64KB 起一节（block 文本 == 行文本）
                sections: List[Dict[str, object]] = []
                cur_chars = 0
                for raw in textutil.split_lines(text):
                    line = self._clean(raw)
                    if not line:
                        continue
                    cur_chars += len(line)
                    if cur_chars >= SECTION_CHARS:
                        sections.append(
                            {
                                "index": len(sections),
                                "title": f"第 {len(sections) + 1} 节",
                                "total_chars": cur_chars,
                            }
                        )
                        cur_chars = 0
                if cur_chars or not sections:
                    sections.append(
                        {
                            "index": len(sections),
                            "title": f"第 {len(sections) + 1} 节",
                            "total_chars": cur_chars,
                        }
                    )
                chapters = sections
        self._scan = chapters
        return chapters

    def meta(self) -> AdapterMeta:
        chapters = self._scan_chapters()
        text, enc = self._load_text()
        language = "zh" if textutil.is_cjk(text) else None
        return AdapterMeta(
            title=self.title_guess,
            format=self.format,
            mode="flow",
            author=None,
            language=language,
            size=self.ref.size or len(text.encode("utf-8", errors="ignore")),
            page_count=None,
            chapters=chapters,
            supports_json=True,
        )

    # ------------------------------------------------------------ 解析
    def document(self) -> DocumentModel:
        text, enc = self._load_text()
        chapters = self._build_chapters(text)
        return DocumentModel(
            book_id=self.ref.book_id,
            format=self.format,
            meta={
                "title": self.title_guess,
                "author": None,
                "language": "zh" if textutil.is_cjk(text) else None,
                "encoding": enc,
                "total_chars": sum(c.total_chars for c in chapters),
                "source": self.ref.vpath,
            },
            chapters=chapters,
        )

    def _build_chapters(self, text: str) -> List[Chapter]:
        chapters: List[Chapter] = []
        blocks: List[Block] = []
        cur_title: Optional[str] = None
        heading_seen = False

        def flush() -> None:
            # cur_title 只读不写，不需要 nonlocal（写了 pyflakes 会提示无用）
            nonlocal blocks
            # 首个标题之前若没有正文，不要产生一个空章节
            if not blocks and cur_title is None:
                return
            chapters.append(
                Chapter(index=len(chapters), title=cur_title or self.title_guess, blocks=blocks)
            )
            blocks = []

        for raw in textutil.split_lines(text):
            line = self._clean(raw)
            if not line:
                continue
            title = textutil.match_chapter_title(line)
            if title:
                flush()
                heading_seen = True
                cur_title = title
                blocks.append(Block.heading(title, 1))
                continue
            blocks.append(Block.paragraph(line))
        flush()

        if not chapters:
            chapters = [Chapter(index=0, title=self.title_guess, blocks=[])]

        if not heading_seen and len(chapters) == 1:
            byte_len = len(text.encode("utf-8", errors="ignore"))
            if _should_section(byte_len, chapters[0].total_chars):
                chapters = _split_chapter_into_sections(chapters[0])
        # 重新编号，保证 index 连续
        for i, ch in enumerate(chapters):
            ch.index = i
        return chapters

    # ------------------------------------------------------------ 封面
    def cover(self, width: int = 0) -> Optional[Tuple[bytes, str]]:
        from ..render.raster import text_cover

        try:
            meta = self.meta()
        except Exception:
            meta = AdapterMeta(title=self.title_guess, format=self.format)
        return text_cover(meta.title, meta.author, width=width or 320, fonts=self.env.fonts)


def _clean_line(raw: str) -> str:
    """去掉行首缩进与行尾空白。

    母本 TXT 常用全角空格做段首缩进；若原样保留，会与排版参数的
    ``first_line_indent_chars`` 叠加成"4 个全角空格"，所以这里统一剥掉，
    由排版层给出唯一的段首缩进（设计 4.6）。
    """
    line = raw.replace("\t", "    ").rstrip()
    return line.lstrip("\u3000 \t")


class CodeAdapter(TxtAdapter):
    """源码 / 结构化文本：保留行首缩进（M5）。

    TXT 的段首去缩进会毁掉代码可读性；这里只去掉行尾空白与制表符展开，
    其余原样保留。format 没有规格时回退 ``code``。
    """

    def __init__(self, env, ref) -> None:
        super().__init__(env, ref)
        self.format = (ref.format or "code").lower()

    def _clean(self, raw: str) -> str:
        return raw.replace("\t", "    ").rstrip()


def _split_chapter_into_sections(chapter: Chapter) -> List[Chapter]:
    """按约 64KB 把单章切成"第 N 节"（设计 7.2）。"""
    sections: List[Chapter] = []
    cur: List[Block] = []
    cur_chars = 0
    for block in chapter.blocks:
        cur.append(block)
        cur_chars += len(block.text)
        if cur_chars >= SECTION_CHARS:
            sections.append(
                Chapter(index=len(sections), title=f"第 {len(sections) + 1} 节", blocks=cur)
            )
            cur = []
            cur_chars = 0
    if cur or not sections:
        sections.append(Chapter(index=len(sections), title=f"第 {len(sections) + 1} 节", blocks=cur))
    if len(sections) == 1:
        # 未超阈值时不改名，保留书名作为唯一章节标题
        sections[0].title = chapter.title
    return sections
