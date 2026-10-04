"""文本工具：段落/行处理、编码检测、章节切分正则。"""

from __future__ import annotations

import codecs
import hashlib
import re
from typing import Iterable, List, Optional, Sequence, Tuple


def stable_hash(value: object, length: int = 16) -> str:
    """跨进程稳定的短哈希。

    内置 ``hash()`` 对 str/bytes 会按 ``PYTHONHASHSEED`` 加盐，**每个进程都不同**：
    把它用在 ETag 上会让服务器重启后所有缓存校验失效，用在"按标题选配色"上会让
    同一本书每次重启换个封面颜色。凡是要跨进程一致的地方都必须用这个。
    """
    digest = hashlib.sha1(str(value).encode("utf-8")).hexdigest()
    return digest[: max(8, int(length))]


def content_revision(book_id: str, size: int, mtime: float) -> str:
    """内容修订号（跨实现契约，与 C++ 服务器一致）。

    ``sha1("<book_id>:<size>:<int(mtime)>")[:12]``，mtime 取**地板秒**：
    重启 / 跨进程稳定，供 ``revision`` 字段、页面/封面 ETag 与内存缓存键使用。
    图集虚拟书同样用它：size 传图片数、mtime 传目录 stat 的 st_mtime。
    """
    payload = f"{book_id}:{int(size)}:{int(float(mtime))}"
    return hashlib.sha1(payload.encode("ascii")).hexdigest()[:12]

# ---------------------------------------------------------------- 编码检测
# 注意 utf-16 / utf-32（不带 -le/-be 后缀）：codecs 会**消费掉 BOM**。
# 用 "utf-16-le" 解码 b"\xff\xfe..." 会把 U+FEFF 留在文本开头，后果是
#   1. 第一行 "第X章 …" 因为前缀 BOM 而匹配不上章节正则（目录少一章、错位）；
#   2. 页面上把 BOM 画成一个"未映射字形"的方框。
_BOMS: Sequence[Tuple[bytes, str]] = (
    (b"\xef\xbb\xbf", "utf-8-sig"),
    (b"\xff\xfe\x00\x00", "utf-32"),
    (b"\x00\x00\xfe\xff", "utf-32"),
    (b"\xff\xfe", "utf-16"),
    (b"\xfe\xff", "utf-16"),
)

FALLBACK_ENCODINGS = ("utf-8", "gb18030", "big5", "shift_jis", "cp1252")

# charset-normalizer 在短样本上会给多个候选相同 chaos，此时它的内部排序不代表
# "更可能正确"。实测的**具体**冲突是：短 GB18030 中文与 cp949（韩文）同为 chaos=0，
# 而 best() 会选中 cp949，把中文读成韩文乱码。
#
# 因此这里只做一件很窄的事：**当且仅当** chaos 打平的候选里同时出现"韩文编码"与
# "中文编码"时，选中文编码。
#
# 为什么不按全局偏好排序：那样会顺手把短日文（euc_jis_2004 / cp932 与 gb18030 同为
# chaos=0）判成中文 —— 用日文换中文不是这次修复的目的，实测 232 个样本里有 8 例
# 短日文会被读坏。
_KOREAN_CODECS = {"cp949", "euc_kr", "johab", "uhc", "ks_c_5601", "korean"}
_CHINESE_CODECS = {"gb18030", "gbk", "gb2312", "cp936", "big5", "big5hkscs", "cp950", "hz"}

# 同一编码族的别名：charset-normalizer 会报出 cp932 / euc_jis_2004 / big5hkscs
# 这类名字，直接按字符串比较会漏掉
_CODEC_FAMILY = {
    "cp932": "shift_jis",
    "shift_jis_2004": "shift_jis",
    "shift_jisx0213": "shift_jis",
    "sjis": "shift_jis",
    "euc_jis_2004": "euc_jp",
    "euc_jisx0213": "euc_jp",
    "eucjp": "euc_jp",
    "cp950": "big5",
    "big5hkscs": "big5",
    "cp936": "gbk",
    "mskanji": "shift_jis",
}


