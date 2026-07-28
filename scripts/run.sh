#!/usr/bin/env bash
# VRX-GL: єдина точка запуску.
#
# Викликається подвійним кліком по "ЗАПУСТИТИ VRX-GL.desktop" у корені
# папки. Робить усе сам: перевіряє залежності, за потреби збирає,
# піднімає станцію й повертає робочий стіл після виходу.
#
# Нічого встановлювати, нічого налаштовувати, жодних ярликів на
# робочому столі — папку розпакували, файл запустили.
set -uo pipefail

# Корінь рахується від розташування скрипта, а не прибитий у тексті:
# папка має працювати з будь-якого місця й під будь-яким користувачем.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

BIN="$HERE/build/vrx_gl"
LOGDIR="$HERE/logs"

say()  { printf '\n\033[1m%s\033[0m\n' "$*"; }
fail() { printf '\n\033[1;31m%s\033[0m\n' "$*" >&2; }

# Термінал не має зникнути миттєво, інакше людина не встигне прочитати,
# що пішло не так.
pause_exit() {
    echo
    read -r -p "Натисніть Enter, щоб закрити вікно..." _ || true
    exit "${1:-0}"
}

say "VRX-GL — приймальна станція"
echo "Папка: $HERE"

# --- залежності ---
missing=()
for lib in libdrm gbm egl glesv2 gstreamer-1.0 gstreamer-app-1.0; do
    pkg-config --exists "$lib" 2>/dev/null || missing+=("$lib")
done
if (( ${#missing[@]} )); then
    fail "Бракує бібліотек: ${missing[*]}"
    echo "Встановіть їх командою:"
    echo "  sudo apt install libdrm-dev libgbm-dev libegl-dev libgles-dev \\"
    echo "                   libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev cmake build-essential"
    pause_exit 1
fi

# --- збірка ---
# Збираємо лише якщо збірки немає. Готовий бінарник у папці працює на
# такому ж залізі одразу, і чекати компіляції в полі не доводиться.
if [[ ! -x "$BIN" ]]; then
    say "Збірки немає — збираю (це кілька хвилин, лише перший раз)"
    if ! command -v cmake >/dev/null; then
        fail "Немає cmake: sudo apt install cmake build-essential"
        pause_exit 1
    fi
    if ! cmake -B build -S . >/dev/null || ! cmake --build build -j"$(nproc)"; then
        fail "Зібрати не вдалося."
        pause_exit 1
    fi
fi

# --- лог ---
mkdir -p "$LOGDIR"
LOG="$LOGDIR/vrx-gl_$(date +%Y%m%d_%H%M%S).log"
# Лишаємо останні 10. "|| true" обов'язковий: порожній ls на новій
# машині інакше повалив би скрипт ще до запуску.
ls -1t "$LOGDIR"/vrx-gl_*.log 2>/dev/null | tail -n +11 | xargs -r rm -f || true

# --- root ---
# Потрібен для DRM master: без нього не буде ні зміни режиму, ні плейна.
SUDO=""
if [[ $EUID -ne 0 ]]; then
    SUDO="sudo"
    if ! sudo -n true 2>/dev/null; then
        say "Потрібні права адміністратора — введіть пароль користувача"
    fi
    if ! sudo -v; then
        fail "Без прав адміністратора станція не запуститься."
        pause_exit 1
    fi
fi

# Друга копія не піднімається: DRM master ексклюзивний.
if pgrep -x vrx_gl >/dev/null 2>&1; then
    echo "Зупиняю попередню копію..."
    $SUDO pkill -x vrx_gl || true
    for _ in $(seq 1 30); do
        pgrep -x vrx_gl >/dev/null 2>&1 || break
        sleep 0.2
    done
fi

# --- графічна сесія ---
#
# DRM master ексклюзивний, і поки його тримає робочий стіл, станція не
# підніметься. Застосунок уміє гасити lightdm сам, але покладатись на це
# не можна: якщо клікнути ярлик ОДРАЗУ ПІСЛЯ ЗАВАНТАЖЕННЯ, lightdm ще в
# стані "запускається" — перевірка "is-active" каже "ні", а DRM він уже
# захопив. Ловилось наживо: станція падала з "Device or resource busy",
# і причина з повідомлення не читалась.
#
# Тому гасимо тут і чекаємо, поки пристрій справді звільниться.
if systemctl list-unit-files lightdm.service >/dev/null 2>&1; then
    if [[ "$($SUDO systemctl is-active lightdm 2>/dev/null)" != "inactive" ]]; then
        echo "Зупиняю графічну сесію..."
        $SUDO systemctl stop lightdm 2>/dev/null || true
        for _ in $(seq 1 50); do
            [[ "$($SUDO systemctl is-active lightdm 2>/dev/null)" == "inactive" ]] && break
            sleep 0.2
        done
    fi
fi

# Якщо пристрій усе одно зайнятий — кажемо ХТО його тримає. Сам
# застосунок цього не знає й повідомляє лише "Device or resource busy",
# з чого причина не читається. Ловилось наживо: DRM тримала стороння
# програма, і без цього рядка довелося б гадати.
if command -v fuser >/dev/null 2>&1; then
    holders="$($SUDO fuser -v /dev/dri/card0 2>&1 | tail -n +2 | awk '{print $NF}' | sort -u | tr '\n' ' ')"
    if [[ -n "${holders// /}" ]]; then
        fail "Пристрій виводу зайнятий: $holders"
        echo "Зупиніть ці програми й запустіть ще раз."
        pause_exit 1
    fi
fi

say "Працюю. Щоб зупинити — Ctrl+C або закрийте це вікно."
echo "Лог: $LOG"
echo

# Робочий стіл повертаємо ЗАВЖДИ, хоч по Ctrl+C, хоч після падіння:
# застосунок зупиняє графічну сесію сам, і без цього лишився б чорний
# екран, тобто станція без жодного способу нею скористатись.
restore_desktop() {
    echo
    echo "Повертаю робочий стіл..."
    $SUDO systemctl start lightdm 2>/dev/null || true
}
trap restore_desktop EXIT

# Робочий каталог — корінь проєкту. Не косметика: атлас OSD,
# osd_config.json і картинки шукаються за відносними шляхами, і запуск
# із іншого каталогу дав би відео без телеметрії, причому мовчки.
$SUDO "$BIN" "$@" 2>&1 | tee "$LOG"
