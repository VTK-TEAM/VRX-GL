#!/usr/bin/env bash
# vrx_activate.sh — активувати станцію по SSH.
#
# Робить три речі й нічого більше:
#   1. питає в станції особистість картки  (vrx_gl -sdinfo)
#   2. підписує її ЛОКАЛЬНО приватним ключем (vrx_license_sign.sh)
#   3. вписує 64 байти підпису у файл-приманку на станції, на зміщенні
#
# Приватний ключ лишається тут. На станцію їде лише готовий підпис.
#
# usage: vrx_activate.sh user@station [/шлях/до/каталогу/станції]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIGN="$HERE/vrx_license_sign.sh"

HOST="${1:-}"
REMOTE_DIR="${2:-/opt/vrx}"
[[ -n "$HOST" ]] || { echo "usage: $0 user@station [remote_dir]" >&2; exit 2; }

SIG_OFFSET=11024          # = license/license_store.hpp kSignatureOffset
STORE="osd_glyph_kern.bin"

echo "[1/3] читаю картку на $HOST ..."
INFO="$(ssh "$HOST" "cd '$REMOTE_DIR' && ./vrx_gl -sdinfo")"
echo "$INFO"
MSG="$(printf '%s\n' "$INFO" | awk '/^MESSAGE /{print $2}')"
[[ -n "$MSG" ]] || { echo "станція не віддала MESSAGE — картки немає або корінь не на SD" >&2; exit 1; }

echo "[2/3] підписую локально ..."
SIG_HEX="$("$SIGN" "$MSG")"
echo "  підпис: $SIG_HEX"

echo "[3/3] вписую підпис у $REMOTE_DIR/$STORE на зміщенні $SIG_OFFSET ..."
# seek у байтах, conv=notrunc — не чіпаємо решту файлу-приманки.
printf '%s' "$SIG_HEX" | xxd -r -p | \
    ssh "$HOST" "dd of='$REMOTE_DIR/$STORE' bs=1 seek=$SIG_OFFSET conv=notrunc status=none"

echo "готово. перезапусти станцію (наглядач підхопить) і перевір екран."
