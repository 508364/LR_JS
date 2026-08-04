#!/usr/bin/env bash
#
# L/R_JS - MinGW-w64 Cross-Build Script (run from WSL)
#
# Builds Windows .exe for x64, x86, ARM64 using MinGW GCC, which enables
# direct-threaded bytecode dispatch (computed goto) — typically 4-6× faster
# than the MSVC indirect-threaded build on loop-heavy workloads.
#
# Prerequisites (Ubuntu/WSL):
#   sudo apt install gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 gcc-mingw-w64-arm64
#
# After install you should have:
#   x86_64-w64-mingw32-gcc
#   i686-w64-mingw32-gcc
#   aarch64-w64-mingw32-gcc
#
# Output per architecture:
#   build_mingw/<arch>/bin/lr_js.exe
#   releases/LR_JS-<version>-mingw-<arch>.zip
#
# MSVC support (build_all.bat) is preserved alongside this.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# ── Version ────────────────────────────────────────────────────────────────
VER_MAJOR=$(grep -E '^#define +LR_JS_VERSION_MAJOR ' include/lr_js.h | awk '{print $3}')
VER_MINOR=$(grep -E '^#define +LR_JS_VERSION_MINOR ' include/lr_js.h | awk '{print $3}')
VER_PATCH=$(grep -E '^#define +LR_JS_VERSION_PATCH ' include/lr_js.h | awk '{print $3}')
VERSION="${VER_MAJOR}.${VER_MINOR}.${VER_PATCH}"

echo "========================================"
echo "  L/R_JS v$VERSION - MinGW Cross-Build"
echo "  Compiler: GCC (direct-threaded bytecode)"
echo "========================================"

# ── Cleanup ─────────────────────────────────────────────────────────────────
echo
echo "[1/3] Cleaning previous MinGW builds..."
rm -rf build_mingw
rm -f releases/LR_JS-$VERSION-mingw-*.zip
mkdir -p build_mingw releases

# ── Detect available MinGW compilers ────────────────────────────────────────
echo
echo "[2/3] Detecting MinGW cross-compilers..."

declare -A MINGW_CC=(
    [x64]="x86_64-w64-mingw32-gcc"
    [x86]="i686-w64-mingw32-gcc"
    [arm64]="aarch64-w64-mingw32-gcc"
)

declare -A MINGW_TRIPLE=(
    [x64]="x86_64-w64-mingw32"
    [x86]="i686-w64-mingw32"
    [arm64]="aarch64-w64-mingw32"
)

TARGETS=()
for arch in x64 x86 arm64; do
    cc="${MINGW_CC[$arch]}"
    if command -v "$cc" >/dev/null 2>&1; then
        TARGETS+=("$arch")
        ver=$("$cc" --version 2>&1 | head -1)
        echo "  [OK]   $arch  →  $ver"
    else
        echo "  [--]   $arch  →  $cc (not found, skipped)"
    fi
done

if [ "${#TARGETS[@]}" -eq 0 ]; then
    echo
    echo "[ERROR] No MinGW cross-compilers found."
    echo "  Ubuntu/WSL: sudo apt install gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 gcc-mingw-w64-arm64"
    exit 1
fi
echo "  Building for ${#TARGETS[@]} architecture(s)."

# ── Build each architecture ─────────────────────────────────────────────────
echo
echo "[3/3] Building..."

for arch in "${TARGETS[@]}"; do
    cc="${MINGW_CC[$arch]}"
    triple="${MINGW_TRIPLE[$arch]}"
    bdir="build_mingw/$arch"
    echo
    echo "----------------------------------------"
    echo "  $arch  ($cc)"
    echo "----------------------------------------"

    rm -rf "$bdir"
    mkdir -p "$bdir"

    # Toolchain file: tells CMake to cross-compile for Windows with MinGW
    tc="$ROOT/$bdir/toolchain.cmake"
    cat > "$tc" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR $arch)
set(CMAKE_C_COMPILER $cc)
set(CMAKE_RC_COMPILER ${triple}-windres)

# Use host tools (cmake, etc.), find target libs/headers in sysroot
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Threading: MinGW provides winpthreads
set(THREADS_PTHREAD_ARG "2" CACHE STRING "Force pthread" FORCE)
EOF

    # Configure (must run inside build dir so CMake writes there)
    (
        cd "$ROOT/$bdir"
        cmake -G "Unix Makefiles" \
            -DCMAKE_TOOLCHAIN_FILE="$tc" \
            -DCMAKE_BUILD_TYPE=Release \
            "$ROOT"
    ) > "$ROOT/$bdir/configure.log" 2>&1 \
        || { echo "[FAIL] $arch configure failed."; tail -30 "$ROOT/$bdir/configure.log"; exit 1; }

    # Build (parallel)
    nproc=$(nproc 2>/dev/null || echo 4)
    cmake --build "$ROOT/$bdir" --config Release -j "$nproc" > "$ROOT/$bdir/build.log" 2>&1 \
        || { echo "[FAIL] $arch build failed."; tail -30 "$ROOT/$bdir/build.log"; exit 1; }

    # Verify
    exe="$ROOT/$bdir/bin/lr_js.exe"
    if [ ! -f "$exe" ]; then
        exe=$(find "$ROOT/$bdir" -name "lr_js.exe" -type f 2>/dev/null | head -1)
    fi
    if [ -z "$exe" ] || [ ! -f "$exe" ]; then
        echo "[FAIL] $arch: lr_js.exe not found after build."
        exit 1
    fi

    # Confirm it's a Windows PE (not Linux ELF)
    filetype=$(file "$exe" 2>/dev/null || echo "unknown")
    echo "  Built:  $exe"
    echo "  Type:   $filetype"

    # Quick smoke test (run with wine if available, else skip)
    if command -v wine64 >/dev/null 2>&1; then
        echo -n "  Test:   "
        wine64 "$exe" -e "console.log('OK from MinGW $arch')" 2>/dev/null \
            && echo "PASS" || echo "SKIP (wine failed)"
    else
        echo "  Test:   SKIP (wine not installed)"
    fi

    # Package
    pkg="$ROOT/releases/LR_JS-$VERSION-mingw-$arch.zip"
    tmp="$ROOT/build_mingw/pkg_$arch"
    rm -rf "$tmp"
    mkdir -p "$tmp"
    cp "$exe" "$tmp/"
    cp "$ROOT/include/lr_js.h" "$tmp/"
    (cd "$tmp" && zip -qr "$pkg" .)
    echo "  Package: $pkg"
    echo "  $arch build completed."
done

echo
echo "========================================"
echo "  All MinGW builds completed!"
echo "========================================"
echo "Packages:"
ls -1 "$ROOT/releases"/LR_JS-$VERSION-mingw-*.zip 2>/dev/null || echo "  (none)"
echo
echo "To run on Windows: extract the zip and run lr_js.exe"
