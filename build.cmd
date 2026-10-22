@echo off
rem Builds msvcenv.exe as a static (/MT) native tool. Everything stays inside this
rem directory: msvcenv.exe, build\, msvcenv-cache\.
rem Usage: build.cmd [output exe path]
setlocal
set "HERE=%~dp0"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%HERE%msvcenv.exe"

pushd "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer" 2>nul
if errorlevel 1 (
  echo build: Visual Studio Installer directory not found
  exit /b 1
)
set "VCVARS="
for /f "delims=" %%i in ('vswhere.exe -latest -products * -find "VC\Auxiliary\Build\vcvarsall.bat"') do set "VCVARS=%%i"
popd

if not defined VCVARS (
  echo build: vcvarsall.bat not found - install Microsoft.VisualStudio.Component.VC.Tools.x86.x64
  exit /b 1
)

call "%VCVARS%" x64 >nul 2>&1
if errorlevel 1 (
  echo build: vcvarsall failed
  exit /b 1
)

if not exist "%HERE%build" mkdir "%HERE%build"
pushd "%HERE%" || exit /b 1
cl /nologo /std:c++17 /EHsc /MT /O2 /W4 "msvcenv.cpp" /Fe:"%OUT%" /Fo:"%HERE%build\msvcenv.obj"
set "RC=%errorlevel%"
popd
if not "%RC%"=="0" exit /b %RC%
echo built: %OUT%
