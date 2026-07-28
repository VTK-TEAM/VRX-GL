#!/usr/bin/env bash
# Одноразове налаштування VRX-GL на новій оранжі.
#
# Що робить: перевіряє залежності, за потреби збирає, кладе на робочий
# стіл два ярлики (запуск і зупинка) і дозволяє запускати їх без пароля.
# Після цього інженеру потрібна лише мишка.
#
# Запускати можна і з-під звичайного користувача — скрипт сам попросить
# пароль один раз.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# --- root ---
# Перезапускаємось під sudo, зберігаючи, ХТО саме нас покликав: ярлики
# мають лягти на робочий стіл того користувача, а не root'а.
if [[ $EUID -ne 0 ]]; then
  echo "Потрібні права адміністратора — введіть пароль."
  exec sudo -E VRX_REAL_USER="${USER}" bash "$0" "$@"
fi

REAL_USER="${VRX_REAL_USER:-${SUDO_USER:-}}"
if [[ -z "$REAL_USER" || "$REAL_USER" == "root" ]]; then
  echo "Не вдалося визначити користувача робочого столу." >&2
  echo "Запустіть як звичайний користувач: bash scripts/install.sh" >&2
  exit 1
fi
REAL_HOME="$(getent passwd "$REAL_USER" | cut -d: -f6)"
if [[ -z "$REAL_HOME" || ! -d "$REAL_HOME" ]]; then
  echo "Не знайдено домашній каталог користувача $REAL_USER" >&2
  exit 1
fi

echo "=== VRX-GL: налаштування ==="
echo "Проєкт:      $HERE"
echo "Користувач:  $REAL_USER ($REAL_HOME)"
echo

# --- залежності ---
missing=()
for lib in libdrm gbm egl glesv2 gstreamer-1.0 gstreamer-app-1.0; do
  pkg-config --exists "$lib" 2>/dev/null || missing+=("$lib")
done
if (( ${#missing[@]} )); then
  echo "БРАКУЄ БІБЛІОТЕК: ${missing[*]}" >&2
  echo "Встановіть їх і запустіть цей скрипт ще раз:" >&2
  echo "  sudo apt install libdrm-dev libgbm-dev libegl-dev libgles-dev \\" >&2
  echo "                   libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev" >&2
  exit 1
fi

# GBM/EGL мають братися з ВЕНДОРНОГО стеку Mali, не з Mesa. З Mesa воно
# теж збереться, але не працюватиме на залізі — і помилка буде далеко
# від причини, тож попереджаємо одразу.
if [[ ! -d /usr/lib/aarch64-linux-gnu/mali ]]; then
  echo "УВАГА: не знайдено /usr/lib/aarch64-linux-gnu/mali —"
  echo "       можливо, EGL/GLES візьмуться з Mesa, і на залізі це не працюватиме."
  echo
fi

# --- збірка ---
if [[ ! -x "$HERE/build/vrx_gl" ]]; then
  echo "Збірки немає, збираю..."
  if ! command -v cmake >/dev/null; then
    echo "Немає cmake: sudo apt install cmake build-essential" >&2
    exit 1
  fi
  cmake -B "$HERE/build" -S "$HERE" >/dev/null
  cmake --build "$HERE/build" -j"$(nproc)"
  echo "Готово."
else
  echo "Збірка на місці: $HERE/build/vrx_gl"
fi
chmod +x "$HERE/scripts/start.sh" "$HERE/scripts/stop.sh"
echo

# --- запуск без пароля ---
#
# Ярлик не може спитати пароль (Terminal=false), тому цим двом скриптам
# дозволяється sudo без нього.
#
# ЧЕСНО ПРО РИЗИК: скрипти лежать у домашньому каталозі, тобто той, хто
# має доступ до цього користувача, може їх змінити й дістати root. Для
# наземної станції в полі це прийнятно — вона й так під повним контролем
# того, хто за нею сидить. На машині загального користування так робити
# не варто.
SUDOERS=/etc/sudoers.d/vrx-gl
cat > "$SUDOERS" <<EOF
# VRX-GL: запуск і зупинка з ярлика без пароля.
$REAL_USER ALL=(root) NOPASSWD: $HERE/scripts/start.sh, $HERE/scripts/stop.sh
EOF
chmod 440 "$SUDOERS"
if visudo -cf "$SUDOERS" >/dev/null; then
  echo "Дозвіл на запуск без пароля: $SUDOERS"
else
  rm -f "$SUDOERS"
  echo "Правило sudo виявилось некоректним і не застосоване." >&2
  exit 1
fi
echo

# --- ярлики ---
DESKTOP_DIR="$REAL_HOME/Desktop"
[[ -d "$DESKTOP_DIR" ]] || DESKTOP_DIR="$(sudo -u "$REAL_USER" xdg-user-dir DESKTOP 2>/dev/null || echo "$REAL_HOME/Desktop")"
mkdir -p "$DESKTOP_DIR"
APPS_DIR="$REAL_HOME/.local/share/applications"
mkdir -p "$APPS_DIR"

ICON="$HERE/img/vrx-gl-icon.svg"
[[ -f "$ICON" ]] || ICON="video-display"

make_launcher() {
  local file="$1" name="$2" comment="$3" exec_line="$4"
  cat > "$file" <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=$name
Comment=$comment
Exec=$exec_line
Path=$HERE
Terminal=false
Icon=$ICON
Categories=AudioVideo;Player;
EOF
  chmod +x "$file"
  chown "$REAL_USER":"$REAL_USER" "$file"
}

for dir in "$DESKTOP_DIR" "$APPS_DIR"; do
  make_launcher "$dir/vrx-gl.desktop" "VRX-GL" \
    "Приймальна станція FPV-лінка" \
    "sudo -n $HERE/scripts/start.sh"
  make_launcher "$dir/vrx-gl-stop.desktop" "VRX-GL — зупинити" \
    "Зупинити станцію й повернути робочий стіл" \
    "sudo -n $HERE/scripts/stop.sh"
done

# Без цієї позначки файловий менеджер на подвійний клік питає
# "чи довіряєте ви цьому файлу" — інженеру в полі це зайве.
for f in "$DESKTOP_DIR/vrx-gl.desktop" "$DESKTOP_DIR/vrx-gl-stop.desktop"; do
  sudo -u "$REAL_USER" gio set "$f" metadata::trusted true 2>/dev/null || true
done

echo "Ярлики на робочому столі: VRX-GL і VRX-GL — зупинити"
echo
echo "=== Готово ==="
echo "Запуск — подвійний клік по ярлику VRX-GL."
echo "Логи   — $HERE/logs/"
