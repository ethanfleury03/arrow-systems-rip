@echo off
setlocal

call "%~dp0setup-env.bat" || exit /b 1

set "ROOT=%~dp0.."
set "EXE=%ROOT%\src\build\Release\memjet-rip.exe"

if not exist "%EXE%" (
  echo [ERROR] %EXE% not found. Run scripts\rebuild.bat first.
  exit /b 1
)

if "%~1"=="" (
  echo Usage:
  echo   scripts\test-print.bat "C:\path\input.pdf" [extra rip args]
  exit /b 1
)

"%EXE%" -i "%~1" --pes-ip 192.168.100.200 --pes-port 13001 --dpi 1600 --paper letter --page 1 -v %2 %3 %4 %5 %6 %7 %8 %9
set "RC=%ERRORLEVEL%"
echo [INFO] EXITCODE=%RC%
exit /b %RC%
