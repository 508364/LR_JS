#!/usr/bin/env bash
#
# L/R_JS - Multi-Architecture Linux Cross-Build Script
#
# Automatically detects every available gcc cross-compiler on the host and
# builds a release package for each one. No architecture list is hard-coded:
# if a cross-compiler is installed it gets built; if not, it is skipped.
#
# Prerequisites (Debian/Ubuntu example):
#   apt install gcc-{x86-64,i686,arm,arm64,riscv64,powerpc64le,mips,s390x}-linux-gnu
#
# Output per architecture:
#   releases/LR_JS-0.1.0-linux-<arch>.tar.gz
#     ├── lib/liblr_js.a
#     ├── lib/liblr_js.so
#     ├── bin/lr_js
#     └── lr_js.h
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# Derive the version from include/lr_js.h (single source of truth).
VER_MAJOR=$(grep -E '^#define +LR_JS_VERSION_MAJOR ' include/lr_js.h | awk '{print $3}')
VER_MINOR=$(grep -E '^#define +LR_JS_VERSION_MINOR ' include/lr_js.h | awk '{print $3}')
VER_PATCH=$(grep -E '^#define +LR_JS_VERSION_PATCH ' include/lr_js.h | awk '{print $3}')
VERSION="${VER_MAJOR}.${VER_MINOR}.${VER_PATCH}"

echo "========================================"
echo "  L/R_JS v$VERSION - Linux Cross Build"
echo "========================================"

# ---------- Phase 1/4: Cleanup ----------
echo
echo "[1/4] Cleaning previous build artifacts..."
rm -rf build_linux
rm -f releases/LR_JS-$VERSION-linux-*.tar.gz
mkdir -p build_linux releases

# ---------- Phase 2/4: Detect available toolchains ----------
# arch -> gcc cross-compiler command. Auto-discovered via `command -v`.
declare -A ARCH_COMPILERS=(
    [x86_64]="x86_64-linux-gnu-gcc"
    [i686]="i686-linux-gnu-gcc"
    [arm]="arm-linux-gnueabihf-gcc"
    [aarch64]="aarch64-linux-gnu-gcc"
    [riscv64]="riscv64-linux-gnu-gcc"
    [ppc64le]="powerpc64le-linux-gnu-gcc"
    [mips]="mips-linux-gnu-gcc"
    [s390x]="s390x-linux-gnu-gcc"
)

echo "[2/4] Detecting available gcc cross-compilers..."
TARGETS=()
for arch in "${!ARCH_COMPILERS[@]}"; do
    cc="${ARCH_COMPILERS[$arch]}"
    if command -v "$cc" >/dev/null 2>&1; then
        TARGETS+=("$arch")
        echo "  [OK]   $arch -> $cc"
    else
        echo "  [--]   $arch -> $cc (not found, skipped)"
    fi
done

if [ "${#TARGETS[@]}" -eq 0 ]; then
    echo "[ERROR] No gcc cross-compilers found."
    echo "        Install e.g. gcc-x86-64-linux-gnu, gcc-arm-linux-gnueabihf, ..."
    exit 1
fi
echo "  Detected ${#TARGETS[@]} target architecture(s)."

# ---------- Phase 3/4: Build ----------
echo
echo "[3/4] Building for detected architectures..."
for arch in "${TARGETS[@]}"; do
    cc="${ARCH_COMPILERS[$arch]}"
    echo
    echo "----------------------------------------"
    echo "  Building for $arch ($cc)"
    echo "----------------------------------------"
    bdir="build_linux/$arch"
    rm -rf "$bdir"
    mkdir -p "$bdir"

    # Minimal CMake toolchain file for cross-compilation.
    tc="$bdir/toolchain.cmake"
    cat > "$tc" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_C_COMPILER $cc)
# NOTE: do NOT set CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY here. That
# would make FindThreads mis-detect pthread as "already in libc" and skip
# linking -lpthread, breaking the link on glibc targets (aarch64, ppc64le,
# i686, ...) where pthread is a separate library.
EOF

    (
        cd "$bdir"
        cmake -G "Unix Makefiles" \
            -DCMAKE_TOOLCHAIN_FILE="$tc" \
            -DCMAKE_BUILD_TYPE=Release \
            "$ROOT"
    ) > "$bdir/configure.log" 2>&1 \
        || { echo "[FAIL] $arch configure failed. Tail of log:"; tail -n 25 "$bdir/configure.log"; exit 1; }

    cmake --build "$bdir" --config Release >> "$bdir/build.log" 2>&1 \
        || { echo "[FAIL] $arch build failed. Tail of log:"; tail -n 25 "$bdir/build.log"; exit 1; }

    echo "  $arch build completed."
done

# ---------- Phase 4/4: Package ----------
echo
echo "[4/4] Packaging builds..."
for arch in "${TARGETS[@]}"; do
    bdir="build_linux/$arch"
    libdir="$bdir/lib"
    bindir="$bdir/bin"
    pkg="releases/LR_JS-$VERSION-linux-$arch.tar.gz"

    # Static archive is built as liblr_js_static.a; the shared object as
    # liblr_js.so (with versioned symlinks). The CLI executable is lr_js.
    if [ ! -f "$libdir/liblr_js_static.a" ] || [ ! -f "$bindir/lr_js" ]; then
        echo "  [WARN] $arch missing artifacts, skipping package."
        continue
    fi

    tmp="build_linux/pkg_$arch"
    rm -rf "$tmp"
    mkdir -p "$tmp/lib" "$tmp/bin"
    cp "$libdir/liblr_js_static.a" "$tmp/lib/"
    # copy the shared library together with any versioned symlinks
    cp -a "$libdir"/liblr_js.so* "$tmp/lib/" 2>/dev/null || true
    cp "$bindir/lr_js" "$tmp/bin/"
    cp include/lr_js.h "$tmp/"

    ( cd "$tmp" && tar -czf "$ROOT/$pkg" . )
    echo "  Package created: $pkg"
done

echo
echo "========================================"
echo "  All Linux builds completed!"
echo "========================================"
echo "Packages in releases/:"
ls -1 releases/LR_JS-$VERSION-linux-*.tar.gz 2>/dev/null || echo "  (none)"
