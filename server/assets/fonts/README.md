# 内置字体目录

这是服务器默认扫描的字体目录：`config.yaml` 里 `render.fonts_dir` 的默认值
`assets/fonts` 相对**配置文件所在目录**解析，即本目录（`server/assets/fonts`）。

把中文字体文件（`.otf` / `.ttf` / `.ttc`）放在这里，服务器启动时会自动发现：

```
NotoSansSC-Regular.otf     # 设计文档 4.1 选定的内置字体（思源黑体）
NotoSansSC-Bold.otf        # 可选：加粗由渲染层二次描边模拟，此项非必需
```

下载：https://github.com/notofonts/noto-cjk/releases（Noto Sans SC）。

## 为什么必须提供

分页需要精确的字符宽度，渲染需要使用与分页完全相同的字体，否则会出现
"服务器分页 vs 客户端字体差异"的错位（设计文档 3.1、4.5）。

## 没有字体时的行为

1. 回退到系统字体（Windows `C:\Windows\Fonts`、Linux `/usr/share/fonts`、
   macOS `/System/Library/Fonts`），并优先挑选支持中文的字体；
2. 仍然找不到时使用 Pillow 内置位图字体，**中文会显示为方框**，
   启动信息中会打印警告。

字体文件按 `render.default_font` 配置项选择默认字体（默认 `NotoSansSC-Regular`），
其余字体可在客户端阅读设置页通过 `GET /render/fonts` 选择。

提示：想在别的目录放字体也可以——把 `config.yaml` 的 `render.fonts_dir`
指向任意目录（相对路径相对配置文件所在目录解析）。
