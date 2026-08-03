#!/usr/bin/env bash
# make_update.sh — зібрати update_list для оновлення VRX-GL.
#
# Запускати на машині розробки (Linux, WSL або Git Bash), НЕ на станції.
#
#   ./make_update.sh <каталог VRX-update> <файл ключа> [версія]
#
# Приклад:
#   ./make_update.sh /media/usb/VRX-update ./vrx-update.key V01.2767.0001
#
# Версію можна НЕ вказувати: тоді береться version/version.txt із кореня
# репо — рівно та, з якою зібрано бінарник, що поруч кладеться в пакет.
# Так рядок на екрані й version.txt оновлення завжди збігаються.
#
# Каталог VRX-update має містити файли РІВНО за тими шляхами, за якими
# вони лежать у корені станції. Тобто щоб замінити build/vrx_gl, файл
# кладеться у VRX-update/build/vrx_gl.
#
# Скрипт обходить усі файли в каталозі, крім version.txt і update_list,
# рахує HMAC-SHA256 з ключем і пише update_list та version.txt.
set -euo pipefail

DIR="${1:-}"
KEYFILE="${2:-}"
VERSION="${3:-}"

# Без версії — беремо version/version.txt із кореня репо. Скрипт лежить у
# tools/update/, тож корінь на два рівні вище.
if [ -z "$VERSION" ]; then
    SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    VER_FILE="$SELF_DIR/../../version/version.txt"
    if [ -r "$VER_FILE" ]; then
        VERSION="$(cat "$VER_FILE")"
    fi
fi

if [ -z "$DIR" ] || [ -z "$KEYFILE" ] || [ -z "$VERSION" ]; then
    echo "використання: $0 <каталог VRX-update> <файл ключа> [версія]" >&2
    echo "  (без версії потрібен version/version.txt — зроби білд)" >&2
    exit 1
fi

[ -d "$DIR" ]      || { echo "немає каталогу: $DIR" >&2; exit 1; }
[ -r "$KEYFILE" ]  || { echo "немає ключа: $KEYFILE" >&2; exit 1; }

# $(cat) прибирає кінцевий перенос рядка — так само, як робить станція.
KEY="$(cat "$KEYFILE")"
[ -n "$KEY" ] || { echo "ключ порожній" >&2; exit 1; }

LIST="$DIR/update_list"
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

printf '# список файлів оновлення VRX-GL\n' > "$TMP"
printf '# формат: <hmac-sha256>  <шлях відносно кореня станції>\n' >> "$TMP"
printf '# версія: %s\n' "$VERSION" >> "$TMP"

count=0
while IFS= read -r -d '' f; do
    rel="${f#"$DIR"/}"

    case "$rel" in
        version.txt | update_list) continue ;;
    esac

    # Ті самі правила, що й на станції: без .. , без ./ і без //
    case "/$rel/" in
        */../* | */./* | *//*)
            echo "недопустимий шлях: $rel" >&2
            exit 1
            ;;
    esac

    h="$(openssl dgst -sha256 -hmac "$KEY" -r "$f" | awk '{print $1}')"
    printf '%s  %s\n' "$h" "$rel" >> "$TMP"
    printf '  %s  %s\n' "$h" "$rel"
    count=$((count + 1))
done < <(find "$DIR" -type f -print0 | sort -z)

[ "$count" -gt 0 ] || { echo "у каталозі немає файлів для оновлення" >&2; exit 1; }

mv "$TMP" "$LIST"
trap - EXIT
printf '%s\n' "$VERSION" > "$DIR/version.txt"

echo
echo "готово: файлів $count, версія $VERSION"
echo "  $LIST"
echo "  $DIR/version.txt"
