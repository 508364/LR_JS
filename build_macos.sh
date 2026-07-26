#!/usr/bin/env bash
#
# L/R_JS - macOS (Darwin) Cross-Build Script (via osxcross)
#
# Automatically detects available osxcross toolchains for the macOS target
# architectures (x86_64 and arm64). Any toolchain that is present is built;
# any that is missing is skipped. Nothing is hard-coded beyond the candidate
# compiler names.
#
# Prerequisites:
#   Build and install osxcross (https://github.com/tpoechtrager/osxcross)
#   so that <arch>-apple-darwin*-clang (or o64-clang / oa64-clang) is on PATH.
#
# Output per architecture:
#   releases/LR_JS-0.1.0-macos-<arch>.tar.gz
#     ├── lib/liblr_js.a
#     ├── lib/liblr_js.dylib
#     ├── bin/lr_js
#     └── lr_js.h
#
set -euo pipefail

VERSION="0.1.0"
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

echo "========================================"
echo "  L/R_JS v$VERSION - macOS Cross Build"
echo "========================================"

# ---------- Phase 1/4: Cleanup ----------
echo
echo "[1/4] Cleaning previous build artifacts..."
rm -rf build_macos
rm -f releases/LR_JS-$VERSION-macos-*.tar.gz
mkdir -p build_macos releases

# ---------- Phase 2/4: Detect available toolchains ----------
# arch -> space-separated candidate compiler names (first hit wins).
declare -A ARCH_CC=(
    [x86_64]="x86_64-apple-darwin-clang x86_64-apple-darwin20-clang x86_64-apple-darwin21-clang o64-clang"
    [arm64]="aarch64-apple-darwin-clang aarch64-apple-darwin20-clang aarch64-apple-darwin21-clang oa64-clang"
)

echo "[2/4] Detecting available macOS (osxcross) toolchains..."
TARGETS=()
for arch in "${!ARCH_CC[@]}"; do
    found=""
    for cand in ${ARCH_CC[$arch]}; do
        if command -v "$cand" >/dev/null 2>&1; then
            found="$cand"
            break
        fi
    done
    if [ -n "$found" ]; then
        ARCH_CC[$arch]="$found"
        TARGETS+=("$arch")
        echo "  [OK]   $arch -> $found"
    else
        echo "  [--]   $arch -> (no osxcross toolchain found, skipped)"
    fi
done

if [ "${#TARGETS[@]}" -eq 0 ]; then
    echo "[ERROR] No macOS (osxcross) toolchains found."
    echo "        Install osxcross so that the darwin clang compilers are on PATH."
    exit 1
fi
echo "  Detected ${#TARGETS[@]} target architecture(s)."

# ---------- Phase 3/4: Build ----------
echo
echo "[3/4] Building for detected architectures..."
for arch in "${TARGETS[@]}"; do
    cc="${ARCH_CC[$arch]}"
    echo
    echo "----------------------------------------"
    echo "  Building for macOS $arch ($cc)"
    echo "----------------------------------------"
    bdir="build_macos/$arch"
    rm -rf "$bdir"
    mkdir -p "$bdir"

    tc="$bdir/toolchain.cmake"
    cat > "$tc" <<EOF
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_C_COMPILER $cc)
set(CMAKE_OSX_ARCHITECTURES $arch CACHE STRING "" FORCE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
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

    echo "  macOS $arch build completed."
done

# ---------- Phase 4/4: Package ----------
echo
echo "[4/4] Packaging builds..."
for arch in "${TARGETS[@]}"; do
    bdir="build_macos/$arch"
    libdir="$bdir/lib"
    bindir="$bdir/bin"
    pkg="releases/LR_JS-$VERSION-macos-$arch.tar.gz"

    # Static archive is built as liblr_js_static.a; the shared object as
    # liblr_js.dylib (with versioned symlinks). The CLI executable is lr_js.
    if [ ! -f "$libdir/liblr_js_static.a" ] || [ ! -f "$bindir/lr_js" ]; then
        echo "  [WARN] $arch missing artifacts, skipping package."
        continue
    fi

    tmp="build_macos/pkg_$arch"
    rm -rf "$tmp"
    mkdir -p "$tmp/lib" "$tmp/bin"
    cp "$libdir/liblr_js_static.a" "$tmp/lib/"
    # copy the shared library together with any versioned symlinks
    cp -a "$libdir"/liblr_js.dylib* "$tmp/lib/" 2>/dev/null || true
    cp "$bindir/lr_js" "$tmp/bin/"
    cp include/lr_js.h "$tmp/"

    ( cd "$tmp" && tar -czf "$ROOT/$pkg" . )
    echo "  Package created: $pkg"
done

echo
echo "========================================"
echo "  All macOS builds completed!"
echo "========================================"
echo "Packages in releases/:"
ls -1 releases/LR_JS-$VERSION-macos-*.tar.gz 2>/dev/null || echo "  (none)"
