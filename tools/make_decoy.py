#!/usr/bin/env python3
"""Генератор файлу-приманки для сховку ліцензії.

Наповнює файл байтами, статистично схожими на бінарний ресурс станції:
беремо справжній osd_glyph_info.bin, ріжемо на дрібні шматки й
перемішуємо їх. Виходить щось, що виглядає як ще одна таблиця гліфів, а
не як контейнер для ключа. Підпис ліцензії (64 байти) вписується пізніше
активаційним скриптом на фіксованому зміщенні; тут на його місце кладемо
такий самий "шум", щоб неактивований файл не відрізнявся від активованого
нічим, крім самих 64 байтів.

Зміщення й довжина мусять збігатися з license/license_store.hpp.
"""
import os
import random
import sys

SIG_OFFSET = 11024   # = license/license_store.hpp kSignatureOffset
SIG_LEN = 64
TOTAL = 24000        # приблизно як osd_glyph_info.bin (21904)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    src = os.path.join(root, "osd_glyph_info.bin")
    out = os.path.join(root, "osd_glyph_kern.bin")

    seed = bytearray()
    if os.path.exists(src):
        data = open(src, "rb").read()
        # дрібні шматки й перемішати — щоб не читалось як копія
        chunks = [data[i:i + 17] for i in range(0, len(data), 17)]
        random.shuffle(chunks)
        for c in chunks:
            seed += c

    # добити/обрізати до потрібного розміру псевдовипадковими байтами
    while len(seed) < TOTAL:
        seed += bytes(random.randrange(256) for _ in range(64))
    buf = bytearray(seed[:TOTAL])

    # на місце майбутнього підпису — теж "шум": неактивований файл має
    # виглядати так само, як активований
    for i in range(SIG_LEN):
        buf[SIG_OFFSET + i] = random.randrange(256)

    open(out, "wb").write(buf)
    print("приманку записано:", out, len(buf), "байт; зміщення підпису", SIG_OFFSET)


if __name__ == "__main__":
    sys.exit(main())
