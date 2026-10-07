"""LocalNetViewer 端到端 UI 验证（adb + 模拟器）。

前置：宿主机上服务器已在 8765 运行（见 .e2e/server-config.yaml），
模拟器已启动且 app-debug.apk 可安装。

用法：
    server\\.venv\\Scripts\\python.exe -X utf8 .e2e\\android_e2e.py

要点（来自 adb/模拟器实测经验）：
- 模拟器 guest 通过 10.0.2.2 访问宿主机，所以服务器地址填 10.0.2.2:8765；
- 每次输入/点击前重新 dump（键盘弹出会改布局）；
- 用 content-desc / text 定位，取 bounds 中心点 input tap；
- 中文一律走 UTF-8 文件，不经 PowerShell here-string。
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path


def _default_sdk() -> Path:
    env = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
    if env:
        return Path(env)
    # Windows 上 SDK Manager 的默认安装位置（随 Android Studio 安装）
    return Path.home() / "AppData" / "Local" / "Android" / "Sdk"


SDK = _default_sdk()
ADB = SDK / "platform-tools" / "adb.exe"
APK = Path(__file__).resolve().parent.parent / "android" / "app" / "build" / "outputs" / "apk" / "debug" / "app-debug.apk"
SHOTS = Path(__file__).resolve().parent / "shots"
PKG = "com.zqr.localnetviewer"
SERVER = "10.0.2.2:8765"

failures: list[str] = []
step_no = 0


def adb(*args: str, timeout: int = 90, binary: bool = False):
    proc = subprocess.run(
        [str(ADB), *args],
        capture_output=True,
        timeout=timeout,
    )
    if binary:
        return proc.stdout
    return proc.stdout.decode("utf-8", errors="replace")


def shell(*args: str, timeout: int = 90) -> str:
    return adb("shell", *args, timeout=timeout)


def check(label: str, ok: bool, detail: str = "") -> bool:
    print(f"[{'OK  ' if ok else 'FAIL'}] {label} {detail}", flush=True)
    if not ok:
        failures.append(label)
    return ok


def dump() -> ET.Element:
    """uiautomator dump 并解析；失败时重试（首次 dump 偶尔为空）。"""
    for _ in range(4):
        shell("uiautomator", "dump", "/sdcard/ui.xml")
        raw = adb("exec-out", "cat", "/sdcard/ui.xml", binary=True)
        text = raw.decode("utf-8", errors="replace")
        start = text.find("<?xml")
        if start >= 0:
            try:
                return ET.fromstring(text[start:])
            except ET.ParseError:
                pass
        time.sleep(1.5)
    raise AssertionError("uiautomator dump 解析失败")


def nodes(root: ET.Element):
    return root.iter("node")


def find(root: ET.Element, *, text: str | None = None, desc: str | None = None,
         contains: str | None = None):
    for node in nodes(root):
        t = node.get("text", "")
        d = node.get("content-desc", "")
        if text is not None and t == text:
            return node
        if desc is not None and d == desc:
            return node
        if contains is not None and (contains in t or contains in d):
            return node
    return None


def center(node: ET.Element) -> tuple[int, int]:
    m = re.match(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", node.get("bounds", ""))
    if not m:
        raise AssertionError(f"无法解析 bounds: {node.get('bounds')}")
    x1, y1, x2, y2 = map(int, m.groups())
    return (x1 + x2) // 2, (y1 + y2) // 2


def tap(node: ET.Element) -> None:
    x, y = center(node)
    shell("input", "tap", str(x), str(y))
    time.sleep(1.2)


def tap_text(label: str = "", *, contains: str | None = None, desc: str | None = None,
             wait: float = 6.0) -> bool:
    deadline = time.time() + wait
    while time.time() < deadline:
        root = dump()
        node = find(root, text=label or None, desc=desc, contains=contains)
        if node is not None:
            tap(node)
            return True
        time.sleep(1.0)
    return False


def type_text(value: str) -> None:
    shell("input", "text", value.replace(" ", "%s"))
    time.sleep(0.8)


def screenshot(name: str) -> Path:
    global step_no
    step_no += 1
    path = SHOTS / f"{step_no:02d}_{name}.png"
    path.write_bytes(adb("exec-out", "screencap", "-p", binary=True))
    print(f"      shot -> {path.name}", flush=True)
    return path


def has_text(fragment: str, *, wait: float = 8.0) -> bool:
    deadline = time.time() + wait
    while time.time() < deadline:
        if find(dump(), contains=fragment) is not None:
            return True
        time.sleep(1.0)
    return False


def main() -> int:
    SHOTS.mkdir(parents=True, exist_ok=True)

    if not APK.exists():
        print(f"APK 不存在: {APK}")
        return 2

    print("== 安装 ==", flush=True)
    out = adb("install", "-r", "-t", str(APK), timeout=300)
    check("安装 APK", "Success" in out, out.strip().splitlines()[-1] if out.strip() else "")

    shell("pm", "clear", PKG)
    shell("am", "start", "-n", f"{PKG}/.MainActivity")
    time.sleep(6)
    screenshot("connect_screen")
    check("连接页启动", has_text("连接服务器") or has_text("服务器地址"))

    print("== 连接服务器 ==", flush=True)
    root = dump()
    field = find(root, contains="192.168.1.10") or find(root, text="服务器地址")
    if field is None:
        # 退而求其次：点第一个可编辑框
        field = next((n for n in nodes(root) if n.get("class", "").endswith("EditText")), None)
    if not check("找到地址输入框", field is not None):
        return 1
    tap(field)
    type_text(SERVER)
    shell("input", "keyevent", "4")  # 收起键盘
    time.sleep(1)
    screenshot("address_entered")

    check("点击连接按钮", tap_text("连接", wait=8))
    time.sleep(6)
    screenshot("after_connect")

    connected = has_text("测试库", wait=15)
    check("连接成功并进入浏览页", connected)

    print("== 浏览书库 ==", flush=True)
    if not connected:
        root = dump()
        err = find(root, contains="无法连接") or find(root, contains="失败")
        if err is not None:
            print("      错误提示:", err.get("text"))
        screenshot("connect_failed")
        return 1

    # 书库根 → 测试库 → 小说
    check("进入 测试库", tap_text("测试库", wait=8))
    time.sleep(2)
    screenshot("library_root")
    check("打开 小说 目录", tap_text("小说", wait=8))
    time.sleep(2)
    screenshot("novel_dir")
    check("列出 TXT 书籍", has_text("长篇小说.txt", wait=8))

    print("== 打开 TXT 并阅读 ==", flush=True)
    check("打开长篇小说", tap_text("长篇小说.txt", wait=10))
    # 首次要解析 + 分页
    check("阅读页渲染完成", has_text("第 1 /", wait=40) or has_text("排版", wait=5), "")
    time.sleep(3)
    screenshot("reader_page1")
    root = dump()
    label = next((n.get("text", "") for n in nodes(root) if n.get("text", "").startswith("第 ")), "")
    check("显示页码", bool(label), label)
    check("显示章节名", find(root, contains="第1章") is not None or find(root, contains="测试章节") is not None)

    # 翻页
    print("== 翻页 ==", flush=True)
    for i in range(3):
        shell("input", "swipe", "800", "960", "200", "960", "250")
        time.sleep(2.5)
    screenshot("reader_after_swipes")
    root = dump()
    label2 = next((n.get("text", "") for n in nodes(root) if n.get("text", "").startswith("第 ")), "")
    check("翻页后页码前进", label2 != label and bool(label2), f"{label} -> {label2}")

    # 工具栏 → 目录 / 设置
    print("== 工具栏显隐 / 目录 / 设置 ==", flush=True)
    check("工具栏默认可见（含目录按钮）", has_text("目录", wait=8))
    screenshot("overlay_visible")

    shell("input", "tap", "540", "960")  # 点中间 → 隐藏
    time.sleep(1.8)
    check("点中间隐藏工具栏", not has_text("目录", wait=3))
    screenshot("overlay_hidden")

    shell("input", "tap", "540", "960")  # 再点 → 恢复
    time.sleep(1.8)
    check("再点中间恢复工具栏", has_text("目录", wait=6))

    if tap_text(desc="目录", wait=6):
        time.sleep(2)
        screenshot("toc_sheet")
        check("目录弹层显示章节", has_text("测试章节", wait=8) or has_text("章", wait=4))
        shell("input", "keyevent", "4")
        time.sleep(1.5)

    if not has_text("目录", wait=3):
        shell("input", "tap", "540", "960")
        time.sleep(1.5)
    if tap_text(desc="阅读设置", wait=6):
        time.sleep(2)
        screenshot("settings_sheet")
        check("设置弹层显示字号", has_text("字号", wait=8))
        shell("input", "keyevent", "4")
        time.sleep(1.5)

    # 返回键行为：沉浸态先出工具栏，再返回浏览页
    print("== 返回键 ==", flush=True)
    shell("input", "keyevent", "4")
    time.sleep(1.5)
    shell("input", "keyevent", "4")
    time.sleep(2.5)
    screenshot("back_to_browser")
    check("返回浏览页而不是退出 App",
          has_text("长篇小说.txt", wait=10) or has_text("小说", wait=4))

    print("== 漫画 ==", flush=True)
    shell("input", "keyevent", "4")  # 回到书库根（若在子目录）
    time.sleep(2)
    if tap_text("测试库", wait=8):
        time.sleep(2)
    if has_text("漫画", wait=6):
        if tap_text("漫画", wait=6):
            time.sleep(2)
            if tap_text("示例漫画.cbz", wait=10):
                time.sleep(10)
                screenshot("comic_reader")
                check("漫画阅读页", has_text("第 1 /", wait=30) or has_text("排版", wait=3))

    print()
    if failures:
        print(f"FAILED ({len(failures)}): {failures}")
        return 1
    print("ALL UI CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
