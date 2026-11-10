#!/usr/bin/env bash
# Shared platform package operations for the three project setup scripts.
setup_packages() {
    local platform="$1"; shift
    local package prefix
    case "$platform" in
        mac)
            for package in "$@"; do
                if [ "${SETUP_DRY_RUN:-0}" = 1 ]; then echo "brew install $package"; continue; fi
                brew list --formula "$package" >/dev/null 2>&1 || brew install "$package" || return 1
            done ;;
        linux)
            for package in "$@"; do
                if [ "${SETUP_DRY_RUN:-0}" = 1 ]; then echo "sudo apt install -y $package"; continue; fi
                [ "$(dpkg-query -W -f='${Status}' "$package" 2>/dev/null || true)" = 'install ok installed' ] || sudo apt install -y "$package" || return 1
            done ;;
        windows)
            case "${MSYSTEM:-}" in
                CLANG64) prefix=mingw-w64-clang-x86_64 ;;
                MINGW64) prefix=mingw-w64-x86_64 ;;
                *) echo 'Use an MSYS2 CLANG64 or MINGW64 shell.' >&2; return 1 ;;
            esac
            for package in "$@"; do
                package="${package/@toolchain@/$prefix}"
                if [ "${SETUP_DRY_RUN:-0}" = 1 ]; then echo "pacman -S --needed --noconfirm $package"; continue; fi
                pacman -Qq "$package" >/dev/null 2>&1 || pacman -S --needed --noconfirm "$package" || return 1
            done ;;
        *) echo "Unknown setup platform: $platform" >&2; return 1 ;;
    esac
}
