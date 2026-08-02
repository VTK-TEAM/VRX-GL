#!/usr/bin/env python3
"""
Офлайн-підписувач ліцензії станції VRX-GL.

Аналог license_sign.py з камери, але під станцію: підпис Ed25519 (а не
ECDSA-P256), і не окремий файл, а 64 байти, вписані у файл-приманку
osd_glyph_kern.bin на фіксованому зміщенні. Перевіряє цей підпис
license/ed25519.cpp зашитим публічним ключем.

SIG_OFFSET/SIG_LEN мусять збігатися з license/license_store.hpp.

Приватний ключ (vendor_ed25519_private.pem) НІКОЛИ не потрапляє на
станцію й НЕ комітиться — тримай його офлайн. Втрата = будь-хто зможе
активувати будь-яку картку; втрата можливості підписувати = згенеруй нову
пару (tools/vrx_license_keygen.sh) і зашей нову публічну половину в
license/pubkey.h — раніше активовані станції це не зачіпає.

Requires: pip install cryptography

Usage:
    vrx_sign.py <MESSAGE-hex> <vendor_private.pem> [out.sig]

<MESSAGE-hex> — рядок після "MESSAGE " з виводу `vrx_gl -sdinfo`.
"""
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey, Ed25519PublicKey)

# = license/license_store.hpp
SIG_OFFSET = 11024
SIG_LEN = 64


def load_private_key(path):
    with open(path, "rb") as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    if not isinstance(key, Ed25519PrivateKey):
        raise ValueError("ключ не Ed25519: %s" % type(key).__name__)
    return key


def public_raw(key):
    """32 сирі байти публічного ключа — рівно те, що в license/pubkey.h."""
    return key.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def sign_message(msg, key):
    """msg: bytes -> підпис 64 байти."""
    sig = key.sign(msg)
    if len(sig) != SIG_LEN:
        raise ValueError("підпис не %d байтів: %d" % (SIG_LEN, len(sig)))
    return sig


def verify_message(msg, sig, pub_raw):
    """True, якщо sig дійсний для msg під публічним ключем (32 сирі байти)."""
    try:
        Ed25519PublicKey.from_public_bytes(bytes(pub_raw)).verify(bytes(sig), bytes(msg))
        return True
    except Exception:
        return False


def message_from_hex(hexstr):
    return bytes.fromhex(hexstr.strip().replace("MESSAGE", "").strip())


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    msg = message_from_hex(sys.argv[1])
    key = load_private_key(sys.argv[2])
    out_path = sys.argv[3] if len(sys.argv) > 3 else "license.sig"

    sig = sign_message(msg, key)
    with open(out_path, "wb") as f:
        f.write(sig)
    print("підпис (%d байтів) записано: %s" % (len(sig), out_path))
    print("вписати на станції: dd of=osd_glyph_kern.bin bs=1 seek=%d "
          "conv=notrunc  (з цього файлу)" % SIG_OFFSET)


if __name__ == "__main__":
    main()
