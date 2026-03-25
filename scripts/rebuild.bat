@echo off
setlocal

call "%~dp0setup-env.bat" || exit /b 1

cd /d "%~dp0..\src" || exit /b 1

if exist build rmdir /s /q build
mkdir build
cd build

cmake .. || exit /b 1
cmake --build . --config Release || exit /b 1

if exist "%~dp0..\vendor\runtime\jsl\*.dll" (
  xcopy /Y /I /Q "%~dp0..\vendor\runtime\jsl\*.dll" "%CD%\Release\" >nul
)

if not exist "%CD%\Release\memjet-rip.exe" (
  echo [ERROR] memjet-rip.exe missing after build
  exit /b 1
)

echo [OK] BUILD_OK
exit /b 0
