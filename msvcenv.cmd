@rem Injects the cached MSVC environment into the current cmd session.
@rem No-op when the environment is already active, so it is safe to run always.
@if defined VSCMD_VER exit /b 0
@set "MSVCENV_TMP=%~dp0msvcenv-inject-%RANDOM%%RANDOM%.cmd"
@"%~dp0msvcenv.exe" print --shell bat > "%MSVCENV_TMP%" || goto :msvcenv_done
@call "%MSVCENV_TMP%"
:msvcenv_done
@del /q "%MSVCENV_TMP%" >nul 2>&1
@set "MSVCENV_TMP="
