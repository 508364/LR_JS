@echo off
chcp 65001 >nul 2>&1
REM L/R_JS Multi-Architecture Build Script (Windows x86, x64, ARM64)
REM Derive the version from include/lr_js.h (single source of truth).
for /f "tokens=3" %%a in ('findstr /C:"#define LR_JS_VERSION_MAJOR" "%~dp0include\lr_js.h"') do set VM=%%a
for /f "tokens=3" %%a in ('findstr /C:"#define LR_JS_VERSION_MINOR" "%~dp0include\lr_js.h"') do set VN=%%a
for /f "tokens=3" %%a in ('findstr /C:"#define LR_JS_VERSION_PATCH" "%~dp0include\lr_js.h"') do set VP=%%a
set VERSION=%VM%.%VN%.%VP%

echo ========================================
echo   L/R_JS v%VERSION% Build Script
echo ========================================
echo.

if not exist "build" mkdir build
if not exist "releases" mkdir releases

where cl >nul 2>nul
if %errorlevel% neq 0 (
    echo [ERROR] Visual Studio build tools not found.
    echo Please run this script from a Visual Studio Developer Command Prompt.
    exit /b 1
)

REM ---------- x64 ----------
echo [1/3] Building for Windows x64...
if exist "build\x64" rmdir /s /q "build\x64"
mkdir "build\x64"
cd "build\x64"
cmake -G "Visual Studio 17 2022" -A x64 ..\.. > "..\..\build\x64_cfg.log" 2>&1
if %errorlevel% neq 0 (
    echo [FAIL] x64 CMake configure failed. See build\x64_cfg.log
    cd "..\.."
    exit /b 1
)
cmake --build . --config Release >> "..\..\build\x64_cfg.log" 2>&1
if %errorlevel% neq 0 (
    echo [FAIL] x64 build failed. See build\x64_cfg.log
    cd "..\.."
    exit /b 1
)
cd "..\.."
echo x64 build completed!
powershell.exe -NoProfile -Command "Compress-Archive -Path 'build\x64\lib\Release\lr_js_static.lib','build\x64\bin\Release\lr_js.dll','build\x64\bin\Release\lr_js.exe','include\lr_js.h' -DestinationPath 'releases\LR_JS-%VERSION%-windows-x64.zip' -Force"
if %errorlevel% neq 0 (
    echo [FAIL] x64 packaging failed.
    exit /b 1
)
echo x64 package created!
echo.

REM ---------- x86 ----------
echo [2/3] Building for Windows x86 (Win32)...
if exist "build\x86" rmdir /s /q "build\x86"
mkdir "build\x86"
cd "build\x86"
cmake -G "Visual Studio 17 2022" -A Win32 ..\.. > "..\..\build\x86_cfg.log" 2>&1
if %errorlevel% neq 0 (
    echo [FAIL] x86 CMake configure failed. See build\x86_cfg.log
    cd "..\.."
    exit /b 1
)
cmake --build . --config Release >> "..\..\build\x86_cfg.log" 2>&1
if %errorlevel% neq 0 (
    echo [FAIL] x86 build failed. See build\x86_cfg.log
    cd "..\.."
    exit /b 1
)
cd "..\.."
echo x86 build completed!
powershell.exe -NoProfile -Command "Compress-Archive -Path 'build\x86\lib\Release\lr_js_static.lib','build\x86\bin\Release\lr_js.dll','build\x86\bin\Release\lr_js.exe','include\lr_js.h' -DestinationPath 'releases\LR_JS-%VERSION%-windows-x86.zip' -Force"
if %errorlevel% neq 0 (
    echo [FAIL] x86 packaging failed.
    exit /b 1
)
echo x86 package created!
echo.

REM ---------- ARM64 ----------
echo [3/3] Building for Windows ARM64...
if exist "build\arm64" rmdir /s /q "build\arm64"
mkdir "build\arm64"
cd "build\arm64"
cmake -G "Visual Studio 17 2022" -A ARM64 ..\.. > "..\..\build\arm64_cfg.log" 2>&1
if %errorlevel% neq 0 (
    echo [FAIL] ARM64 CMake configure failed. See build\arm64_cfg.log
    cd "..\.."
    exit /b 1
)
cmake --build . --config Release >> "..\..\build\arm64_cfg.log" 2>&1
if %errorlevel% neq 0 (
    echo [FAIL] ARM64 build failed. See build\arm64_cfg.log
    cd "..\.."
    exit /b 1
)
cd "..\.."
echo ARM64 build completed!
powershell.exe -NoProfile -Command "Compress-Archive -Path 'build\arm64\lib\Release\lr_js_static.lib','build\arm64\bin\Release\lr_js.dll','build\arm64\bin\Release\lr_js.exe','include\lr_js.h' -DestinationPath 'releases\LR_JS-%VERSION%-windows-arm64.zip' -Force"
if %errorlevel% neq 0 (
    echo [FAIL] ARM64 packaging failed.
    exit /b 1
)
echo ARM64 package created!
echo.

echo ========================================
echo   All builds completed successfully!
echo ========================================
echo.
echo Packages:
echo   - releases\LR_JS-%VERSION%-windows-x64.zip
echo   - releases\LR_JS-%VERSION%-windows-x86.zip
echo   - releases\LR_JS-%VERSION%-windows-arm64.zip
echo.
exit /b 0
