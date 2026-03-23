@echo off
setlocal

cd /d "%~dp0.." || exit /b 1

set "ROOT=%CD%"
set "SRC=%ROOT%\src"
set "BUILD=%SRC%\build"
set "EXE=%BUILD%\Release\memjet-rip.exe"
set "JSL_RUNTIME=%ROOT%\vendor\runtime\jsl"
set "PDL_THRIFT_ROOT=%ROOT%\vendor\pdl_py"
set "GS_STUB=%ROOT%\gswin64c"
set "GS_STUB_EXE=%ROOT%\gswin64c.exe"

if not exist "%SRC%\CMakeLists.txt" (
  echo [ERROR] Missing %SRC%\CMakeLists.txt
  exit /b 1
)

if not exist "%JSL_RUNTIME%" (
  echo [ERROR] Missing JSL runtime folder: %JSL_RUNTIME%
  echo [HINT] Place required JSL DLLs in vendor\runtime\jsl\
  exit /b 1
)

if not exist "%PDL_THRIFT_ROOT%\thrift" (
  echo [ERROR] Missing Thrift python runtime: %PDL_THRIFT_ROOT%\thrift
  echo [HINT] Ensure vendor\pdl_py contains thrift\ and Memjet\ packages
  exit /b 1
)

set "PATH=%JSL_RUNTIME%;%PATH%"
set "PDL_THRIFT_ROOT=%PDL_THRIFT_ROOT%"

if exist "%GS_STUB%" (
  echo [WARN] Local gswin64c shim detected at repo root: %GS_STUB%
  echo [WARN] This can shadow the real Ghostscript binary.
)
if exist "%GS_STUB_EXE%" (
  echo [WARN] Local gswin64c.exe detected at repo root: %GS_STUB_EXE%
  echo [WARN] This can shadow the real Ghostscript binary.
)

for /f "delims=" %%G in ('where gswin64c 2^>nul') do (
  echo [INFO] gswin64c=%%G
  set "GS_FOUND=1"
  goto gs_found
)

:gs_not_found
if not defined GS_FOUND (
  echo [ERROR] gswin64c not found in PATH
  echo [HINT] Install Ghostscript (gswin64c.exe)
  exit /b 1
)

:gs_found

echo [OK] setup-env complete
echo [INFO] ROOT=%ROOT%
echo [INFO] JSL_RUNTIME=%JSL_RUNTIME%
echo [INFO] PDL_THRIFT_ROOT=%PDL_THRIFT_ROOT%
exit /b 0
