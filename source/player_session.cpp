#include "source/player_session.hpp"
#include "record/snapshot.hpp"

#include <cstdio>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace vrx::source {
namespace {
// ІМЕНА КАНАЛІВ У ФАЙЛАХ. Вони ж потрапляють у назви записів, у журнал і
// в назви знімків.
const char* kNames[PlayerSession::kChannels] = {"main", "sub", "local"};
const PlaybackSource::Codec kCodecs[PlayerSession::kChannels] = {
    PlaybackSource::Codec::H265,     // основний — з борту
    PlaybackSource::Codec::MJPEG,    // PiP
    PlaybackSource::Codec::MJPEG,    // захват із камери
};
}

PlayerSession::PlayerSession() {
    for (int i = 0; i < kChannels; ++i) {
        PlaybackSource::Config c;
        c.channel = kNames[i];
        c.codec = kCodecs[i];
        ch_[i] = std::make_shared<PlaybackSource>(c);
    }
}

std::shared_ptr<FrameSource> PlayerSession::source(int i) const {
    return (i >= 0 && i < kChannels) ? ch_[i] : nullptr;
}

void PlayerSession::adopt() {
    length_us_.store(ix_.length_us(), std::memory_order_relaxed);
    start_wall_us_.store(ix_.start_us(), std::memory_order_relaxed);
    live_.store(ix_.live(), std::memory_order_relaxed);
}

bool PlayerSession::open(const std::string& journal_path, int64_t t_us) {
    for (auto& c : ch_) c->stop();
    if (!ix_.load(journal_path)) return false;
    journal_ = journal_path;
    adopt();

    // Невдача каналу — НЕ привід кидати сеанс: канал міг узагалі не
    // писатись (не було сигналу, вимкнений захват), і це нормальний запис,
    // просто з двома доріжками замість трьох. Джерело лишається на місці й
    // просто мовчить.
    for (int i = 0; i < kChannels; ++i)
        ch_[i]->open(ix_, t_us);

    // Лог телеметрії лежить поруч із журналом і зветься так само. Немає —
    // не біда: сеанси, записані до появи логу, просто йдуть без телеметрії.
    tlm_ok_ = tlm_.open(ix_.dir() + "/session_" + ix_.id() + "_tlm.bin");
    std::fprintf(stderr, "[плеєр] телеметрія сеансу %s: %s\n",
                 ix_.id().c_str(), tlm_ok_ ? "є" : "немає (старий запис)");

    open_ = true;
    position_us_.store(t_us, std::memory_order_relaxed);
    last_tick_ns_ = 0;
    push_target();
    return true;
}

void PlayerSession::close() {
    for (auto& c : ch_) c->stop();     // джерела лишаються, кадри зникають
    open_ = false;

    // ЗАБУВАЄМО САМ СЕАНС, а не лише зупиняємо подачу.
    //
    // Інакше довжина лишалась ненульовою, і плеєр при наступному вході
    // показував старий таймлайн замість списку — виглядало так, ніби вибір
    // не працює й уперто вмикається той самий запис.
    tlm_.close();
    tlm_ok_ = false;
    ix_ = record::SessionIndex{};
    journal_.clear();
    length_us_.store(0, std::memory_order_relaxed);
    start_wall_us_.store(0, std::memory_order_relaxed);
    live_.store(false, std::memory_order_relaxed);
    position_us_.store(0, std::memory_order_relaxed);
    last_tick_ns_ = 0;
    last_reload_ns_ = 0;
}

FrameSource* PlayerSession::channel(int i) {
    if (i < 0 || i >= kChannels) return nullptr;
    return ch_[i].get();
}

const char* PlayerSession::channel_name(int i) const {
    return (i >= 0 && i < kChannels) ? kNames[i] : "";
}

int PlayerSession::save_snapshots(const std::string& dir) const {
    // Мить береться ОДНА на всі канали — та, що зараз на таймлайні.
    const int64_t wall = start_wall_us_.load(std::memory_order_relaxed) +
                         position_us_.load(std::memory_order_relaxed);

    std::vector<std::pair<std::string, SourceFrame>> frames;
    for (int i = 0; i < kChannels; ++i) {
        if (!ch_[i]) continue;
        SourceFrame f;
        if (ch_[i]->snapshot(f)) frames.push_back({kNames[i], std::move(f)});
    }
    return record::save_set(std::move(frames), dir, wall);
}

int64_t PlayerSession::timeline_len_us() const {
    const int64_t len = length_us_.load(std::memory_order_relaxed);
    if (!live_.load(std::memory_order_relaxed)) return len;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const int64_t now_wall = ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
    const int64_t edge = now_wall - start_wall_us_.load(std::memory_order_relaxed);
    return edge > len ? edge : len;
}

void PlayerSession::push_target() {
    for (auto& c : ch_) if (c) c->set_target(position_us_);
}

void PlayerSession::request_open(const std::string& journal_path) {
    std::lock_guard<std::mutex> lk(req_mtx_);
    req_open_ = journal_path;
    have_open_ = true;
}

void PlayerSession::request_seek(int64_t t_us) {
    std::lock_guard<std::mutex> lk(req_mtx_);
    req_seek_ = t_us;
    have_seek_ = true;
    req_jump_ = 0;              // явна ціль скасовує накопичені стрибки
}

void PlayerSession::request_jump(int64_t delta_us) {
    std::lock_guard<std::mutex> lk(req_mtx_);
    req_jump_ += delta_us;      // натиснули двічі — стрибок подвійний
}

