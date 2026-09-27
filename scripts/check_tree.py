#!/usr/bin/env python3
"""Перевірка цілісності робочого дерева.

ЧОМУ ЦЕ ПОТРІБНО ОКРЕМО ВІД `git status`. git довіряє кешу stat: якщо
розмір і mtime файлу не змінились, вміст він не перечитує. Пошкодження на
рівні носія міняє саме вміст, лишаючи розмір і час незмінними — і
`git status` каже "чисто" над побитим файлом.

Так уже було: control/camera_api.hpp і .cpp лежали заповнені байтами 0xFF
того самого розміру, з початковим mtime, а дерево вважалося чистим. Файли
знайшлись лише тоді, коли їх спробували прочитати.

ЩО РОБИТЬ. Перехешовує КОЖЕН відслідковуваний файл власними силами (хеш
блоба git — це sha1 від "blob <довжина>\\0" плюс вміст, тобто git для
цього запускати не треба) і порівнює з HEAD.

Розбіжність сама по собі нормальна — це незакомічена правка. Тому
окремо називаються ті, що НЕ виглядають правкою:

  - суцільні 0xFF (підпис "інод вижив, блоки ні");
  - NUL-байти у файлі, який у HEAD був текстовим;
  - файл зник, хоч git його відслідковує.

Код виходу 1 — є підозра на пошкодження. Звичайні правки код виходу не
міняють: інакше скрипт був би непридатним у роботі.
"""

import hashlib
import os
import subprocess
import sys


def blob_sha(data: bytes) -> str:
    h = hashlib.sha1()
    h.update(b"blob %d\0" % len(data))
    h.update(data)
    return h.hexdigest()


def head_tree(root: str) -> dict:
    out = subprocess.run(["git", "-C", root, "ls-tree", "-r", "-z", "HEAD"],
                         capture_output=True)
    if out.returncode != 0:
        sys.stderr.write("не вдалося прочитати HEAD — це взагалі репозиторій?\n")
        sys.exit(2)
    tree = {}
    for rec in out.stdout.split(b"\0"):
        if not rec:
            continue
        meta, _, path = rec.partition(b"\t")
        parts = meta.split()
        if len(parts) < 3 or parts[1] != b"blob":
            continue            # підмодулі та інше — не наша справа
        tree[path.decode("utf-8", "surrogateescape")] = parts[2].decode()
    return tree


def looks_corrupt(data: bytes, was_text: bool) -> str:
    if data and data.count(b"\xff") * 2 > len(data):
        return "суцільні 0xFF"
    if was_text and b"\0" in data:
        return "NUL-байти в текстовому файлі"
    return ""


def main() -> int:
    root = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                          capture_output=True, text=True).stdout.strip()
    if not root:
        sys.stderr.write("не в репозиторії\n")
        return 2

    tree = head_tree(root)
    bad, gone, edited = [], [], []

    for path, want in tree.items():
        full = os.path.join(root, path)
        if os.path.islink(full):
            continue
        if not os.path.exists(full):
            gone.append(path)
            continue
        try:
            with open(full, "rb") as f:
                data = f.read()
        except OSError as e:
            bad.append((path, "не читається: %s" % e))
            continue

        if blob_sha(data) == want:
            continue

        # Текстовість беремо з HEAD, а не з файлу: у побитому файлі її вже
        # немає, і питання саме в тому, чи вона там була.
        was_text = b"\0" not in subprocess.run(
            ["git", "-C", root, "cat-file", "blob", want],
            capture_output=True).stdout
        why = looks_corrupt(data, was_text)
        (bad if why else edited).append((path, why) if why else path)

    n = len(tree)
    if not bad and not gone:
        if edited:
            print("цілісність: %d файлів ok, %d змінено (правки, не пошкодження)"
                  % (n - len(edited), len(edited)))
        else:
            print("цілісність: %d файлів, усі збігаються з HEAD" % n)
        return 0

    print("ЦІЛІСНІСТЬ: ЗНАЙДЕНО ПРОБЛЕМИ (перевірено %d файлів)" % n)
    for path, why in bad:
        print("  ПОШКОДЖЕНО  %s  — %s" % (path, why))
    for path in gone:
        print("  ЗНИК        %s" % path)
    if edited:
        print("  (ще %d змінено — це правки, не пошкодження)" % len(edited))
    print()
    print("Відновити побите з HEAD (git checkout сам НЕ спрацює — за кешем")
    print("stat він вважає файл незміненим, тому спершу видалити):")
    for path, _ in bad:
        print("    rm -f '%s' && git checkout -- '%s'" % (path, path))
    return 1


if __name__ == "__main__":
    sys.exit(main())
