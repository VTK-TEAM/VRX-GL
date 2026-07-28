#!/usr/bin/env bash
# Зупинка VRX-GL і повернення робочого столу.
#
# Застосунок зупиняє графічну сесію сам (DRM master ексклюзивний), тому
# після виходу екран лишився б чорним. Тут ми її вертаємо — інакше
# інженеру в полі нема чим користуватись.
set -euo pipefail

if pgrep -x vrx_gl >/dev/null 2>&1; then
  echo "Зупиняю VRX-GL..."
  # SIGTERM, а не KILL: застосунок за ним коректно дозаписує файл на
  # флешку й повертає камері номінальну частоту.
  pkill -x vrx_gl || true
  for _ in $(seq 1 50); do
    pgrep -x vrx_gl >/dev/null 2>&1 || break
    sleep 0.2
  done
  pgrep -x vrx_gl >/dev/null 2>&1 && pkill -9 -x vrx_gl || true
else
  echo "VRX-GL не працює."
fi

if systemctl list-unit-files lightdm.service >/dev/null 2>&1; then
  echo "Повертаю робочий стіл..."
  systemctl start lightdm || true
fi
