#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
platform="${1:?platform required}"
case "${2:-}" in --dry-run) export SETUP_DRY_RUN=1 ;; '') ;; *) echo "Unknown option: $2" >&2; exit 2 ;; esac
source "$ROOT/scripts/setup_packages.sh"
case "$platform" in
    mac) setup_packages mac mbedtls@3 nghttp2 zlib bzip2 openjpeg libpng jpeg-turbo libuv libevent mpdecimal xz zstd ;;
    linux) setup_packages linux libnghttp2-dev zlib1g-dev libopenjp2-7-dev libpng-dev libjpeg-dev libturbojpeg0-dev libuv1-dev libevent-dev liblzma-dev libzstd-dev libbz2-dev ;;
    windows) setup_packages windows @toolchain@-mbedtls @toolchain@-nghttp2 @toolchain@-zlib @toolchain@-openjpeg2 @toolchain@-libpng @toolchain@-libjpeg-turbo @toolchain@-libuv @toolchain@-libevent @toolchain@-mpdecimal @toolchain@-xz @toolchain@-zstd @toolchain@-bzip2 ;;
    *) echo "Unknown setup platform: $platform" >&2; exit 2 ;;
esac
if [ "$platform" = linux ]; then
    if [ "${SETUP_DRY_RUN:-0}" = 1 ]; then echo "python3 $ROOT/scripts/setup_linux_sources.py"
    else python3 "$ROOT/scripts/setup_linux_sources.py"; fi
fi
if [ "${SETUP_DRY_RUN:-0}" = 1 ]; then
    echo "make -C $ROOT vendor-deps curl-deps native-deps"
else
    make -C "$ROOT" vendor-deps curl-deps native-deps
fi
