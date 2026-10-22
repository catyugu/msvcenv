# msvcenv

[English](README.md) | 简体中文

为 **cmd** 和 **PowerShell** 快速提供 MSVC 开发环境，不必在每个 shell 里重复付
`vcvarsall.bat` / `Launch-VsDevShell.ps1` 的启动开销。

`Launch-VsDevShell.ps1` 大约要 2 秒；`msvcenv` 只捕获一次环境，之后在几十毫秒内重新注入。

## 工作原理

1. 用 `vswhere.exe -latest -products *` 定位 `vcvarsall.bat`，不硬编码任何工具链路径。
2. 生成一个临时批处理，在干净的 `cmd /d` 里执行 `vcvarsall.bat <arch>`，前后各 dump 一次
   环境。子进程拿到的是「撤掉上一次注入」之后的环境，所以无论是启动钩子（AutoRun）还是
   已经激活的 shell，都无法掩盖掉一部分差异。
3. 前后差异成为缓存：变更的变量、被移除的变量，以及属于工具集、Windows SDK、MSBuild 等的
   PATH 条目。
4. `print` 在那一刻把缓存渲染成 PowerShell 或批处理代码。PATH 也是此时才解析：前置目录排在
   最前面，上一次捕获遗留的条目被剔除，重复项去掉。

缓存刻意与 shell 无关：原始 `key=value`，不带引号，也不含任何脚本语法。只有 `print` 懂
PowerShell 和批处理的语法。

## 构建

需要一套可用的 MSVC（`cl.exe`）。

```bat
build.cmd
```

脚本通过 `vswhere` 找到 `vcvarsall.bat`，以 `/MT` 编译（完全静态、单个自包含 exe、不需要
CRT 可再发行包），把 `msvcenv.exe` 生成在源码旁边，目标文件放在 `build\`。编译选项为
`/std:c++17 /EHsc /MT /O2 /W4`。

## 安装

把 `msvcenv.exe`、`msvcenv.cmd`、`msvcenv.ps1` 复制到同一个目录，例如
`C:\tools\msvcenv`。缓存会自动创建为 exe 旁边的 `cache\<arch>.env`。

### cmd

把当前用户的 `AutoRun` 指向包装脚本：

```bat
reg add "HKCU\Software\Microsoft\Command Processor" /v AutoRun /t REG_EXPAND_SZ /d "call \"C:\tools\msvcenv\msvcenv.cmd\"" /f
```

如果 `AutoRun` 已被别的工具占用，保留它并追加，而不是覆盖：

```bat
if exist "C:\path\to\hook.bat" "C:\path\to\hook.bat" & call "C:\tools\msvcenv\msvcenv.cmd"
```

### PowerShell

`$PROFILE` 里加一行：

```powershell
. 'C:\tools\msvcenv\msvcenv.ps1'
```

当环境已经激活、或者 `msvcenv.exe` 不存在时，两个包装脚本都是静默无操作。捕获失败会写到
stderr，不会被吞掉。

## 命令

```text
msvcenv print   [--shell ps1|bat] [--arch x64]   输出注入脚本
msvcenv refresh [--arch x64]                     强制重新捕获
msvcenv status                                   显示发现结果与缓存状态
```

`print` 默认输出 PowerShell，可以直接用：

```powershell
Invoke-Expression ((& 'C:\tools\msvcenv\msvcenv.exe' print) -join [Environment]::NewLine)
```

`--shell bat` 输出批处理；`msvcenv.cmd` 走的就是这条路，因为 cmd 无法从管道应用环境变量，
必须有个文件让它 `call`。

## Visual Studio 升级之后

缓存记录了 `vcvarsall.bat` 的时间戳和工具集目录。任一变化时，下一次 `print` 会自己重新
捕获；`msvcenv refresh` 强制触发，`msvcenv status` 显示缓存是否过期。

捕获算的是「差量」，所以不能从已激活的环境里起步。`msvcenv` 会先把上一次注入从子进程的
环境里移除，再执行 `vcvarsall.bat`，因此从任何 shell 刷新都可以，包括本工具已经生效的
shell。唯一需要留意的是首次捕获：此时还没有缓存，没有东西可撤，需要在一个没有激活 MSVC
环境的 shell 里跑一次。删除 `cache\` 永远是安全的。

## 限制

- 默认只捕获 `x64`；用 `--arch` 选别的。
- 生成的批处理以 UTF-8 字节写出，而 cmd 按 ANSI 代码页读取，所以非 ASCII 的环境变量值在
  PowerShell 里没问题，走 cmd 不行。
- 只捕获环境变量。MSBuild 属性、CMake 包注册表、安装器写进 PATH 的条目一律不碰。
- 不会在 exe 自己的 `cache\` 之外写任何东西；工具不会替你改 `$PROFILE` 或注册表。

## 许可证

MIT
