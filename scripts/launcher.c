// Крихітний запускач: єдине, що робить — знаходить себе на диску й
// передає керування scripts/run.sh поруч.
//
// НАВІЩО ВІН ВЗАГАЛІ ПОТРІБЕН. Файловий менеджер XFCE не виконує кліком
// ні .desktop у папці (віддає його редактору ярликів XFCE), ні шелл-
// скрипти (типове налаштування "виконувати скрипти" вимкнене, файл іде
// в текстовий редактор). Обидва варіанти перевірені на живій системі й
// виглядають однаково: "вікно мигнуло" або відкрився текст.
//
// А ось звичайний ДВІЙКОВИЙ файл Thunar виконує. Тому запускач
// скомпільований, хоча всієї роботи в ньому — знайти сусідній скрипт.
//
// Шлях береться з /proc/self/exe, а не з argv[0] і не з поточного
// каталогу: файловий менеджер може запустити нас звідки завгодно, і
// єдине надійне джерело власного розташування — ядро.

#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    char self[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) {
        fprintf(stderr, "Не вдалося визначити власне розташування\n");
        return 1;
    }
    self[n] = '\0';

    // dirname() може міняти буфер, тож віддаємо йому копію.
    char copy[PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", self);
    const char* dir = dirname(copy);

    char script[PATH_MAX];
    if ((size_t)snprintf(script, sizeof(script), "%s/scripts/run.sh", dir) >= sizeof(script)) {
        fprintf(stderr, "Завеликий шлях\n");
        return 1;
    }

    // Аргументи передаємо далі як є: колись знадобиться --no-phase.
    char* args[64];
    int k = 0;
    args[k++] = (char*)"bash";
    args[k++] = script;
    for (int i = 1; i < argc && k < 62; ++i) args[k++] = argv[i];
    args[k] = NULL;

    execvp("bash", args);
    perror("bash");
    return 1;
}
