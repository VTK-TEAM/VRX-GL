// vrx_supervisor — хто вирішує, що зараз на екрані.
//
// НАВІЩО ОКРЕМА ПРОГРАМА. Станція, редактор розкладки і все, що з'явиться
// далі, не можуть працювати одночасно: DRM master ексклюзивний, і той,
// хто його тримає, тримає екран цілком. Отже потрібен хтось, хто екрана
// не тримає взагалі, а лише запускає по одному й чекає.
//
// Раніше це робив shell усередині run.sh. Поки сценарій був один
// ("підняти станцію"), цього вистачало; щойно з'являється другий, у
// скрипті починається розгалуження, яке ніхто не перечитає через місяць.
// Тут — звичайний автомат станів, і кожен новий режим це один case.
//
// ПРОТОКОЛ — КОД ВИХОДУ, і більше нічого. Ні сокетів, ні файлів-
// прапорців, ні сигналів. Станція, натомість щоб самій щось запускати,
// просто виходить із домовленим числом:
//
//     10           "хочу редактор розкладки" — піднімаємо його, чекаємо,
//                  повертаємо станцію
//     будь-що інше піднімаємо станцію знову
//
// ВИХОДУ "САМ ПО СОБІ" В НАГЛЯДАЧА НЕМАЄ. Станція завершилась із нулем,
// упала з сигналом, віддала невідомий код — реакція одна: підняти її
// назад. Причина проста: на станції немає ні термінала, ні робочого
// стола, і "нічого не показувати" там не є станом, з якого можна вийти.
// Єдиний спосіб зупинити наглядача — зупинити ЙОГО (systemctl stop).
//
// Причина саме така: поки процес станції живий, він тримає DRM master, і
// редактор однаково не отримав би екрана. Тобто передати керування можна
// РІВНО в момент виходу — і код виходу є природним способом сказати,
// кому саме.
//
// ЩО ЦЕ ДАЄ НА ГОЛІЙ СИСТЕМІ. Наглядач — єдине, що треба покласти в
// автозапуск (systemd-юніт). Він переживає і падіння станції, і вихід із
// редактора, і не залежить ні від графічної сесії, ні від термінала.

#define _GNU_SOURCE
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// --- домовлені коди виходу станції ---
#define EXIT_RUN_EDITOR 10

// --- пауза між перезапусками після збою: 1, 2, 4, 8 секунд ---
//
// Та сама логіка, що й у сторожів усередині станції: стала помилка (немає
// монітора, зайнятий порт, не збирається пайплайн) не виправляється
// негайним повторенням, вона виправляється РІДШИМ повторенням. Без паузи
// наглядач крутив би запуск десятки разів на секунду й залив би лог.
#define RESTART_MS_MIN 1000
#define RESTART_MS_MAX 8000

// Скільки має прожити станція, щоб вважатись здоровою. Прожила довше —
// збій разовий, лічильник обнуляємо.
#define HEALTHY_MS 20000

static volatile sig_atomic_t g_stop = 0;
static volatile pid_t g_child = 0;

static void on_signal(int sig) {
    g_stop = 1;
    // Дитину глушимо тим самим сигналом: станція вміє коректно закритись
    // по SIGTERM — дописати файл запису, повернути камері нейтральний
    // тримінг, розібрати кільце буферів.
    if (g_child > 0) kill(g_child, sig);
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// Корінь проєкту від САМОГО СЕБЕ, а не від поточного каталогу.
//
// Наглядача запускає systemd, а той стартує з "/". Шляхи до станції,
// редактора, атласа й конфігу — відносні, тож робочий каталог мусить
// бути коренем проєкту, інакше станція підніметься без телеметрії, і
// мовчки. Той самий прийом, що й у запускача: /proc/self/exe.
static int find_root(char* out, size_t n) {
    char exe[PATH_MAX];
    const ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0) return -1;
    exe[len] = '\0';

    // <корінь>/build/vrx_supervisor -> два рівні вгору.
    char* dir = dirname(exe);           // <корінь>/build
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    char* root = dirname(tmp);          // <корінь>
    snprintf(out, n, "%s", root);
    return 0;
}

