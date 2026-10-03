"""轻量 XML 辅助：按 local-name 检索，忽略命名空间差异（FB2 / OPF / NCX 共用）。

**重要陷阱**：lxml 元素的真值来自"有没有子元素"，`bool(<book-title>示例</book-title>)`
是 ``False``。因此这些函数的返回值**必须**用 ``is not None`` / ``is None`` 判断，
绝不能写 ``if first_local(...):`` 或 ``first_local(...) or fallback`` —— 那会把
"找到了叶子节点"误判成"没找到"。
"""

from __future__ import annotations

from typing import List, Optional


def safe_fromstring(data: bytes):
    """解析书籍内的不可信 XML（container.xml / OPF / NCX / FB2）。

    安全约束（防 XXE / 实体膨胀）：
    - ``resolve_entities=False``：不展开内部子集里定义的实体——恶意书籍可借
      ``file://`` 外部实体把服务器本机文件读进正文返回给客户端；
    - ``no_network=True``、``load_dtd=False``、``dtd_validation=False``：
      不加载任何外部 DTD；
    - ``huge_tree=False``（默认）：保留 libxml2 对深嵌套/超大节点的防护。

    书籍格式本身不需要 DTD 实体（OPF/NCX/FB2 都不含），因此关闭它们不影响
    正常书籍的解析。
    """
    from lxml import etree  # noqa: PLC0415

    parser = etree.XMLParser(
        resolve_entities=False,
        no_network=True,
        load_dtd=False,
        dtd_validation=False,
        huge_tree=False,
    )
    return etree.fromstring(data, parser=parser)


def localname(el) -> str:
    try:
        from lxml import etree  # noqa: PLC0415

        return str(etree.QName(el).localname)
    except Exception:
        tag = getattr(el, "tag", "")
        if isinstance(tag, str) and "}" in tag:
            return tag.rsplit("}", 1)[-1]
        return str(tag)


def iter_local(root, localname_: str):
    for el in root.iter():
        if localname(el) == localname_:
            yield el


def find_local(root, localname_: str) -> List:
    return list(iter_local(root, localname_))


def first_local(root, localname_: str):
    for el in iter_local(root, localname_):
        return el
    return None


def has_local(root, localname_: str) -> bool:
    """是否存在该 local-name 的元素。

    用于替代 ``if first_local(...):`` —— 直接判断元素真值会踩到 lxml 的
    "叶子节点为假值"陷阱（见模块 docstring）。
    """
    return first_local(root, localname_) is not None


def text_local(root, localname_: str) -> Optional[str]:
    for el in iter_local(root, localname_):
        text = _all_text(el).strip()
        if text:
            return text
    return None


def _all_text(el) -> str:
    parts: List[str] = []
    if el.text:
        parts.append(str(el.text))
    for child in el:
        parts.append(_all_text(child))
        if child.tail:
            parts.append(str(child.tail))
    return "".join(parts)


def all_text(el) -> str:
    return _all_text(el)


def attr_local(el, name: str) -> Optional[str]:
    """取属性，兼容带命名空间的 ``xlink:href`` 形式。"""
    value = el.get(name)
    if value:
        return str(value)
    for key, val in el.attrib.items():
        if str(key).endswith("}" + name) or str(key) == name:
            return str(val)
    return None