def detect_bom(data: bytes) -> Optional[str]:
    for bom, enc in _BOMS:
        if data.startswith(bom):
            return enc
    return None


def detect_encoding(data: bytes) -> str:
    """BOM 优先 → 无 BOM 的 UTF-16 → charset-normalizer → GB18030 兜底（设计 7.2）。"""
    bom = detect_bom(data)
    if bom:
        return bom
    utf16 = detect_bomless_utf16(data)
    if utf16:
        return utf16
    chosen = _normalizer_choice(data)
    if chosen:
        return chosen
    # 纯 ASCII 快速判定
    try:
        data.decode("ascii")
        return "ascii"
    except UnicodeDecodeError:
        pass
    return "gb18030"


def _codec_name(encoding: str) -> str:
    try:
        return codecs.lookup(encoding).name
    except (LookupError, TypeError, ValueError):
        return str(encoding or "").lower()


def _codec_family(encoding: str) -> str:
    """把别名归到同一编码族（cp932 → shift_jis、euc_jis_2004 → euc_jp …）。"""
    name = _codec_name(encoding)
    return _CODEC_FAMILY.get(name, name)


def _normalizer_choice(data: bytes) -> Optional[str]:
    """用 charset-normalizer 排名，只修掉"短中文被判成韩文"这一个具体冲突。

    短样本上 cp949（韩文）与 gb18030（中文）可能同为 ``chaos=0``，``best()`` 返回
    哪一个纯看内部排序 —— 实测会把 GB18030 中文判成 cp949，读出来是韩文乱码。

    修正范围刻意收窄到"chaos 打平的候选里同时有韩文编码与中文编码"：只有这时才改选
    中文编码。任何其它情况（包括短日文与 gb18030 打平）都尊重检测器的排序，避免为了
    修中文而读坏日文。
    """
    try:
        from charset_normalizer import from_bytes

        matches = list(from_bytes(data))
    except Exception:
        return None
    if not matches:
        return None
    try:
        best_chaos = min(float(m.percent_chaos) for m in matches)
    except Exception:
        return str(getattr(matches[0], "encoding", "") or "") or None

    tied = [m for m in matches if abs(float(m.percent_chaos) - best_chaos) < 1e-9]
    families = {_codec_family(str(m.encoding)): m for m in tied}
    has_korean = any(fam in _KOREAN_CODECS for fam in families)
    if has_korean:
        for fam, match in families.items():
            if fam in _CHINESE_CODECS:
                return str(match.encoding)
    return str(matches[0].encoding) or None


def detect_bomless_utf16(data: bytes) -> Optional[str]:
    """没有 BOM 的 UTF-16 兜底判定。

    charset-normalizer 对短样本容易把 ``b"C\\x00h\\x00..."`` 判成单字节编码，
    于是整本书变成乱码。判据：UTF-16 编码的拉丁文本每两个字节必有一个 0x00，
    据此判断字节序；再用"解出来是否可读"复核，避免把二进制误判成 UTF-16。

    已知局限：中文 UTF-16 的高低字节都不为 0，没有可用的偏向信号，因此**不做猜测**
    （猜错会把正常的 GB18030 文件读成乱码，代价比漏判高）。真实场景里无 BOM 的
    UTF-16 中文极少 —— Windows 记事本"Unicode"另存为一定会写 BOM。
    """
    sample = data[:8192]
    if len(sample) < 8:
        return None
    if len(sample) % 2:
        sample = sample[:-1]
    half = len(sample) // 2
    if half <= 0:
        return None
    nul_even = sum(1 for i in range(0, len(sample), 2) if sample[i] == 0)
    nul_odd = sum(1 for i in range(1, len(sample), 2) if sample[i] == 0)
    candidates: List[str] = []
    if nul_odd / half >= 0.2:
        candidates.append("utf-16-le")
    if nul_even / half >= 0.2:
        candidates.append("utf-16-be")
    for enc in candidates:
        try:
            text = sample.decode(enc)
        except UnicodeDecodeError:
            continue
        if _plausible_text(text):
            return enc
    return None


