# 踩坑记录（server-cpp）

## 2026-10-04 构建成功被误报为失败：PS 5.1 Start-Process -NoNewWindow 重定向时 ExitCode 为 null
- 现象: build.ps1 打完 `[119/120] Linking lnvserver.exe` 后抛「构建失败 (exit )」；
  实际 lnvserver.exe 已生成（2.9MB）、build.err.log 为 0 字节，报错里退出码位置是空的。
- 根因: Windows PowerShell 5.1 中 `Start-Process -NoNewWindow -RedirectStandardOutput
  -RedirectStandardError -PassThru`（不带 -Wait）返回的 Process 对象，在轮询
  `HasExited` 结束后读 `ExitCode` 得到 $null（最小复现 2/2 稳定复现）；
  后续判断 `$proc.ExitCode -ne 0` 中 `$null -ne 0` 恒为真 → 成功构建被判为失败。
  对照：用 `-WindowStyle Hidden`（无 -NoNewWindow、无重定向）时 ExitCode 正常，
  坑只在 -NoNewWindow+重定向这个组合上。
- 修复: build.ps1 —— 弃用 Start-Process，改为直接 `& cmd.exe /c $allArgs | Tee-Object
  -FilePath build\build.log`；退出码用 `$LASTEXITCODE`；stderr 在 cmd 层面 `2>&1`
  并入 stdout（顺带绕开 PS 5.1 里 `$ErrorActionPreference=Stop` + 原生命令 stderr
  重定向会把第一行 stderr 变成 NativeCommandError 终止错误的坑）。
- 验证: 复现命令
  `Start-Process cmd '/c exit 7' -NoNewWindow -PassThru -RedirectStandardOutput o.log -RedirectStandardError e.log`
  + HasExited 轮询 → `ExitCode` 为空；`& cmd.exe /c 'exit 7'` → `$LASTEXITCODE`=7。
  端到端确认：重跑 build.ps1，增量构建应以「构建完成」结束。
- 防再犯: PS 5.1 脚本里原生子进程的退出码一律用 `$LASTEXITCODE`（直接 `&` 调用），
  不要依赖 `Start-Process -PassThru` 的 `ExitCode`；需要在
  `$ErrorActionPreference=Stop` 下合并原生命令 stderr 时，在 cmd 层做 `2>&1`，
  不要用 PowerShell 的 `2>&1`。

## 2026-10-04 PS 5.1 `& cmd /c` 传复合命令行会被引号机制静默破坏
- 现象: build.ps1 改为 `& cmd.exe /c $allArgs`（$allArgs =
  `/c call "vcvars64.bat" >nul 2>&1 && "inner.cmd" 2>&1`）后，cmd 立即以 exit 1
  退出、控制台零输出、build.log 根本没创建（Tee 没收到任何输入）。
- 根因: PowerShell 5.1 原生参数编组给**含空格的参数整体加引号，且不转义内部引号**；
  cmd.exe 收到后按自己的引号剥除规则（首字符为引号时剥掉首尾引号）解析，
  复合命令行被破坏成与预期完全不同的内容。探针实测 4 种写法：3 种带引号/空格的
  复合命令行**内层脚本静默不执行**（其中两种还返回 exit 0），唯一可靠形态是
  `& cmd.exe /c <不含空格的文件路径>`。旧脚本的 `Start-Process -ArgumentList
  <单字符串>` 是原样传递所以没触发这个坑，但那条路有上一条的 ExitCode=null 坑。
- 修复: build.ps1 —— 把 vcvars 调用、cd、cmake configure、cmake --build 全部写进
  临时 .cmd 文件（`[IO.File]::WriteAllText(..., ASCII)`，避免中文在 GBK 控制台乱码），
  PS 只执行 `& cmd.exe /c $innerFile | Tee-Object -FilePath build\build.log`；
  批处理内部对各命令做 `2>&1` 合并 stderr；调用前临时把 EAP 降为 Continue 兜底。
- 验证: 探针脚本 4 变体对照（`cmd /c <文件路径>` 正常执行并输出 INNER-RAN，
  复合命令行 3/4 静默失败）；重跑 build.ps1 全量构建 120/120 步完成并输出
  「构建完成: ...\lnvserver.exe」。
- 防再犯: PS 5.1 里要让 cmd 执行复合命令（call xxx && yyy、多步构建）时，
  复合逻辑一律写进 .cmd 文件、PS 只传文件路径；绝不把含引号+空格的复合命令行
  作为单个参数传给 `cmd /c`。

