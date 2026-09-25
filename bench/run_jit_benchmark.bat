@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

echo ========================================
echo   JIT Performance Benchmark
echo   LR_JS (Debug) vs V8 (Node.js)
echo ========================================
echo.

set "LR_JS=.\build-debug\bin\lr_js.exe"
set "NODE=node"
set "SCRIPT=bench\bench_jit_vs_v8.js"

:: Check if files exist
if not exist "%LR_JS%" (
    echo [ERROR] LR_JS not found: %LR_JS%
    echo Please build with: build_debug.bat
    pause
    exit /b 1
)

where node >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Node.js not found in PATH
    pause
    exit /b 1
)

echo [INFO] LR_JS: %LR_JS%
echo [INFO] V8: %NODE%
echo [INFO] Script: %SCRIPT%
echo.

:: Run LR_JS test
echo --- LR_JS (JIT) ---
"%LR_JS%" "%SCRIPT%"
set "LR_JS_EXIT=%ERRORLEVEL%"
echo.

:: Run V8 test
echo --- V8 (Node.js) ---
"%NODE%" "%SCRIPT%"
set "V8_EXIT=%ERRORLEVEL%"
echo.

:: Summary
echo ========================================
echo   Results
echo ========================================
if "%LR_JS_EXIT%"=="0" (
    echo   LR_JS: SUCCESS (exit code 0)
) else (
    echo   LR_JS: FAILED (exit code %LR_JS_EXIT%)
)

if "%V8_EXIT%"=="0" (
    echo   V8:    SUCCESS (exit code 0)
) else (
    echo   V8:    FAILED (exit code %V8_EXIT%)
)
echo ========================================
echo.

if "%LR_JS_EXIT%"=="0" if "%V8_EXIT%"=="0" (
    echo Both engines completed successfully!
    echo Compare the timing output above.
)

pause
