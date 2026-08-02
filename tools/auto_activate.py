#!/usr/bin/env python3
"""
Провізійний активатор станцій VRX-GL — крос-платформний (Windows/macOS/Linux).

Той самий принцип, що й камерний auto_activate.py: крутиться безперервно
(лиши запущеним, поки прошиваєш/тестуєш станції), і щойно на IP
з'являється станція — читає особистість її мікроСД, і якщо дійсної
ліцензії ще немає, підписує її й вписує у файл-приманку. Хто прошиває —
просто вмикає станцію в мережу, і вона активується сама.

Відмінності від камери:
  - особистість беремо не з HTTP, а через SSH: `vrx_gl -sdinfo` друкує
    рядок "MESSAGE <hex>" — канонічні байти CID картки для підпису;
  - підпис Ed25519 (не ECDSA), 64 байти, вписуються у osd_glyph_kern.bin
    на зміщенні SIG_OFFSET (див. vrx_sign.py / license_store.hpp);
  - файли ллємо через `ssh ... "dd ..."`, а не scp: на станції може не
    бути sftp-server, від якого залежить scp за замовчуванням.

Автентифікація — ВИДІЛЕНИМ ключем (provision_key), а не паролем. Ключ
ставиться на станцію один раз через install_ssh_key.sh, і далі доступ є
незалежно від пароля.

Requires: pip install cryptography   (ssh береться з ОС)

Usage: auto_activate.py [station-ip] [ssh-private-key] [vendor_private.pem]
"""
import datetime
import os
import shutil
import subprocess
import sys
import time

import vrx_sign

DEFAULT_IP = "192.168.1.1"
DEFAULT_SSH_KEY = "provision_key"
DEFAULT_PRIVKEY = "vendor_ed25519_private.pem"

# Шляхи на станції. Типове розгортання — /opt/vrx (див. README, розділ
# "Розгортання на чужій оранжі"). Перекривається змінними оточення.
REMOTE_BIN = os.environ.get("VRX_REMOTE_BIN", "/opt/vrx/vrx_gl")
REMOTE_STORE = os.environ.get("VRX_REMOTE_STORE", "/opt/vrx/osd_glyph_kern.bin")
LOG_PATH = os.environ.get("VRX_ACTIVATE_LOG", "activation.log")

SSH_BIN = None