// Запускає програму й чекає. Повертає код виходу, або -1 якщо процес
// убито сигналом (це не збій станції, це нас зупиняють).
static int run_and_wait(const char* what, char* const argv[], char* const envp_extra[]) {
    fprintf(stderr, "[наглядач] запускаю %s\n", what);
    fflush(stderr);

    const pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[наглядач] fork не вдався: %s\n", strerror(errno));
        return -2;
    }
    if (pid == 0) {
        // Дитина. Власна група процесів: інакше Ctrl+C у терміналі
        // прилетів би і їй, і нам одночасно, і хто кого зупинить —
        // питання випадку.
        setpgid(0, 0);
        for (int i = 0; envp_extra && envp_extra[i]; ++i) putenv(envp_extra[i]);
        execv(argv[0], argv);
        fprintf(stderr, "[наглядач] не запустилось %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    g_child = pid;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) break;
    }
    g_child = 0;

    if (WIFSIGNALED(status)) {
        // Сигнал — це або падіння (SIGSEGV, SIGABRT), або хтось убив
        // процес ззовні. Для наглядача це РІВНО ТАКИЙ САМИЙ збій, як
        // ненульовий код виходу: екран лишився без картинки, і його треба
        // повернути. Відрізняти ці випадки нема сенсу — реакція одна.
        //
        // Спершу я тут виходив із циклу, і наглядач помирав разом зі
        // станцією: тобто рівно в тому випадку, заради якого він і
        // потрібен. Вихід лишився лише один — коли зупиняють НАС самих,
        // і це видно по g_stop.
        fprintf(stderr, "[наглядач] %s зупинено сигналом %d\n", what, WTERMSIG(status));
        return -1;
    }
    const int rc = WEXITSTATUS(status);
    fprintf(stderr, "[наглядач] %s вийшов, код %d\n", what, rc);
    return rc;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    // Дочірній процес може вмерти будь-коли — це нормальний хід подій,
    // а не аварія.
    signal(SIGPIPE, SIG_IGN);

    char root[PATH_MAX];
    if (find_root(root, sizeof(root)) != 0) {
        fprintf(stderr, "[наглядач] не визначив корінь проєкту\n");
        return 1;
    }
    if (chdir(root) != 0) {
        fprintf(stderr, "[наглядач] не перейшов у %s: %s\n", root, strerror(errno));
        return 1;
    }
    fprintf(stderr, "[наглядач] корінь проєкту: %s\n", root);

    char station[PATH_MAX], editor[PATH_MAX];
    snprintf(station, sizeof(station), "%s/build/vrx_gl", root);
    snprintf(editor, sizeof(editor), "%s/build/osd_editor", root);

    int fail_streak = 0;

    while (!g_stop) {
        char* st_argv[] = { station, NULL };
        const long started = now_ms();
        const int rc = run_and_wait("станцію", st_argv, NULL);
        const long lived = now_ms() - started;

        if (g_stop) break;                      // зупиняють нас

        if (rc == EXIT_RUN_EDITOR) {
            // РЕДАКТОР ЖИВЕ У ВЛАСНОМУ X-СЕРВЕРІ.
            //
            // Не в робочому столі: на станції його немає й не буде.
            // xinit піднімає голий X рівно на час роботи редактора й
            // прибирає його одразу після виходу — тобто екран повертається
            // станції без жодних слідів.
            //
            // vt7 і :1 задані явно: наглядач може працювати з systemd, де
            // ні термінала, ні "поточної" консолі не існує.
            char* ed_argv[] = {
                (char*)"/usr/bin/xinit", editor, (char*)"--",
                (char*)":1", (char*)"-nolisten", (char*)"tcp", (char*)"vt7", NULL
            };
            char* ed_env[] = { (char*)"VRX_EDITOR_FULLSCREEN=1", NULL };
            run_and_wait("редактор розкладки", ed_argv, ed_env);
            // Чим би він не закінчився — вертаємо станцію. Редактор, що
            // впав, не привід лишити пілота з чорним екраном.
            fail_streak = 0;
            continue;
        }

        // Решта — усе інше: нуль, ненульовий код, смерть від сигналу.
        // Розрізняти нема чого, реакція одна — підняти назад.
        if (lived > HEALTHY_MS) fail_streak = 0;
        if (fail_streak < 4) fail_streak++;

        long back = RESTART_MS_MIN;
        for (int i = 1; i < fail_streak && back < RESTART_MS_MAX; ++i) back *= 2;
        if (back > RESTART_MS_MAX) back = RESTART_MS_MAX;

        fprintf(stderr, "[наглядач] станція прожила %ld мс, %d-й збій поспіль,"
                        " піднімаю через %ld мс\n", lived, fail_streak, back);
        for (long slept = 0; slept < back && !g_stop; slept += 100) sleep_ms(100);
    }

    fprintf(stderr, "[наглядач] зупинився\n");
    return 0;
}
