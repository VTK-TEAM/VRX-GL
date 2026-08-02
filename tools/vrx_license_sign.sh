#!/usr/bin/env bash
# vrx_license_sign.sh — підписати особистість картки приватним ключем вендора.
#
# Вхід: hex канонічного повідомлення (рядок "MESSAGE ...." з `vrx_gl -sdinfo`,
#        або самі hex-символи).
# Вихід: 64 байти підпису Ed25519 (у stdout як hex; --raw дає бінарні байти).
#
# Приватний ключ НІКОЛИ не залишає машину вендора. Станція його не бачить.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIV="${VRX_PRIV_KEY:-$HERE/vendor_ed25519_private.pem}"

usage() { echo "usage: $0 [--raw] <MESSAGE-hex>" >&2; exit 2; }

RAW=0
if [[ "${1:-}" == "--raw" ]]; then RAW=1; shift; fi
[[ $# -ge 1 ]] || usage
[[ -f "$PRIV" ]] || { echo "немає приватного ключа: $PRIV" >&2; exit 1; }

# приймаємо і "MESSAGE abcd..", і просто "abcd.."
HEX="$*"
HEX="${HEX#MESSAGE }"
HEX="${HEX// /}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# hex -> бінарне повідомлення
echo -n "$HEX" | xxd -r -p > "$TMP/msg.bin"

# Ed25519 підписує повідомлення цілком (-rawin), без попереднього гешу.
openssl pkeyutl -sign -inkey "$PRIV" -rawin -in "$TMP/msg.bin" -out "$TMP/sig.bin"

SZ=$(stat -c %s "$TMP/sig.bin")
[[ "$SZ" == "64" ]] || { echo "підпис не 64 байти ($SZ)" >&2; exit 1; }

if [[ "$RAW" == "1" ]]; then
    cat "$TMP/sig.bin"
else
    xxd -p -c 64 "$TMP/sig.bin"
fi
