# thirdparty（裁剪后的 vendored 依赖）

这些源码**随仓库提交**，构建不需要联网。为控制体积，只保留编译与 CMake configure
需要的文件（原始上游约 51 MB → 这里约 13 MB）。构建入口见 `../README.md`。

| 目录 | 上游 | 版本 / 参考 | 说明 |
|---|---|---|---|
| `cpp-httplib/` | https://github.com/yhirose/cpp-httplib | master @ `09fa6820`（`CPPHTTPLIB_VERSION "0.59.0"`） | **打了本地补丁**，见下 |
| `json/` | https://github.com/nlohmann/json | v3.12.0（`single_include/nlohmann/json.hpp`） | 单头 |
| `miniz/` | https://github.com/richgel999/miniz | 3.1.2（`MZ_VERSION "11.3.2"`） | 只用 `miniz.c/miniz_tdef.c/miniz_tinfl.c/miniz_zip.c` |
| `stb/` | https://github.com/nothings/stb | master @ `2c980bb5` | 只用 `stb_image.h`、`stb_image_write.h`、`stb_image_resize2.h` |
| `pugixml/` | https://github.com/zeux/pugixml | v1.16 | `src/` 全量 |
| `yaml-cpp/` | https://github.com/jbeder/yaml-cpp | yaml-cpp-0.9.0 | 关掉 test/tools，只留 `src/`+`include/`+`cmake/` |
| `freetype/` | https://gitlab.freedesktop.org/freetype/freetype | VER-2-14-3 | 关掉 zlib/bzip2/png/harfbuzz/brotli；保留 `src/`+`include/`+`builds/`（`builds/unix/*.in` 被 CMakeLists 引用） |

## cpp-httplib 的本地补丁

`patches/cpp-httplib-lnv-range.patch`（在 `server-cpp/` 下）只改一处：语法非法的
`Range` 头按 RFC 9110 §14.2 忽略，而不是在路由之前直接 416 —— 参照实现
（Python `routes_read.py::_slice_range`）对非法 Range 返回 200 整页，两边必须一致；
Range 语义由应用层负责。

- `thirdparty/cpp-httplib/httplib.h` 里带 `[LNV PATCH]` 注释标记。
- `build.ps1` 会校验这个标记，缺失时直接报错（否则会静默产出"非法 Range 返回 416"
  的二进制，与 Python 行为不一致）。
- 重新 vendor 上游时：先取干净的上游 `httplib.h`，再 `git apply` 该补丁文件。

## 升级某个依赖的步骤

1. 下载目标版本的源码（或 `git clone` 到临时目录）；
2. 按上表保留"编译 + configure 需要"的文件（可参考当前目录结构：删掉 `test/`、
   `docs/`、`examples/`、`.github/` 等）；
3. 覆盖对应子目录，跑一次**干净构建**（删掉 `build/`）：
   `powershell -ExecutionPolicy Bypass -File server-cpp\build.ps1`；
4. 跑回归测试：`cd server && .venv\Scripts\python -m pytest ..\server-cpp\tests -q`；
5. 对 cpp-httplib 记得重新应用补丁并确认 `build.ps1` 的校验通过；
6. 更新本文件的版本表。
