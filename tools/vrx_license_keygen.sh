#!/usr/bin/env bash
# vrx_license_keygen.sh — згенерувати НОВУ ключову пару вендора.
#
# Запускати РАЗ. Приватний ключ лишається тут (у .gitignore), 32 байти
# публічного треба вписати в license/pubkey.h. Перезапуск = усі раніше
# видані ліцензії стають недійсними.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIV="$HERE/vendor_ed25519_private.pem"

if [[ -f "$PRIV" ]]; then
    echo "приватний ключ уже існує: $PRIV" >&2
    echo "видали його вручну, якщо справді хочеш перевипустити все." >&2
    exit 1
fi

openssl genpkey -algorithm ed25519 -out "$PRIV"
chmod 600 "$PRIV"
openssl pkey -in "$PRIV" -pubout -out "$HERE/vendor_ed25519_public.pem"

echo "приватний ключ: $PRIV"
echo
echo "встав ці 32 байти в license/pubkey.h -> kVendorPublicKey:"
openssl pkey -in "$PRIV" -pubout -outform DER 2>/dev/null | \
    tail -c 32 | xxd -i
