#!/usr/bin/env bash
# install_ssh_key.sh — поставити провізійний SSH-ключ на ЦЮ станцію.
#
# Запускати РАЗ на новій машині (від root або через sudo). Після цього
# провізійний ПК заходить по SSH ключем provision_key НЕЗАЛЕЖНО ВІД
# ПАРОЛЯ — саме на цьому тримається авто-активація (auto_activate.py).
#
# Робить лише необхідне й ідемпотентно:
#   1. кладе публічний ключ у /root/.ssh/authorized_keys (як його там ще нема)
#   2. вмикає й запускає sshd
#   3. дозволяє вхід root по ключу (drop-in, без пароля)
set -euo pipefail

# Публічна половина провізійної пари. Приватна (provision_key) лишається
# на провізійному ПК і в git НЕ комітиться.
PROVISION_PUBKEY="ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAINgasRRNBV4gS+O0tZT/hfrjWHiCuRR1t1RbYdBLpsmm vrx-provision"

if [[ "$(id -u)" != "0" ]]; then
    echo "потрібні права root. запусти: sudo $0" >&2
    exit 1
fi

# --- 1. ключ у authorized_keys root ---
install -d -m 700 /root/.ssh
touch /root/.ssh/authorized_keys
chmod 600 /root/.ssh/authorized_keys
if grep -qF "$PROVISION_PUBKEY" /root/.ssh/authorized_keys; then
    echo "[ключ] уже стоїть"
else
    printf '%s\n' "$PROVISION_PUBKEY" >> /root/.ssh/authorized_keys
    echo "[ключ] додано в /root/.ssh/authorized_keys"
fi

# --- 2. sshd увімкнено й піднято ---
# Пакет може називатись ssh або sshd залежно від дистрибутива.
if command -v systemctl >/dev/null 2>&1; then
    systemctl enable ssh 2>/dev/null || systemctl enable sshd 2>/dev/null || true
    systemctl restart ssh 2>/dev/null || systemctl restart sshd 2>/dev/null || true
    echo "[sshd] увімкнено й перезапущено"
else
    echo "[sshd] systemctl не знайдено — увімкни sshd вручну" >&2
fi

# --- 3. дозволити root по ключу ---
# Drop-in, а не правка головного конфігу: так нічого не ламаємо й легко
# прибрати. prohibit-password = вхід ключем дозволено, паролем root — ні.
DROPIN_DIR=/etc/ssh/sshd_config.d
if [[ -d "$DROPIN_DIR" ]]; then
    cat > "$DROPIN_DIR/10-vrx-provision.conf" <<'CONF'
# VRX-GL provisioning: доступ по ключу незалежно від пароля.
PubkeyAuthentication yes
PermitRootLogin prohibit-password
CONF
    systemctl reload ssh 2>/dev/null || systemctl reload sshd 2>/dev/null || true
    echo "[sshd] drop-in 10-vrx-provision.conf встановлено"
else
    # Старий OpenSSH без каталогу drop-in — правимо головний конфіг з
    # маркерами, ідемпотентно.
    CONF=/etc/ssh/sshd_config
    if [[ -f "$CONF" ]] && ! grep -q "VRX-GL provisioning" "$CONF"; then
        {
            echo ""
            echo "# VRX-GL provisioning"
            echo "PubkeyAuthentication yes"
            echo "PermitRootLogin prohibit-password"
        } >> "$CONF"
        systemctl reload ssh 2>/dev/null || systemctl reload sshd 2>/dev/null || true
        echo "[sshd] додано в $CONF"
    else
        echo "[sshd] конфіг уже містить наш блок або відсутній"
    fi
fi

echo "готово. перевір із провізійного ПК:"
echo "  ssh -i provision_key root@<ip>  ':'"
