#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ./package_linux.sh [--no-lib32]

Build a Linux release tarball containing:
- sglrenderer at the archive root
- 64-bit Linux client libraries in lib/
- optional 32-bit Linux client libraries in lib32/
EOF
}

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WITH_LIB32=1

while (($# > 0)); do
    case "$1" in
        --no-lib32)
            WITH_LIB32=0
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'ERROR: unknown option: %s\n\n' "$1" >&2
            usage >&2
            exit 1
            ;;
    esac
    shift
done

VERSION="$(sed -nE 's/^[[:space:]]*project\(sharedgl VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES C\)$/\1/p' "$ROOT_DIR/CMakeLists.txt")"
if [[ -z "$VERSION" ]]; then
    printf 'ERROR: could not extract version from CMakeLists.txt\n' >&2
    exit 1
fi

BUILD64_DIR="$ROOT_DIR/build-package-linux64"
BUILD32_DIR="$ROOT_DIR/build-package-linux32"
DIST_DIR="$ROOT_DIR/dist"
PACKAGE_NAME="sharedgl-v${VERSION}-linux"
STAGE_DIR="$DIST_DIR/$PACKAGE_NAME"
ARCHIVE_PATH="$DIST_DIR/$PACKAGE_NAME.tar.gz"

resolve_output_dir() {
    local build_dir="$1"
    local marker="$2"

    if [[ -e "$build_dir/$marker" ]]; then
        printf '%s\n' "$build_dir"
        return 0
    fi

    if [[ -e "$build_dir/Release/$marker" ]]; then
        printf '%s\n' "$build_dir/Release"
        return 0
    fi

    printf 'ERROR: could not find %s in %s\n' "$marker" "$build_dir" >&2
    return 1
}

stage_client_dir() {
    local output_dir="$1"
    local dest_dir="$2"

    mkdir -p "$dest_dir"
    cp -a \
        "$output_dir/libGL.so" \
        "$output_dir/libGL.so.1" \
        "$output_dir/libOpenGL.so.0" \
        "$output_dir/libGLX.so.0" \
        "$dest_dir/"
}

printf 'Packaging SharedGL v%s\n' "$VERSION"

rm -rf "$BUILD64_DIR" "$STAGE_DIR"
mkdir -p "$DIST_DIR"

cmake -S "$ROOT_DIR" -B "$BUILD64_DIR"
cmake --build "$BUILD64_DIR" --target sglrenderer sharedgl-core --config Release

OUTPUT64_DIR="$(resolve_output_dir "$BUILD64_DIR" "sglrenderer")"

mkdir -p "$STAGE_DIR"
install -m 0755 "$OUTPUT64_DIR/sglrenderer" "$STAGE_DIR/sglrenderer"
stage_client_dir "$OUTPUT64_DIR" "$STAGE_DIR/lib"
install -m 0644 "$ROOT_DIR/README.md" "$STAGE_DIR/README.md"
install -m 0644 "$ROOT_DIR/LICENSE" "$STAGE_DIR/LICENSE"

if (( WITH_LIB32 )); then
    rm -rf "$BUILD32_DIR"
    cmake -S "$ROOT_DIR" -B "$BUILD32_DIR" -DLINUX_LIB32=ON
    cmake --build "$BUILD32_DIR" --target sharedgl-core --config Release

    OUTPUT32_DIR="$(resolve_output_dir "$BUILD32_DIR" "libGL.so.1")"
    stage_client_dir "$OUTPUT32_DIR" "$STAGE_DIR/lib32"
fi

rm -f "$ARCHIVE_PATH"
tar -C "$DIST_DIR" -czf "$ARCHIVE_PATH" "$PACKAGE_NAME"
rm -rf "$BUILD64_DIR" "$BUILD32_DIR"

printf 'Done: %s\n' "$ARCHIVE_PATH"
