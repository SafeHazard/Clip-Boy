#!/usr/bin/env bash
# sign_release_paste.sh — op-free fallback: sign release/SHA256SUMS with a PASTED
# minisign private key that lives only in RAM (+ a shredded temp) for the run.
#
# Run in an INTERACTIVE Git Bash shell (it needs the paste + the minisign
# passphrase prompt — those hang in a non-TTY). After make_release_bins.sh.
#
#   bash scripts/sign_release_paste.sh
#
# Same output as sign_release.sh: release/SHA256SUMS.minisig (legacy Ed25519,
# LF-normalized), verified against keys/clipboy-release.pub.
set -e
# Resolve the real path so this works when invoked via the ~/ symlink too.
SELF="$(readlink -f "$0" 2>/dev/null || echo "$0")"
SCRIPT_DIR="$(cd "$(dirname "$SELF")" && pwd)"
REL="$(cd "$SCRIPT_DIR/.." && pwd)/release"
PUB="$(cd "$SCRIPT_DIR/.." && pwd)/keys/clipboy-release.pub"

command -v minisign >/dev/null 2>&1 || { echo "ERROR: minisign not installed"; exit 1; }
[[ -f "$REL/SHA256SUMS" ]] || { echo "ERROR: $REL/SHA256SUMS missing — run make_release_bins.sh first"; exit 1; }
cd "$REL"

# Autonomous path: if the gitignored creds sidecar is present, use it (no paste,
# no prompt) so an agent can sign unattended. Otherwise fall back to the interactive
# paste. sign_creds.local holds the passwordless secret key base64 and is NEVER
# committed (see .gitignore) -- it is the ONLY place the live key lives on disk.
CREDS="$SCRIPT_DIR/sign_creds.local"
if [[ -f "$CREDS" ]]; then
    echo "Signing with autonomous creds ($CREDS) -- no paste needed."
    # shellcheck disable=SC1090
    source "$CREDS"
    KEYB64="${CB_SIGN_SECKEY_B64:-}"
    [[ -n "$KEYB64" ]] || { echo "ERROR: $CREDS present but CB_SIGN_SECKEY_B64 is empty"; exit 1; }
else
    echo "Paste the Clip-Boy minisign secret-key BASE64 (the long 2nd line only),"
    echo "then press Enter and Ctrl-D:"
    RAW="$(cat)"
    # Keep only the base64 body: drop an 'untrusted comment:' line if it's pasted too,
    # plus blanks; take the last remaining line.
    KEYB64="$(printf '%s\n' "$RAW" | grep -v '^untrusted comment:' | grep -vE '^[[:space:]]*$' | tail -1)"
    unset RAW
    [[ -n "$KEYB64" ]] || { echo "ERROR: no base64 key line found"; exit 1; }
fi

TMPKEY="$(mktemp)"
cleanup() { shred -u "$TMPKEY" 2>/dev/null || rm -f "$TMPKEY"; unset KEYB64 PW 2>/dev/null || true; }
trap cleanup EXIT
# minisign needs a 2-line key file; line 1 is an 'untrusted comment' NOT bound to the
# key, so a placeholder is fine — only the base64 matters.
printf 'untrusted comment: minisign encrypted secret key\n%s\n' "$KEYB64" > "$TMPKEY"
unset KEYB64

echo "--- signing (encrypted key -> passphrase prompt; passwordless key -> none) ---"
# -l = legacy / pure Ed25519 (algo tag 'Ed') so the browser flasher (WebCrypto,
# Ed25519 only, no Blake2b) can verify. MANDATORY.
minisign -S -l -s "$TMPKEY" -m SHA256SUMS


# LF-normalize: Windows minisign writes the .minisig CRLF, but the flasher verifies
# the LF git blob — a CRLF .minisig fails over the wire (bit us live 2026-06-27).
tr -d '\r' < SHA256SUMS.minisig > .ms.tmp && mv .ms.tmp SHA256SUMS.minisig

echo "--- verify against the pinned public key ---"
minisign -Vm SHA256SUMS -p "$PUB"

echo "--- CR check (want 0 each) ---"
for f in SHA256SUMS SHA256SUMS.minisig flash-spec.json; do
    echo "  $f: $(tr -cd '\r' < "$f" | wc -c) CR"
done
echo "DONE — release/SHA256SUMS.minisig written + verified. The agent will commit + push."
