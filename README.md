# msvcenv

Fast MSVC developer environment for **cmd** and **PowerShell**, without paying the
`vcvarsall.bat` / `Launch-VsDevShell.ps1` startup cost in every shell.

`Launch-VsDevShell.ps1` takes roughly 2 seconds; `msvcenv` captures the environment
once and re-injects it in tens of milliseconds.

## How it works

1. `vswhere.exe -latest -products *` locates `vcvarsall.bat`, so no toolchain path is
   hardcoded.
2. A temporary batch file runs `vcvarsall.bat <arch>` inside a clean `cmd /d` (AutoRun
   disabled, so a startup hook cannot pre-activate the environment) and dumps the
   environment before and after.
3. The difference becomes the cache: changed variables, removed variables, and the PATH
   entries belonging to the toolset, the Windows SDK, MSBuild and friends.
4. `print` renders that cache as PowerShell or batch code. PATH is resolved at that
   moment: prepended directories first, stale entries from an earlier capture dropped,
   duplicates removed.

The cache is deliberately shell-agnostic: raw `key=value` lines, no quoting, no
script syntax inside. Only `print` knows about PowerShell and batch.

## Build

Needs a working MSVC installation (`cl.exe`).

```bat
build.cmd
```

The script finds `vcvarsall.bat` through `vswhere`, compiles with `/MT` (fully static,
single self-contained exe, no CRT redistributable needed) and writes `msvcenv.exe` next
to the sources. Object files go to `build\`. The compile flags are `/std:c++17 /EHsc
/MT /O2 /W4`.

## Install

Copy `msvcenv.exe`, `msvcenv.cmd` and `msvcenv.ps1` into one directory, for example
`C:\tools\msvcenv`. The cache is created next to the exe as `cache\<arch>.env`.

### cmd

Point the per-user `AutoRun` value at the wrapper:

```bat
reg add "HKCU\Software\Microsoft\Command Processor" /v AutoRun /t REG_EXPAND_SZ /d "call \"C:\tools\msvcenv\msvcenv.cmd\"" /f
```

If something else already uses `AutoRun`, keep it and append instead of overwriting:

```bat
if exist "C:\path\to\hook.bat" "C:\path\to\hook.bat" & call "C:\tools\msvcenv\msvcenv.cmd"
```

### PowerShell

One line in `$PROFILE`:

```powershell
. 'C:\tools\msvcenv\msvcenv.ps1'
```

Both wrappers are silent no-ops when the environment is already active, when
`msvcenv.exe` is missing, and when no MSVC installation can be found.

## Commands

```text
msvcenv print   [--shell ps1|bat] [--arch x64]   emit the injection script
msvcenv refresh [--arch x64]                     force a new capture
msvcenv status                                   show discovery and cache state
```

`print` defaults to PowerShell output, so it can be used directly:

```powershell
Invoke-Expression ((& 'C:\tools\msvcenv\msvcenv.exe' print) -join [Environment]::NewLine)
```

`--shell bat` writes a batch script; that is what `msvcenv.cmd` does, because cmd cannot
apply environment changes from a pipe and needs a file to `call`.

## After a Visual Studio update

The cache stores the `vcvarsall.bat` timestamp and the toolset directory. If either
changes, the next `print` recaptures by itself. `msvcenv refresh` forces it; `msvcenv
status` shows whether the cache is stale. Deleting `cache\` is always safe.

## Limits

- `x64` is the only architecture captured by default; `--arch` selects another.
- The generated batch file is written as UTF-8 bytes and read by cmd in the ANSI code
  page, so non-ASCII environment values are safe in PowerShell but not through cmd.
- Only the environment is captured. MSBuild props, the CMake package registry and
  installer-provided PATH entries are untouched.
- Nothing is written outside the exe's own `cache\` directory; the tool never edits
  `$PROFILE` or the registry for you.

## License

MIT
