#!/usr/bin/env bash
# gen_version.sh — версія збірки у форматі Vyy.xxxx.bbbb.
#
# Викликається CMake ЩОБІЛД (ціль vrx_version, ALL). Пише два артефакти:
#   <out.h>                 — #define VRX_VERSION "..."  для бінарника
#   <root>/version/version.txt — той самий рядок для оновлювача
#                                (він порівнює саме version.txt)
#
# Формат Vyy.xxxx.bbbb:
#   yy   — стала мажорна версія. Лежить у version/major.txt, руками.
#          Зараз 01. Міняється лише коли скажуть.
#   xxxx — днів від 2019-01-05 (5 січня 2019). Росте самé з календарем.
#   bbbb — лічильник білдів. ++ на кожен білд, НЕ скидається. Лежить у
#          version/build_counter.txt (локальний, у git не йде).
#
# Мінімум 4 цифри в кожному полі; переросте — просто стане більше цифр.
set -euo pipefail

ROOT="${1:?потрібен корінь репо}"
OUT_H="${2:?потрібен шлях до version.h}"
# 3-й аргумент — однканальна збірка (1/ON). Тоді у кінець версії йде 'S'.
SINGLE="${3:-0}"

VDIR="$ROOT/version"
mkdir -p "$VDIR"

MAJOR_FILE="$VDIR/major.txt"
CNT_FILE="$VDIR/build_counter.txt"

# major — стала, руками. Якщо файла нема — заводимо 01.
[ -f "$MAJOR_FILE" ] || printf '01\n' > "$MAJOR_FILE"
MAJOR="$(tr -dc '0-9' < "$MAJOR_FILE")"
[ -n "$MAJOR" ] || MAJOR="01"

# лічильник — ++ щобілд, не скидається.
if [ -f "$CNT_FILE" ]; then
    CNT="$(tr -dc '0-9' < "$CNT_FILE")"
else
    CNT=0
fi
[ -n "$CNT" ] || CNT=0
CNT=$((CNT + 1))
printf '%s\n' "$CNT" > "$CNT_FILE"

# днів від 2019-01-05, за UTC-північчю обох дат (без похибки годин/DST).
EPOCH="$(date -u -d '2019-01-05 00:00:00' +%s)"
TODAY="$(date -u +%Y-%m-%d)"
NOW="$(date -u -d "$TODAY 00:00:00" +%s)"
DAYS=$(( (NOW - EPOCH) / 86400 ))

VER="$(printf 'V%02d.%04d.%04d' "$((10#$MAJOR))" "$DAYS" "$CNT")"

# Суфікс однканальної збірки — щоб на екрані й в оновленні одразу видно,
# що це обрізаний приймач, а не повний.
case "$SINGLE" in
    1|ON|on|true|TRUE|yes|YES) VER="${VER}S" ;;
esac

printf '%s\n' "$VER" > "$VDIR/version.txt"
printf '#pragma once\n// Згенеровано scripts/gen_version.sh — НЕ редагувати руками.\n#define VRX_VERSION "%s"\n' "$VER" > "$OUT_H"

# Тихо, але лишаємо слід у виводі збірки.
printf '[версія] %s\n' "$VER" >&2