def log(msg):
    line = "%s  %s" % (datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"), msg)
    print(line)
    try:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


def fix_key_permissions(key_path):
    """OpenSSH відмовляється вантажити приватний ключ, доступний іншим.
    На Unix це chmod; на Windows os.chmod() не чіпає NTFS ACL, який
    перевіряє ssh.exe, тож там потрібен icacls. Ідемпотентно, ніколи не
    фатально."""
    if os.name == "nt":
        try:
            subprocess.run(["icacls", key_path, "/inheritance:r"],
                           capture_output=True, timeout=5)
            user = os.environ.get("USERNAME", "")
            domain = os.environ.get("USERDOMAIN", "")
            account = "%s\\%s" % (domain, user) if domain else user
            subprocess.run(["icacls", key_path, "/grant:r", "%s:(R)" % account],
                           capture_output=True, timeout=5)
        except Exception:
            pass
    else:
        try:
            os.chmod(key_path, 0o600)
        except Exception:
            pass


def find_ssh_binary(name):
    path = shutil.which(name)
    if not path:
        print("ERROR: %r не знайдено в PATH. На Windows увімкни 'OpenSSH "
              "Client' (Settings > Apps > Optional Features) — він є з "
              "Win10 1809+ / Win11." % name)
        sys.exit(1)
    return path


def ssh_opts(key_path):
    return [
        "-i", key_path,
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=" + os.devnull,
        "-o", "BatchMode=yes",
        "-o", "ConnectTimeout=2",
        "-o", "LogLevel=ERROR",
    ]


def read_identity(ip, key_path):
    """Через SSH запускає `vrx_gl -sdinfo` і повертає (msg_bytes, опис) або
    None, якщо станція недоступна чи картки немає."""
    cmd = [SSH_BIN, *ssh_opts(key_path), "root@%s" % ip, "%s -sdinfo" % REMOTE_BIN]
    try:
        out = subprocess.run(cmd, capture_output=True, timeout=8)
    except subprocess.TimeoutExpired:
        return None
    if out.returncode not in (0, 2):
        return None
    text = out.stdout.decode(errors="replace")
    msg = None
    describe = ""
    for line in text.splitlines():
        if line.startswith("MESSAGE "):
            try:
                msg = vrx_sign.message_from_hex(line)
            except Exception:
                return None
        elif line.startswith("CID="):
            describe = line.strip()
    if not msg:
        return None
    return (msg, describe)


def read_remote_sig(ip, key_path):
    """Читає 64 байти підпису з приманки на станції (dd за зміщенням)."""
    cmd = [SSH_BIN, *ssh_opts(key_path), "root@%s" % ip,
           "dd if=%s bs=1 skip=%d count=%d 2>/dev/null"
           % (REMOTE_STORE, vrx_sign.SIG_OFFSET, vrx_sign.SIG_LEN)]
    try:
        out = subprocess.run(cmd, capture_output=True, timeout=8)
    except subprocess.TimeoutExpired:
        raise RuntimeError("ssh (читання підпису) завис")
    if out.returncode != 0:
        return None
    return out.stdout


def upload_sig(ip, key_path, sig):
    """Вписує 64 байти підпису у приманку на зміщенні, не чіпаючи решту
    файлу (conv=notrunc). Через `ssh ... dd`, а не scp — sftp-server на
    станції може бути відсутній."""
    cmd = [SSH_BIN, *ssh_opts(key_path), "root@%s" % ip,
           "dd of=%s bs=1 seek=%d conv=notrunc status=none"
           % (REMOTE_STORE, vrx_sign.SIG_OFFSET)]
    out = subprocess.run(cmd, input=sig, capture_output=True, timeout=10)
    if out.returncode != 0:
        raise RuntimeError("запис підпису не вдався: %s"
                           % out.stderr.decode(errors="replace").strip())


def restart_station(ip, key_path):
    """Убиває процес станції — наглядач (vrx_supervisor) підніме його сам,
    уже з дійсною ліцензією. Саме kill, а не окремий перезапуск: у станції
    немає "виходу", будь-яке завершення (крім запуску редактора) наглядач
    трактує як рестарт. Бʼємо ТІЛЬКИ vrx_gl, наглядача не чіпаємо."""
    cmd = [SSH_BIN, *ssh_opts(key_path), "root@%s" % ip, "pkill -x vrx_gl"]
    try:
        # rc=1 = процес не знайдено (уже перезапускається) — не помилка.
        subprocess.run(cmd, capture_output=True, timeout=6)
    except subprocess.TimeoutExpired:
        raise RuntimeError("ssh (рестарт) завис")


def wait_for_device(ip, key_path):
    """Блокує, поки станція не відповість на SSH `-sdinfo`."""
    while True:
        ident = read_identity(ip, key_path)
        if ident:
            return ident
        time.sleep(0.5)


def hold_until_disconnected(ip, key_path):
    """Тримає одну SSH-сесію відкритою й блокує тут, поки вона не впаде
    (станцію вимкнули/перезавантажили/зникла мережа). ServerAlive робить
    виявлення швидким. Віддалена команда має блокуватись САМА, не на
    читанні stdin: stdin=DEVNULL дає EOF одразу, і `cat` вийшов би за
    секунду — тому цикл зі sleep."""
    cmd = [SSH_BIN, *ssh_opts(key_path),
           "-o", "ServerAliveInterval=2",
           "-o", "ServerAliveCountMax=2",
           "root@%s" % ip, "while :; do sleep 3600; done"]
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        proc.wait()
    except KeyboardInterrupt:
        proc.terminate()
        raise


def main():
    global SSH_BIN

    try:
        sys.stdout.reconfigure(line_buffering=True)
    except Exception:
        pass

    ip = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_IP
    key_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_SSH_KEY
    privkey_path = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_PRIVKEY

    if not os.path.isfile(key_path):
        print("ERROR: SSH-ключ не знайдено: %s" % key_path)
        sys.exit(1)
    if not os.path.isfile(privkey_path):
        print("ERROR: підписний ключ вендора не знайдено: %s" % privkey_path)
        sys.exit(1)

    fix_key_permissions(key_path)
    SSH_BIN = find_ssh_binary("ssh")

    # Публічну половину рахуємо з приватної — щоб звіряти вже наявний
    # підпис і не переписувати чинну ліцензію дарма.
    vendor_key = vrx_sign.load_private_key(privkey_path)
    pub_raw = vrx_sign.public_raw(vendor_key)

    log("[auto_activate] стежу за %s, sshkey=%s, signkey=%s (Ctrl+C — стоп)"
        % (ip, key_path, privkey_path))

    while True:
        # Фаза 1: пошук — чекаємо, поки станція відповість на -sdinfo.
        msg, describe = wait_for_device(ip, key_path)
        log("[auto_activate] станція є: %s" % (describe or "?"))

        # Фаза 2: активація, якщо ще немає або підпис недійсний.
        try:
            cur = read_remote_sig(ip, key_path)
            if cur is not None and vrx_sign.verify_message(msg, cur, pub_raw):
                log("[auto_activate] вже активовано й дійсно")
            else:
                why = "підпису немає" if not cur else "підпис недійсний, переписую"
                log("[auto_activate] активую (%s) ..." % why)
                sig = vrx_sign.sign_message(msg, vendor_key)
                upload_sig(ip, key_path, sig)
                # перечитуємо й перевіряємо, що справді лягло
                back = read_remote_sig(ip, key_path)
                if back is not None and vrx_sign.verify_message(msg, back, pub_raw):
                    # Одразу вбиваємо станцію — наглядач підніме її вже
                    # ліцензованою, без ручного кроку.
                    restart_station(ip, key_path)
                    log("[auto_activate] OK: активовано, станцію перезапущено")
                else:
                    log("[auto_activate] УВАГА: підпис записано, але перевірка "
                        "не пройшла — перевір шлях %s" % REMOTE_STORE)
        except Exception as e:
            log("[auto_activate] ПОМИЛКА: %s (повторю)" % e)
            time.sleep(0.5)
            continue

        # Фаза 3: тримаємо сесію відкритою, поки не від'єднається.
        log("[auto_activate] готово, тримаю з'єднання до від'єднання ...")
        hold_until_disconnected(ip, key_path)
        log("[auto_activate] станцію від'єднано, шукаю знову ...")


if __name__ == "__main__":
    main()
