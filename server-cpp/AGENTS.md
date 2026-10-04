# server-cpp 修改规则

- Windows PowerShell 5.1 脚本中原生子进程退出码一律用 `$LASTEXITCODE`（直接 `&` 调用），
  不要用 `Start-Process -PassThru` 的 `ExitCode`（-NoNewWindow+重定向时为 null，
  会把成功误判为失败）；原生命令 stderr 合并要在 cmd 层做 `2>&1`，不要用 PS 的 `2>&1`。
  见 LESSONS.md 2026-10-04 条目一。
- PS 5.1 让 cmd 执行复合命令（call vcvars && build 等）时，复合逻辑一律写进临时
  .cmd 文件（ASCII 编码），PS 只传文件路径；绝不把含引号+空格的复合命令行作为
  单个参数传给 `cmd /c`（`&` 传参整体加引号且不转义内部引号，命令行被 cmd 引号
  剥除规则破坏，实测内层脚本静默不执行）。见 LESSONS.md 2026-10-04 条目二。
