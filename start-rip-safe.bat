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
powershell -NoProfile -Command ^
"$drive='%RIP_DRIVE%'; $minGb=[double]'%RIP_MIN_FREE_GB%'; $required=[int64]($minGb*1GB); $disk=Get-CimInstance Win32_LogicalDisk | Where-Object { $_.DeviceID -eq $drive } | Select-Object -First 1; if(-not $disk){Write-Host ('[ERROR] Drive not found: ' + $drive); exit 1}; $freeText=[string]$disk.FreeSpace; [int64]$free=0; if(-not [int64]::TryParse($freeText,[ref]$free)){Write-Host ('[ERROR] Invalid free space value for ' + $drive + ': ' + $freeText); exit 1}; $freeGb=[math]::Round($free/1GB,2); $requiredGb=[math]::Round($required/1GB,2); if($free -lt $required){Write-Host ('[ERROR] Low space on ' + $drive + ': ' + $freeGb + ' GB free (required >= ' + $requiredGb + ' GB)'); exit 1}else{Write-Host ('[OK] ' + $drive + ' free: ' + $freeGb + ' GB (required >= ' + $requiredGb + ' GB)'); exit 0}"
if errorlevel 1 exit /b 1

echo [INFO] RIP_TEMP_DIR=%RIP_TEMP_DIR%
echo [INFO] Starting RIP...

REM ===== Start your RIP command here =====
REM Replace the line below with your actual command:
REM C:\Users\Arrow\Arrow-Rip\build\memjet-rip.exe --your --args

endlocal & exit /b 0