void PlayerSession::request_step(int dir) {
    std::lock_guard<std::mutex> lk(req_mtx_);
    req_step_ += dir > 0 ? 1 : -1;
}

void PlayerSession::process_requests() {
    std::string op;
    bool do_open = false, do_seek = false;
    int64_t sk = 0, jp = 0;
    int st = 0;
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        if (have_open_) { op = req_open_; do_open = true; have_open_ = false; }
        if (have_seek_) { sk = req_seek_; do_seek = true; have_seek_ = false; }
        jp = req_jump_; req_jump_ = 0;
        st = req_step_; req_step_ = 0;
    }

    if (do_open) open(op, 0);
    if (!open_) return;

    // Порядок навмисний: спершу явна ціль, потім відносні зміщення від неї.
    if (do_seek) seek(sk);
    if (jp != 0) seek(position_us_.load(std::memory_order_relaxed) + jp);

    // Крок ставить паузу сам: натискають його саме тоді, коли хочуть
    // роздивитись, і змушувати спершу зупиняти — зайвий рух.
    if (st != 0 && speed_.load(std::memory_order_relaxed) > 0.0)
        speed_.store(0.0, std::memory_order_relaxed);
    for (; st > 0; --st) step(1);
    for (; st < 0; ++st) step(-1);
}

void PlayerSession::tick() {
    process_requests();
    if (!open_) return;          // закритий плеєр часу не рахує

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t now = ts.tv_sec * 1000000000LL + ts.tv_nsec;

    // ЖИВИЙ ЗАПИС РОСТЕ ПІД НОГАМИ. Журнал дописується далі, тож раз на
    // дві секунди перечитуємо його: інакше кінець таймлайну застиг би на
    // тому, яким запис був у мить відкриття, і догнати "зараз" було б ніяк.
    if (live_.load(std::memory_order_relaxed) && !journal_.empty() &&
        (last_reload_ns_ == 0 || now - last_reload_ns_ > 2000000000LL)) {
        last_reload_ns_ = now;
        record::SessionIndex fresh;
        if (fresh.load(journal_) && fresh.length_us() >= ix_.length_us()) {
            ix_ = std::move(fresh);
            adopt();
            for (auto& c : ch_) if (c) c->refresh(ix_);
        }
    }

    const double sp = speed_.load(std::memory_order_relaxed);
    if (last_tick_ns_ != 0 && sp > 0.0) {
        // Понад чверть секунди між тактами — це не сповільнений показ, а
        // зупинка: перемикання екрана, підняття пайплайна, затик носія.
        // Зараховувати такий провал у позицію означало б перескочити
        // шматок запису, якого глядач не бачив.
        int64_t dt = now - last_tick_ns_;
        if (dt > 250000000LL) dt = 0;
        position_us_.fetch_add((int64_t)(dt / 1000.0 * sp), std::memory_order_relaxed);
        // Межа — гладка: по ній позиція вже не смикається.
        const int64_t len = timeline_len_us();
        if (len > 0 && position_us_.load(std::memory_order_relaxed) > len)
            position_us_.store(len, std::memory_order_relaxed);
    }
    last_tick_ns_ = now;
    push_target();

    // Телеметрію ставимо на ту саму мить, що й кадри. Читач сам не робить
    // нічого, поки номер кадру не змінився, тож виклик щотакту дешевий.
    if (tlm_ok_)
        tlm_.fill(start_wall_us_.load(std::memory_order_relaxed) +
                      position_us_.load(std::memory_order_relaxed),
                  tlm_store_);
}

void PlayerSession::seek(int64_t t_us) {
    const int64_t len = timeline_len_us();
    position_us_.store(t_us < 0 ? 0 : (len > 0 && t_us > len ? len : t_us),
                       std::memory_order_relaxed);
    last_tick_ns_ = 0;

    // Усі канали на ту саму секунду СЕАНСУ. Кожен сам знайде свій файл і
    // свій байтовий зсув — у них різні ротації й різні розриви.
    const int64_t at = position_us_.load(std::memory_order_relaxed);
    for (auto& c : ch_) if (c) c->seek(ix_, at);
    push_target();
}

void PlayerSession::set_speed(double s) {
    speed_.store(s < 0 ? 0 : (s > 10.0 ? 10.0 : s), std::memory_order_relaxed);
}

void PlayerSession::step(int dir) {
    if (dir >= 0) {
        // Питаємо основний канал, коли в нього НАСТУПНИЙ кадр. Якщо його
        // немає (провал або черга ще порожня) — пробуємо решту, і лише
        // потім відступаємо до умовного кроку.
        int64_t next = 0;
        for (int i = 0; i < kChannels && next == 0; ++i)
            if (ch_[i]) next = ch_[i]->next_after(position_us_.load(std::memory_order_relaxed));
        position_us_.store(next > 0 ? next
                                    : position_us_.load(std::memory_order_relaxed) + 16667,
                           std::memory_order_relaxed);
        push_target();
        return;
    }
    // Назад наявних кадрів у черзі немає — вони вже позаду. Тому просто
    // відступаємо на кадр і перезбираємо подачу.
    seek(position_us_.load(std::memory_order_relaxed) - 16667);
}

void PlayerSession::jump(int64_t delta_us) {
    seek(position_us_.load(std::memory_order_relaxed) + delta_us);
}

} // namespace vrx::source
