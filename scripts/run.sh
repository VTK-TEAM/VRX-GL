#!/usr/bin/env bash
# VRX-GL: єдина точка запуску.
#
# Викликається подвійним кліком по "ЗАПУСТИТИ VRX-GL.desktop" у корені
# папки. Робить усе сам: перевіряє залежності, за потреби збирає,
# піднімає станцію й повертає робочий стіл, коли вона зупиниться.
#
# ДВА РЕЖИМИ, І ЦЕ ГОЛОВНЕ В ЦЬОМУ ФАЙЛІ.
#
# Станція забирає DRM master, а його тримає графічна сесія — отже її
# доводиться гасити. Але вікно термінала, з якого нас запустили, живе
# ВСЕРЕДИНІ тієї самої сесії: гасимо lightdm -> падає X -> падає вікно ->
# падає й сам скрипт разом зі станцією. Ззовні це виглядає як "вікно
# мигнуло і зникло", і причина з цього не читається.
#
# Тому робота розділена:
#   без аргументів  — перевірки й збірка у видимому вікні, далі запуск
#                     наглядача ОКРЕМИМ сеансом і вихід;
#   --supervise     — той наглядач: гасить сесію, тримає станцію, вертає
#                     робочий стіл після її зупинки. Термінала вже не
#                     потребує й від його загибелі не залежить.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

BIN="$HERE/build/vrx_gl"
LOGDIR="$HERE/logs"

say()  { printf '\n\033[1m%s\033[0m\n' "$*"; }
fail() { printf '\n\033[1;31m%s\033[0m\n' "$*" >&2; }

# Вихід із поясненням. Саму паузу робить trap EXIT нижче — щоб вона
# була ГАРАНТОВАНО на кожному шляху, а не лише там, де я про неї згадав.
pause_exit() { exit "${1:-0}"; }

# ---------------------------------------------------------------------
# Режим наглядача: викликається сам собою, окремим сеансом, від root.
# ---------------------------------------------------------------------
if [[ "${1:-}" == "--supervise" ]]; then
    shift
    mkdir -p "$LOGDIR"
    LOG="$LOGDIR/vrx-gl_$(date +%Y%m%d_%H%M%S).log"
    ls -1t "$LOGDIR"/vrx-gl_*.log 2>/dev/null | tail -n +11 | xargs -r rm -f || true

    # Робочий стіл вертаємо ЗАВЖДИ — хоч станція зупинилась штатно, хоч
    # впала. Інакше лишиться чорний екран без жодного способу цим
    # скористатись.
    restore_desktop() {
        systemctl start lightdm >/dev/null 2>&1 || true
    }
    trap restore_desktop EXIT

    {
        echo "=== $(date '+%Y-%m-%d %H:%M:%S') запуск ==="

        # Гасимо графічну сесію тут, а не покладаємось на застосунок.
        # Він це вміє, але при кліку ОДРАЗУ ПІСЛЯ ЗАВАНТАЖЕННЯ lightdm ще
        # в стані "запускається": is-active каже "ні", а DRM master він
        # уже захопив. Ловилось наживо — станція падала з "Device or
        # resource busy", і причина з повідомлення не читалась.
        if systemctl list-unit-files lightdm.service >/dev/null 2>&1; then
            systemctl stop lightdm >/dev/null 2>&1 || true
            for _ in $(seq 1 50); do
                [[ "$(systemctl is-active lightdm 2>/dev/null)" == "inactive" ]] && break
                sleep 0.2
            done
        fi

        # Друга копія не піднімається: DRM master ексклюзивний.
        if pgrep -x vrx_gl >/dev/null 2>&1; then
            pkill -x vrx_gl || true
            for _ in $(seq 1 30); do
                pgrep -x vrx_gl >/dev/null 2>&1 || break
                sleep 0.2
            done
        fi

        # Робочий каталог — корінь проєкту. Не косметика: атлас OSD,
        # osd_config.json і картинки шукаються за відносними шляхами, і
        # запуск із іншого каталогу дав би відео без телеметрії, мовчки.
        cd "$HERE"
        "$BIN" "$@"
        echo "=== станція зупинилась, код $? ==="
    } >>"$LOG" 2>&1
    exit 0
fi

# ---------------------------------------------------------------------
# Видима частина: те, що бачить інженер у вікні термінала.
# ---------------------------------------------------------------------

# ТЕРМІНАЛ ВІДКРИВАЄМО СОБІ САМІ, якщо нас запустили без нього. Файловий
# менеджер саме так і робить, а нам треба і показати хід роботи, і дати
# ввести пароль адміністратора. --hold лишає вікно відкритим, щоб
# повідомлення можна було прочитати, а не ловити поглядом.
if [[ ! -t 0 || ! -t 1 ]]; then
    for term in xfce4-terminal x-terminal-emulator mate-terminal gnome-terminal lxterminal xterm; do
        command -v "$term" >/dev/null 2>&1 || continue
        case "$term" in
            xfce4-terminal) exec "$term" --title=VRX-GL --hold -x bash "$0" "$@" ;;
            xterm)          exec "$term" -title VRX-GL -hold -e bash "$0" "$@" ;;
            *)              exec "$term" -e bash "$0" "$@" ;;
        esac
    done
    # Термінала в системі немає — працюємо мовчки, усе піде в лог.
