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
    # CMake cannot auto-detect the Darwin archiver when cross-compiling, so we
    # must point CMAKE_AR / CMAKE_RANLIB at the osxcross wrappers explicitly.
    # The wrapper name varies between installs: o64-ar / oa64-ar, or the
    # versioned x86_64-apple-darwin*-ar / aarch64-apple-darwin*-ar, or llvm-ar.
    # Search the compiler's own bin directory for a match.
    cc_path="$(command -v "$cc")"
    cc_bin="$(dirname "$cc_path")"
    ar_tool=""
    for cand in "$cc_bin/${cc%-clang}-ar" \
                "$cc_bin"/x86_64-apple-darwin*-ar \
                "$cc_bin"/aarch64-apple-darwin*-ar \
                "$cc_bin"/llvm-ar; do
        if [ -x "$cand" ]; then ar_tool="$cand"; break; fi
    done
    if [ -z "$ar_tool" ]; then
        echo "  [WARN] no Darwin archiver found; using system 'ar' (link will likely fail)"
        ar_tool="ar"
    fi
    ranlib_tool=""
    for cand in "$cc_bin/${cc%-clang}-ranlib" \
                "$cc_bin"/x86_64-apple-darwin*-ranlib \
                "$cc_bin"/aarch64-apple-darwin*-ranlib \
                "$cc_bin"/llvm-ranlib; do
        if [ -x "$cand" ]; then ranlib_tool="$cand"; break; fi
    done
    [ -z "$ranlib_tool" ] && ranlib_tool="ranlib"

    # ── SDK + compiler resolution (auto-detected unless overridden) ──────
    # The osxcross *-clang launcher hardcodes SDKROOT=15.5 and appends its
    # own -isysroot AFTER the user's flags, so CMAKE_OSX_SYSROOT / the
    # SDKROOT env var cannot override it. We therefore bypass the launcher
    # and invoke the real target-specific clang directly, with explicit
    # -target / -isysroot / -mmacosx-version-min. The binary's filename
    # encodes the exact triple, which we reuse for -target so the matching
    # <triple>-ld linker (e.g. x86_64-apple-darwin21.4-ld) is found.
    #
    # SDK selection: use LR_OSX_SDK / SDKROOT if set, otherwise auto-detect.
    # Newer SDKs (>=15) expose the '_Float16' type which the older osxcross
    # clang builds cannot compile, so auto-detection prefers the SDK whose
    # macOS major version matches the clang's baked darwin version and, as a
    # final fallback, the oldest available SDK.
    cc_path="${cc_path:-$(command -v "$cc")}"
    cc_bin="${cc_bin:-$(dirname "$cc_path")}"
    cmake_cc="$cc"

    # Locate the real target-specific clang (bypass the o64/oa64 launcher).
    real_clang=""
    case "$arch" in
        arm64|aarch64)
            for pat in arm64-apple-darwin*-clang aarch64-apple-darwin*-clang clang-* clang; do
                for cand in "$cc_bin"/$pat; do
                    if [ -x "$cand" ]; then real_clang="$cand"; break 2; fi
                done
            done ;;
        *)
            for pat in x86_64-apple-darwin*-clang clang-* clang; do
                for cand in "$cc_bin"/$pat; do
                    if [ -x "$cand" ]; then real_clang="$cand"; break 2; fi
                done
            done ;;
    esac
    if [ -z "$real_clang" ]; then
        echo "  [ERROR] cannot locate the real clang binary in $cc_bin" >&2
        exit 1
    fi
    # triple = clang filename minus the trailing '-clang'
    # (x86_64-apple-darwin21.4-clang -> x86_64-apple-darwin21.4)
    triple="$(basename "$real_clang")"
    triple="${triple%-clang}"
    # macOS major version implied by the clang's darwin version (darwin21 -> 12)
    darwin_num="$(printf '%s' "$triple" | sed -n 's/.*apple-darwin\([0-9][0-9]*\).*/\1/p')"
    macos_major=$(( ${darwin_num:-21} - 9 ))
    [ "$macos_major" -lt 10 ] && macos_major=10

    # SDK: honor override, else auto-detect.
    sdk="${LR_OSX_SDK:-${SDKROOT:-}}"
    if [ -n "$sdk" ]; then
        if [ ! -d "$sdk" ]; then
            echo "  [ERROR] LR_OSX_SDK '$sdk' does not exist." >&2
            echo "          Provide a valid macOS SDK directory (one that does" >&2
            echo "          NOT use the '_Float16' type, e.g. MacOSX13.sdk)." >&2
            exit 1
        fi
        echo "  Using SDK override: $sdk"
    else
        # Candidate SDK root directories.
        sdk_roots="$cc_bin/../SDK ${OSXCROSS_TARGET_DIR:+$OSXCROSS_TARGET_DIR/SDK} $HOME/osxcross/target/SDK /opt/osxcross/SDK"
        best=""; best_ver=""
        for root in $sdk_roots; do
            [ -d "$root" ] || continue
            for d in "$root"/MacOSX*.sdk; do
                [ -d "$d" ] || continue
                v="$(printf '%s' "$d" | sed -n 's/.*MacOSX\([0-9][0-9]*\).*/\1/p')"
                if [ "$v" = "$macos_major" ]; then best="$d"; break 2; fi
                if [ -z "$best_ver" ] || { [ -n "$v" ] && [ "$v" -lt "$best_ver" ]; }; then
                    best="$d"; best_ver="${v:-0}"
                fi
            done
        done
        if [ -z "$best" ]; then
            echo "  [ERROR] no macOS SDK found under SDK roots." >&2
            echo "          Set LR_OSX_SDK=/path/to/MacOSXx.y.sdk" >&2
            exit 1
        fi
        sdk="$best"
        echo "  Auto-detected SDK: $sdk"
    fi

    sdk_block="set(CMAKE_OSX_SYSROOT \"$sdk\" CACHE PATH \"SDK\" FORCE)"
    min_ver="${macos_major}.0"
    echo "  Real clang: $real_clang (triple: $triple, min-ver: $min_ver)"
    cc_wrapper="$bdir/cc_wrapper.sh"
    cat > "$cc_wrapper" <<WRAP
#!/bin/sh
exec "$real_clang" -target $triple -isysroot "$sdk" -mmacosx-version-min=$min_ver -B"$(dirname "$real_clang")" "\$@"
WRAP
    chmod +x "$cc_wrapper"
    # CMake requires an absolute path for CMAKE_C_COMPILER.
    cmake_cc="$ROOT/$cc_wrapper"

    cat > "$tc" <<EOF
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_C_COMPILER $cmake_cc)
set(CMAKE_AR "$ar_tool" CACHE FILEPATH "osxcross archiver" FORCE)
set(CMAKE_RANLIB "$ranlib_tool" CACHE FILEPATH "osxcross ranlib" FORCE)
set(CMAKE_OSX_ARCHITECTURES $arch CACHE STRING "" FORCE)
$sdk_block
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
