@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

echo ========================================
echo   Comprehensive Performance Benchmark
echo   IOME586 + Sandbox Parallel + JIT Cache
echo ========================================
echo.

set "LR_JS=.\build-debug\bin\lr_js.exe"
set "NODE=node"
set "SCRIPT=bench\bench_comprehensive.js"

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

:: ============================================================
:: PART 1: LR_JS Single-thread (IOME586 + JIT cache)
:: ============================================================
echo ========================================
echo   PART 1: LR_JS Single-thread
echo ========================================
"%LR_JS%" "%SCRIPT%"
set "LR_JS_EXIT=%ERRORLEVEL%"
echo.

:: ============================================================
:: PART 2: V8 Single-thread (Baseline)
:: ============================================================
echo ========================================
echo   PART 2: V8 (Node.js) Baseline
echo ========================================
"%NODE%" "%SCRIPT%"
set "V8_EXIT=%ERRORLEVEL%"
echo.

:: ============================================================
:: PART 3: LR_JS 16-thread Parallel
:: ============================================================
echo ========================================
echo   PART 3: LR_JS 16-thread Parallel
echo ========================================
"%LR_JS%" --parallel 16 "%SCRIPT%"
set "PAR_EXIT=%ERRORLEVEL%"
echo.

:: ============================================================
:: Summary
:: ============================================================
echo ========================================
echo   Execution Summary
echo ========================================
if "%LR_JS_EXIT%"=="0" (
    echo   [OK] LR_JS single-thread: SUCCESS
) else (
    echo   [FAIL] LR_JS single-thread: exit code %LR_JS_EXIT%
)

if "%V8_EXIT%"=="0" (
    echo   [OK] V8 baseline: SUCCESS
) else (
    echo   [FAIL] V8 baseline: exit code %V8_EXIT%
)

if "%PAR_EXIT%"=="0" (
    echo   [OK] LR_JS 16-thread parallel: SUCCESS
) else (
    echo   [FAIL] LR_JS 16-thread parallel: exit code %PAR_EXIT%
)
echo ========================================
echo.

echo Compare the timing output above for each engine.
echo.
pause
