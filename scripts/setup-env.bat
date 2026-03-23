@echo off
setlocal

cd /d "%~dp0.." || exit /b 1

set "ROOT=%CD%"
set "SRC=%ROOT%\src"
set "BUILD=%SRC%\build"
set "EXE=%BUILD%\Release\memjet-rip.exe"
set "JSL_RUNTIME=%ROOT%\vendor\runtime\jsl"
set "GS_STUB=%ROOT%\gswin64c"

if not exist "%SRC%\CMakeLists.txt" (
  echo [ERROR] Missing %SRC%\CMakeLists.txt
  exit /b 1
)

if not exist "%JSL_RUNTIME%" (
  echo [ERROR] Missing JSL runtime folder: %JSL_RUNTIME%
  echo [HINT] Place required JSL DLLs in vendor\runtime\jsl\
  exit /b 1
)

set "PATH=%JSL_RUNTIME%;%PATH%"

if exist "%GS_STUB%" set "PATH=%ROOT%;%PATH%"

where gswin64c >nul 2>nul
if errorlevel 1 (
  echo [ERROR] gswin64c not found in PATH
  echo [HINT] Install Ghostscript or provide local gswin64c wrapper in repo root
  exit /b 1
)

echo [OK] setup-env complete
echo [INFO] ROOT=%ROOT%
echo [INFO] JSL_RUNTIME=%JSL_RUNTIME%
exit /b 0