fi


# ВСЕ, ЩО ТУТ ДРУКУЄТЬСЯ, ДУБЛЮЄТЬСЯ У ФАЙЛ. Причина конкретна: якщо
# вікно термінала закриється швидше, ніж людина встигне прочитати, від
# помилки не лишається жодного сліду — наглядач свій лог ще не почав, бо
# до нього не дійшло. Одного разу через це довелось гадати.
mkdir -p "$LOGDIR" 2>/dev/null || true
LAUNCH_LOG="$LOGDIR/launcher.log"
{
    echo
    echo "=== $(date '+%Y-%m-%d %H:%M:%S') клік по ярлику ==="
} >>"$LAUNCH_LOG" 2>/dev/null || true
# Каталог логів міг створити наглядач від root, і тоді звичайний
# користувач у нього не запише. Тоді просто не дублюємо — це не привід
# не запускатись.
if : >>"$LAUNCH_LOG" 2>/dev/null; then
    exec > >(tee -a "$LAUNCH_LOG") 2>&1
fi

# Вікно НЕ закривається саме — ні при помилці, ні при успіху. Інакше
# єдине, що бачить людина, це "мигнуло і зникло".
trap 'echo; read -r -p "Натисніть Enter, щоб закрити вікно..." _ || true' EXIT

say "VRX-GL — приймальна станція"
echo "Папка: $HERE"

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

# Збираємо лише якщо збірки немає: готовий бінарник у папці працює на
# такому ж залізі одразу, і чекати компіляції в полі не доводиться.
if [[ ! -x "$BIN" ]]; then
    say "Збірки немає — збираю (кілька хвилин, лише перший раз)"
    if ! command -v cmake >/dev/null; then
        fail "Немає cmake: sudo apt install cmake build-essential"
        pause_exit 1
    fi
    if ! cmake -B build -S . >/dev/null || ! cmake --build build -j"$(nproc)"; then
        fail "Зібрати не вдалося."
        pause_exit 1
    fi
fi

# Потрібен root: DRM master ексклюзивний.
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

# Пристрій виводу може тримати щось стороннє. Застосунок бачить лише
# "Device or resource busy" і хто саме — не знає; скажемо тут.
if command -v fuser >/dev/null 2>&1; then
    holders="$($SUDO fuser -v /dev/dri/card0 2>&1 | tail -n +2 | awk '{print $NF}' \
               | grep -vE '^(Xorg|lightdm)$' | sort -u | tr '\n' ' ')"
    if [[ -n "${holders// /}" ]]; then
        fail "Пристрій виводу зайнятий: $holders"
        echo "Зупиніть ці програми й запустіть ще раз."
        pause_exit 1
    fi
fi

# ЗАПУСК ОКРЕМИМ СИСТЕМНИМ ЮНІТОМ.
#
# Тут недостатньо ні фонового запуску, ні навіть setsid. Причина тонка:
# setsid міняє СЕАНС, але не виводить процес із cgroup графічної сесії.
# А ми цю сесію гасимо — і systemd прибирає разом із нею все, що в ній
# лишилось, включно з нашим наглядачем. У лозі це виглядало як
# "Terminated" одразу після запуску, а на екрані — як спалах і
# повернення робочого стола.
#
# systemd-run піднімає наглядача власним системним юнітом, поза будь-якою
# сесією. Саме тому в старому VRX був systemd-юніт — тепер зрозуміло,
# чому без нього не обійтись.
say "Запускаю станцію..."
if command -v systemd-run >/dev/null 2>&1; then
    $SUDO systemd-run --collect --unit=vrx-gl \
        --description="VRX-GL" \
        --property=WorkingDirectory="$HERE" \
        bash "$HERE/scripts/run.sh" --supervise "$@" >/dev/null 2>&1
else
    # Без systemd лишається setsid: не так надійно, але краще, ніж нічого.
    $SUDO setsid --fork bash "$HERE/scripts/run.sh" --supervise "$@" </dev/null >/dev/null 2>&1
fi

sleep 3
if pgrep -x vrx_gl >/dev/null 2>&1 || pgrep -f 'run.sh --supervise' >/dev/null 2>&1; then
    echo "Станція піднімається. Це вікно можна закрити."
    echo "Логи: $LOGDIR/"
else
    fail "Станція не піднялась. Останній лог:"
    tail -n 20 "$(ls -1t "$LOGDIR"/vrx-gl_*.log 2>/dev/null | head -1)" 2>/dev/null
    pause_exit 1
fi
