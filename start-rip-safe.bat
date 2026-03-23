@echo off
setlocal

REM ===== Config =====
set RIP_DRIVE=D:
set RIP_ROOT=D:\ArrowRip
set RIP_TEMP_DIR=%RIP_ROOT%\temp
set RIP_LOG_DIR=%RIP_ROOT%\logs
set RIP_MIN_FREE_GB=10

REM RIP guardrail env vars
set RIP_TEMP_CLEANUP_ENABLE=1
set RIP_TEMP_CLEANUP_MAX_AGE_HOURS=24
set RIP_TEMP_CLEANUP_KEEP_LATEST=20
set RIP_TEMP_SAFETY_MULTIPLIER=1.5
set RIP_TEMP_MIN_FREE_BYTES=0
set RIP_TEMP_VERBOSE=1

REM ===== Mount checks =====
if not exist "%RIP_DRIVE%\" (
  echo [ERROR] %RIP_DRIVE% not mounted. Plug in storage and retry.
  exit /b 1
)

if not exist "%RIP_ROOT%" mkdir "%RIP_ROOT%"
if not exist "%RIP_TEMP_DIR%" mkdir "%RIP_TEMP_DIR%"
if not exist "%RIP_LOG_DIR%" mkdir "%RIP_LOG_DIR%"
if not exist "%RIP_ROOT%\archive" mkdir "%RIP_ROOT%\archive"
if not exist "%RIP_ROOT%\incoming" mkdir "%RIP_ROOT%\incoming"
if not exist "%RIP_ROOT%\failed" mkdir "%RIP_ROOT%\failed"

REM ===== Free-space check =====
echo [INFO] Checking free space on %RIP_DRIVE%...
set RIP_FREE_BYTES_RAW=
for /f "tokens=2 delims=:" %%A in ('fsutil volume diskfree %RIP_DRIVE% ^| findstr /I /C:"Total free bytes"') do (
  for /f "tokens=*" %%B in ("%%A") do set "RIP_FREE_BYTES_RAW=%%B"
)
for /f "tokens=1" %%C in ("%RIP_FREE_BYTES_RAW%") do set "RIP_FREE_BYTES_RAW=%%C"
set "RIP_FREE_BYTES=%RIP_FREE_BYTES_RAW:,=%"
if not defined RIP_FREE_BYTES (
  echo [ERROR] Could not determine free space on %RIP_DRIVE%.
  exit /b 1
)
echo(%RIP_FREE_BYTES%| findstr /R "^[0-9][0-9]*$" >nul
if errorlevel 1 (
  echo [ERROR] Invalid free space value on %RIP_DRIVE%: %RIP_FREE_BYTES%
  exit /b 1
)
powershell -NoProfile -Command "$free=[int64]%RIP_FREE_BYTES%; $required=[int64](%RIP_MIN_FREE_GB%*1GB); $freeGb=[math]::Round($free/1GB,2); $requiredGb=[math]::Round($required/1GB,2); if($free -lt $required){Write-Host ('[ERROR] Low space on %RIP_DRIVE%: ' + $freeGb + ' GB free (required >= ' + $requiredGb + ' GB)'); exit 1}else{Write-Host ('[OK] %RIP_DRIVE% free: ' + $freeGb + ' GB (required >= ' + $requiredGb + ' GB)'); exit 0}"
if errorlevel 1 exit /b 1

echo [INFO] RIP_TEMP_DIR=%RIP_TEMP_DIR%
echo [INFO] Starting RIP...

REM ===== Start RIP =====
set "RIP_EXE="

REM Keep lookup order explicit; chained IF forms can misparse in cmd when invoked via call .\script.bat.
if exist "C:\Users\Arrow\Arrow-Rip\build\release\memjet-rip.exe" set "RIP_EXE=C:\Users\Arrow\Arrow-Rip\build\release\memjet-rip.exe"
if defined RIP_EXE goto rip_exe_resolved
if exist "C:\Users\Arrow\Arrow-Rip\build\Release\memjet-rip.exe" set "RIP_EXE=C:\Users\Arrow\Arrow-Rip\build\Release\memjet-rip.exe"
if defined RIP_EXE goto rip_exe_resolved
if exist "C:\Users\Arrow\Arrow-Rip\build\memjet-rip.exe" set "RIP_EXE=C:\Users\Arrow\Arrow-Rip\build\memjet-rip.exe"
if defined RIP_EXE goto rip_exe_resolved
if exist "%~dp0build\release\memjet-rip.exe" set "RIP_EXE=%~dp0build\release\memjet-rip.exe"
if defined RIP_EXE goto rip_exe_resolved
if exist "%~dp0build\Release\memjet-rip.exe" set "RIP_EXE=%~dp0build\Release\memjet-rip.exe"
if defined RIP_EXE goto rip_exe_resolved
if exist "%~dp0build\memjet-rip.exe" set "RIP_EXE=%~dp0build\memjet-rip.exe"
if defined RIP_EXE goto rip_exe_resolved

echo [ERROR] RIP executable not found. Checked:
echo [ERROR]   C:\Users\Arrow\Arrow-Rip\build\release\memjet-rip.exe
echo [ERROR]   C:\Users\Arrow\Arrow-Rip\build\Release\memjet-rip.exe
echo [ERROR]   C:\Users\Arrow\Arrow-Rip\build\memjet-rip.exe
echo [ERROR]   %~dp0build\release\memjet-rip.exe
echo [ERROR]   %~dp0build\Release\memjet-rip.exe
echo [ERROR]   %~dp0build\memjet-rip.exe
exit /b 1

:rip_exe_resolved
echo [INFO] Launch command: "%RIP_EXE%" %*
call "%RIP_EXE%" %*
set "RIP_LAUNCH_RC=%ERRORLEVEL%"
if "%RIP_LAUNCH_RC%"=="0" goto rip_ok
echo [ERROR] RIP launch failed (exit code %RIP_LAUNCH_RC%).
exit /b 1

:rip_ok
echo [INFO] RIP exited successfully.
endlocal & exit /b 0
