#!/bin/sh
# Lume installer — downloads the prebuilt binary for this platform and puts it on
# your PATH. Also used by `lume update` (it self-replaces in place).
#
#   curl -fsSL https://raw.githubusercontent.com/uixova/lume/main/install.sh | sh
#
# Env:
#   LUME_CHANNEL   stable (default) | latest    which release to fetch
#   LUME_INSTALL   install dir (default: $HOME/.local/bin)
set -eu

REPO="uixova/lume"
CHANNEL="${LUME_CHANNEL:-stable}"
INSTALL_DIR="${LUME_INSTALL:-$HOME/.local/bin}"

say()  { printf '%s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "missing required tool: $1"; }

need curl
need uname

# --- detect platform -> release asset name -------------------------------------
os="$(uname -s)"; arch="$(uname -m)"
case "$os" in
    Linux)  plat="linux" ;;
    Darwin) plat="macos" ;;
    MINGW*|MSYS*|CYGWIN*) die "on Windows use install.ps1 (PowerShell)" ;;
    *) die "unsupported OS: $os" ;;
esac
case "$arch" in
    x86_64|amd64) cpu="x64" ;;
    aarch64|arm64) cpu="arm64" ;;
    *) die "unsupported architecture: $arch" ;;
esac
asset="lume-${plat}-${cpu}"

# --- resolve the release tag ---------------------------------------------------
api="https://api.github.com/repos/${REPO}/releases"
if [ "$CHANNEL" = "latest" ]; then
    # newest release including pre-releases
    tag="$(curl -fsSL "$api" | grep '"tag_name"' | head -n1 | cut -d'"' -f4)"
else
    tag="$(curl -fsSL "${api}/latest" | grep '"tag_name"' | head -n1 | cut -d'"' -f4)"
fi
[ -n "${tag:-}" ] || die "could not find a release (channel: $CHANNEL). Is a release published yet?"

url="https://github.com/${REPO}/releases/download/${tag}/${asset}"
sum_url="${url}.sha256"

say "Lume ${tag} — downloading ${asset}..."
tmp="$(mktemp)"
curl -fsSL "$url" -o "$tmp" || die "download failed: $url"

# --- verify checksum if the release publishes one ------------------------------
if curl -fsSL "$sum_url" -o "${tmp}.sha256" 2>/dev/null; then
    expected="$(cut -d' ' -f1 < "${tmp}.sha256")"
    if command -v sha256sum >/dev/null 2>&1; then
        actual="$(sha256sum "$tmp" | cut -d' ' -f1)"
    else
        actual="$(shasum -a 256 "$tmp" | cut -d' ' -f1)"
    fi
    [ "$expected" = "$actual" ] || die "checksum mismatch (expected $expected, got $actual)"
    say "checksum verified."
fi

# --- install -------------------------------------------------------------------
mkdir -p "$INSTALL_DIR"
chmod +x "$tmp"
mv "$tmp" "$INSTALL_DIR/lume"
rm -f "${tmp}.sha256" 2>/dev/null || true

say "Installed lume ${tag} to ${INSTALL_DIR}/lume"
case ":$PATH:" in
    *":$INSTALL_DIR:"*) : ;;
    *) say ""
       say "  Add it to your PATH — append this to ~/.bashrc or ~/.zshrc:"
       say "    export PATH=\"$INSTALL_DIR:\$PATH\"" ;;
esac
say "Run 'lume --version' to check, or 'lume' for the REPL."
