#!/usr/bin/env python3
# Локальна активація ЦІЄЇ станції: читає картку через `vrx_gl -sdinfo`,
# підписує приватним ключем вендора й вписує 64 байти підпису у приманку.
# Для активації по SSH є tools/vrx_activate.sh — цей файл лише для тесту
# на самій станції.
import binascii
import os
import subprocess
import sys

OFFSET = 11024
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRIV = os.path.join(ROOT, "tools", "vendor_ed25519_private.pem")
STORE = os.path.join(ROOT, "osd_glyph_kern.bin")
VRX = os.path.join(ROOT, "build", "vrx_gl")


def main():
    out = subprocess.check_output([VRX, "-sdinfo"], text=True)
    msg_hex = ""
    for line in out.splitlines():
        if line.startswith("MESSAGE "):
            msg_hex = line.split()[1]
    if not msg_hex:
        print("картки немає або корінь не на SD:\n" + out, file=sys.stderr)
        return 1
    msg = binascii.unhexlify(msg_hex)

    sig = subprocess.run(
        ["openssl", "pkeyutl", "-sign", "-inkey", PRIV, "-rawin"],
        input=msg, stdout=subprocess.PIPE, check=True).stdout
    if len(sig) != 64:
        print("підпис не 64 байти:", len(sig), file=sys.stderr)
        return 1

    with open(STORE, "r+b") as f:
        f.seek(OFFSET)
        f.write(sig)
    print("активовано: підпис (64 б) вписано на зміщення", OFFSET)
    return 0


if __name__ == "__main__":
    sys.exit(main())
