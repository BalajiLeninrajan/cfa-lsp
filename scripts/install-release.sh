#!/usr/bin/env bash
# Usage: scripts/install-release.sh [VERSION]   (or make install-release [V=VERSION])
#
# Downloads a published release (the latest by default), checks its SHA-256
# and unpacks it into PREFIX (default ~/.local): bin/cfa-lsp,
# libexec/cfa-lsp/cfa-cpp and the licenses in share/doc/cfa-lsp. Needs curl,
# not gh or a build. The binaries are built on Ubuntu 24.04 for x86-64.
set -euo pipefail

die() { echo "install-release: $*" >&2; exit 1; }

repo=${CFA_LSP_REPO:-BalajiLeninrajan/cfa-lsp}
prefix=${PREFIX:-$HOME/.local}
asset=cfa-lsp-linux-x86_64.tar.gz

[ "$(uname -s)-$(uname -m)" = Linux-x86_64 ] || die "releases are built for Linux x86-64 only; build from source"
if [ -n "${1:-}" ]; then
	url=https://github.com/$repo/releases/download/v${1#v}
else
	url=https://github.com/$repo/releases/latest/download
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/$asset" "$url/$asset" || die "can't download $url/$asset"
curl -fsSL -o "$tmp/$asset.sha256" "$url/$asset.sha256" || die "can't download $url/$asset.sha256"
( cd "$tmp" && sha256sum --quiet -c "$asset.sha256" ) || die "checksum mismatch"

mkdir -p "$prefix"
# --no-overwrite-dir keeps the mode and owner of directories that already
# exist, such as ~/.local/bin.
tar -xzf "$tmp/$asset" -C "$prefix" --no-overwrite-dir
echo "Installed $("$prefix/bin/cfa-lsp" --version) in $prefix"