def _plausible_text(text: str) -> bool:
    """解码结果是否"像文本"：控制字符占比要低。"""
    if not text:
        return False
    bad = sum(1 for ch in text if ord(ch) < 32 and ch not in "\t\r\n\f\v")
    return bad / len(text) < 0.02


def decode_text(data: bytes, encoding: Optional[str] = None) -> Tuple[str, str]:
    """返回 (文本, 实际使用的编码)。解码失败逐级降级，最后 replace。"""
    candidates: List[str] = []
    if encoding:
        candidates.append(encoding)
    candidates.append(detect_encoding(data))
    candidates.extend(FALLBACK_ENCODINGS)
    tried: List[str] = []
    for enc in candidates:
        if not enc or enc.lower() in tried:
            continue
        tried.append(enc.lower())
        try:
            text = data.decode(enc)
        except (UnicodeDecodeError, LookupError):
            continue
        # 兜底：即使某个编解码器没有消费 BOM，也不能把 U+FEFF 留在正文里
        if text.startswith("\ufeff"):
            text = text.lstrip("\ufeff")
        return text, enc
    return data.decode("utf-8", errors="replace").lstrip("\ufeff"), "utf-8/replace"


# ---------------------------------------------------------------- 行处理
_LINE_SPLIT_RE = re.compile(r"\r\n|\r|\n")


def split_lines(text: str) -> List[str]:
    return _LINE_SPLIT_RE.split(text)


def normalize_spaces(text: str) -> str:
    """压缩行内连续空白（保留全角空格与制表符语义）。"""
    text = text.replace("\t", "    ").replace("\u3000", " ")
    text = re.sub(r"[ \f\v]{2,}", " ", text)
    return text.strip()


# ---------------------------------------------------------------- 章节切分
CHAPTER_PATTERNS: Sequence[re.Pattern] = (
    re.compile(r"^\s*第[零一二三四五六七八九十百千万两0-9]{1,10}[章卷回节部篇集]\s*.{0,30}$"),
    re.compile(r"^\s*Chapter\s+\d+.{0,30}$", re.IGNORECASE),
    re.compile(r"^\s*(序章|序言|前言|引子|楔子|后记|尾声|番外|附录).{0,20}$"),
)

CHAPTER_LINE_MAX_LEN = 60


def match_chapter_title(line: str) -> Optional[str]:
    """行是否是章节标题；是则返回标题（设计 7.2）。"""
    s = line.strip()
    if not s or len(s) > CHAPTER_LINE_MAX_LEN:
        return None
    for pat in CHAPTER_PATTERNS:
        if pat.match(s):
            return s
    return None


def is_cjk(text: str, ratio: float = 0.05) -> bool:
    if not text:
        return False
    cjk = 0
    sample = text[:20000]
    for ch in sample:
        if "\u4e00" <= ch <= "\u9fff" or "\u3040" <= ch <= "\u30ff":
            cjk += 1
            if cjk / max(1, len(sample)) >= ratio:
                return True
    return False


def count_chars(text: str) -> int:
    return len(text)


def plain_join(chunks: Iterable[str]) -> str:
    return "".join(chunks)


def truncate(text: str, limit: int) -> str:
    if len(text) <= limit:
        return text
    return text[:limit] + "…"


def snippet_around(text: str, start: int, end: int, radius: int = 30) -> str:
    """搜索命中片段：命中词前后各取 radius 个字符。"""
    a = max(0, start - radius)
    b = min(len(text), end + radius)
    prefix = "…" if a > 0 else ""
    suffix = "…" if b < len(text) else ""
    return f"{prefix}{text[a:b]}{suffix}"
