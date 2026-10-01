#!/bin/sh
# shellcheck shell=sh
#
# astral-cli POSIX installer (Linux/macOS).
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/The-Astral-Express-Family/astral-cli/main/scripts/install.sh | sh
#   ASTRAL_VERSION=v0.2.1 sh install.sh        # pin a version ("v" optional)
#   ASTRAL_INSTALL_DIR=/custom/bin sh install.sh
#
# Environment overrides (testing / mirroring):
#   ASTRAL_VERSION       pin a version, with or without leading "v"
#   ASTRAL_INSTALL_DIR   install directory (default: $HOME/.local/bin)
#   ASTRAL_API_BASE      GitHub API base (default: https://api.github.com)
#   ASTRAL_DOWNLOAD_BASE download base (default: https://github.com)
#
# Downloads the release archive astral-<version>-<target>.tar.gz and
# SHA256SUMS.txt from $ASTRAL_DOWNLOAD_BASE/$REPO/releases/download/<tag>/,
# verifies the archive checksum, and installs the binary as "astral".

set -eu

REPO="The-Astral-Express-Family/astral-cli"
API_BASE="${ASTRAL_API_BASE:-https://api.github.com}"
DOWNLOAD_BASE="${ASTRAL_DOWNLOAD_BASE:-https://github.com}"

err() { printf 'install.sh: %s\n' "$*" >&2; }
log() { printf '==> %s\n' "$*" >&2; }

tmp=""
cleanup() {
    if [ -n "$tmp" ]; then
        rm -rf "$tmp"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if ! command -v curl >/dev/null 2>&1; then
    err "curl is required but was not found in PATH"
    exit 1
fi

# 1. uname -s / -m -> release target triplet
os=$(uname -s)
arch=$(uname -m)
target=""
case "$os" in
    Linux)
        case "$arch" in
            x86_64)        target="linux-x64" ;;
            aarch64|arm64) target="linux-arm64" ;;
        esac
        ;;
    Darwin)
        case "$arch" in
            x86_64)        target="macos-x64" ;;
            aarch64|arm64) target="macos-arm64" ;;
        esac
        ;;
esac
if [ -z "$target" ]; then
    err "unsupported platform: $os/$arch"
    err "supported platforms:"
    err "  linux-x64    (Linux x86_64)"
    err "  linux-arm64  (Linux aarch64/arm64)"
    err "  macos-x64    (Darwin x86_64)"
    err "  macos-arm64  (Darwin arm64)"
    exit 1
fi

# 2. version: $ASTRAL_VERSION (optional leading "v" stripped) or latest release
version=""
if [ -n "${ASTRAL_VERSION:-}" ]; then
    version=$ASTRAL_VERSION
    case "$version" in
        v*) version=${version#v} ;;
    esac
else
    log "querying latest release from $API_BASE"
    release_json=$(curl -fsSL "$API_BASE/repos/$REPO/releases/latest" 2>/dev/null) || {
        err "failed to query $API_BASE/repos/$REPO/releases/latest"
        err "(network error, rate limit, or no release published yet)"
        exit 1
    }
    version=$(printf '%s\n' "$release_json" | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p')
    case "$version" in
        v*) version=${version#v} ;;
    esac
    if [ -z "$version" ]; then
        err "could not parse tag_name from the latest-release API response"
        exit 1
    fi
fi

case "$version" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) err "invalid version '$version' (ASTRAL_VERSION accepts 0.2.1 or v0.2.1)"; exit 1 ;;
esac
tag="v$version"

# 3. download archive + checksums into a temp dir
tmp=$(mktemp -d)
url="$DOWNLOAD_BASE/$REPO/releases/download/$tag"
archive="astral-$version-$target.tar.gz"
log "downloading $url/$archive"
curl -fsSL "$url/$archive" -o "$tmp/$archive" || {
    err "download failed: $url/$archive"
    exit 1
}
log "downloading $url/SHA256SUMS.txt"
curl -fsSL "$url/SHA256SUMS.txt" -o "$tmp/SHA256SUMS.txt" || {
    err "download failed: $url/SHA256SUMS.txt"
    exit 1
}

# 4. verify checksum (sha256sum first, shasum -a 256 for macOS)
if ! command -v sha256sum >/dev/null 2>&1 && ! command -v shasum >/dev/null 2>&1; then
    err "need sha256sum (Linux) or shasum (macOS) to verify checksums"
    exit 1
fi
expected=$(awk -v f="$archive" '$2 == f { print $1; exit }' "$tmp/SHA256SUMS.txt")
if [ -z "$expected" ]; then
    err "$archive is not listed in SHA256SUMS.txt"
    exit 1
fi
if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$tmp/$archive" | awk '{print $1}')
else
    actual=$(shasum -a 256 "$tmp/$archive" | awk '{print $1}')
fi
if [ "$actual" != "$expected" ]; then
    err "checksum mismatch for $archive"
    err "  expected: $expected"
    err "  actual:   $actual"
    exit 1
fi
log "checksum OK"

# 5. extract and install
tar -xzf "$tmp/$archive" -C "$tmp"
src_bin="$tmp/astral-$version-$target/astral"
if [ ! -f "$src_bin" ]; then
    err "archive did not contain astral-$version-$target/astral"
    exit 1
fi
install_dir=${ASTRAL_INSTALL_DIR:-$HOME/.local/bin}
mkdir -p "$install_dir"
install -m 0755 "$src_bin" "$install_dir/astral"

# 6. PATH hint (informational only; rc files are never edited)
case ":$PATH:" in
    *":$install_dir:"*) ;;
    *)
        printf '\nNOTE: %s is not in your PATH. Add it with:\n  export PATH="%s:$PATH"\n' "$install_dir" "$install_dir"
        ;;
esac

# 7. success
printf 'astral %s installed to %s\n' "$tag" "$install_dir"
