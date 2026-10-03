#pragma once

// Клієнт HTTP-API камери (`HTTP_API_CONTRACT.md` у прошивці waybeam).
//
// Своя реалізація на сокеті замість libcurl — запити тривіальні, а повний
// контроль над таймаутами тут важливий: камера на тому кінці РЧ-лінка, і
// зависання запиту не має чіпати нікого.
//
// НЕ БЛОКУЄ нікого стороннього: усі виклики йдуть із власного потоку
// PhaseController. Прямо з потоку рендера сюди звертатися не можна.
//
// ЧОМУ ТУТ РОЗБИРАЄТЬСЯ ВІДПОВІДЬ, А НЕ ЛИШЕ КОД HTTP. Контракт камери
// стоїть на двох правилах, і обидва дають нам те, чого раніше не було:
//
//   "Refuse, never clamp" — значення поза смугою ВІДХИЛЯЄТЬСЯ, а не
//   обрізається. Раніше петля не могла відрізнити «застосовано» від
//   «мовчки згинуло»: додатна команда гинула, а петля вважала, що діє.
//
//   "A refusal carries the current state" — у відмові приходить і
//   поточний trim, і справжні межі. Тобто смугу актуатора не треба
//   вгадувати числом у конфізі: камера сама каже, де її рейки.
//
// Тому мінімум, який має сенс, — це конверт `{"ok":…}` цілком, а не
// `strstr(" 200 ")`.

#include <cstdint>
#include <string>

namespace vrx::control {

class CameraApi {
public:
    struct Config {
        std::string host = "192.168.1.10";
        int port = 80;

        // Камера — маленький вбудований пристрій на тому кінці лінка.
        // Таймаути короткі: краще пропустити одне коригування, ніж
        // тримати потік у очікуванні.
        int connect_timeout_ms = 300;
        int io_timeout_ms = 500;
    };

    // Смуга актуатора, як її повідомляє камера. `known` окремо від нулів:
    // hi == 0 — нормальне значення (матриця вже на стелі режиму), тож нуль
    // сам по собі не означає "не знаємо".
    struct Band {
        int lo_mhz = 0;
        int hi_mhz = 0;
        bool known = false;
    };

    struct Phase {
        int trim_mhz = 0;
        Band band;
    };

    // `ok` — це поле конверта камери, а НЕ код HTTP. Викликач гілкується
    // по ньому й ніколи не розбирає текст: `error` називає умову
    // (`out_of_range`, `unavailable`, `bad_request`, `failed`), а
    // формулювання `detail` може змінитися будь-коли.
    struct Reply {
        bool ok = false;
        int http = 0;            // 0 = не з'єдналося взагалі
        std::string error;
        std::string detail;

        // У ВІДМОВІ ТЕЖ БУВАЄ. Петля, що перескочила рейку, дізнається
        // з цієї ж відповіді, де вона стоїть насправді, — без другого
        // запиту, поки фаза продовжує повзти.
        bool has_phase = false;
        Phase phase;
    };

    struct Status {
        bool valid = false;
        std::string version;

        std::string codec;
        int width = 0;
        int height = 0;

        // ДВІ ЧАСТОТИ, І ЦЕ НЕ ДУБЛЮВАННЯ. `fpsRequested` — це запит у
        // конфізі камери, `fpsLive` — те, що реально віддає матриця:
        // стеля режиму сенсора перебиває запит. Наша петля фази міряє
        // фазу по vblank'ах і проти налаштованої частоти нічого не
        // рахує, тож у цю пастку не влазить, — але в лог обидві варто
        // мати, бо розбіжність тут пояснює половину питань "чому мало
        // кадрів".
        int fps_requested = 0;
        double fps_live = 0;

        int bitrate_kbit = 0;
        double gop_s = 0;

        // КУДИ КАМЕРА ШЛЕ. Один рядок, що відповідає на питання, яке
        // інакше вимагає tcpdump: юнікаст на адресу станції означає, що
        // пакет дійде РІВНО ОДНОМУ споживачеві з кількох, що слухають
        // порт, — бо фан-аут ядро робить лише для бродкасту й мультикасту.
        bool outgoing_enabled = false;
        std::string outgoing_server;

        Phase phase;
    };

    explicit CameraApi(Config cfg);

    // АДРЕСА МІНЯЄТЬСЯ НА ХОДУ. Її визначають не з конфіга, а з того,
    // звідки реально приходить відео, — камера переїхала, тракт іде за
    // нею. Викликати можна лише з того потоку, що робить запити (у нас
    // це потік PhaseController); окремого захисту тут немає навмисно.
    void set_host(const std::string& host) { cfg_.host = host; dialect_ = Dialect::kAuto; }
    const std::string& host() const { return cfg_.host; }

    // GET /api/v1/phase — читання. Блокуючий, до io_timeout_ms.
    Reply read_phase();

    // GET /api/v1/phase?millihz=<signed> — запис.
    Reply set_phase(int millihz);

    // GET /api/v1/status.
    Status read_status();

    // GET /api/v1/idr — примусовий ключовий кадр.
    Reply force_idr();

    // Статистика для діагностики: скільки запитів пішло і скільки впало.
    uint64_t requests() const { return requests_; }
    uint64_t failures() const { return failures_; }

private:
    // Низькорівневий GET: з'єднання, запит, читання до закриття. Повертає
    // код HTTP (0 — не з'єдналося або нема відповіді), raw-відповідь у
    // out_raw. requests_++ рахується саме тут, тож і новий, і старий
    // контракт обліковані однаково.
    int http_get(const std::string& path, std::string* out_raw);

    // Один GET і розібраний JSON-конверт НОВОГО контракту. `path` з параметрами.
    Reply call(const std::string& path, std::string* out_body);

    // СТАРИЙ КОНТРАКТ (камери до JSON-API): GET /api/v1/set?video0.
    // fpsTrimMilliHz=<v>, успіх = HTTP 200. Стану не повертає (has_phase=
    // false) — а петля це вже вміє: пише абсолют, не читаючи, і при
    // невідомих межах бере власний ліміт.
    Reply set_legacy(int millihz);

    // ДІАЛЕКТ КАМЕРИ. kAuto — ще не знаємо. Перша ж відповідь без JSON-
    // конверта (HTTP є, тіла контракту нема) ставить kLegacy, валідний
    // конверт — kNew; далі стукаємо лише у відповідний endpoint, а не в
    // обидва щоразу. Скидається на kAuto при зміні хоста (інша камера).
    enum class Dialect { kAuto, kNew, kLegacy };
    void note_legacy();   // перемкнутися на старий контракт (один раз, з логом)
    void lock_new();      // зафіксувати новий контракт (з kAuto)
    Dialect dialect_ = Dialect::kAuto;

    Config cfg_;
    uint64_t requests_ = 0;
    uint64_t failures_ = 0;
};

} // namespace vrx::control
