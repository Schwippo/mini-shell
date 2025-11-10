#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>       // fork, execvp, chdir, getcwd, pipe
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

// MQ/ Pthread-Add-on
#include <mqueue.h>
#include <pthread.h>
#include <fcntl.h>        // O_RDONLY

// Line Definition
#define MAX_LINE 256
#define MAX_ARGS 32

// MQ/ Pthread-Add-on globales
// POSIX Message Queue Handle
static mqd_t g_mq = (mqd_t)-1;

// letzter bekannter CPU-Lastwert; -1 = noch nichts empfangen
static int current_cpu_load = -1;

// Synchronisation für current_cpu_load
static pthread_mutex_t g_cpu_mtx = PTHREAD_MUTEX_INITIALIZER;

// Thread-Steuerung
static int g_mq_thread_running = 0;   // 1 = Thread-Schleife läuft
static int g_mq_thread_started = 0;   // 1 = Thread wurde erfolgreich gestartet
static pthread_t g_mq_tid;

// Signal-Handler
void signal_handler(int sig) {
    switch (sig) {
        case SIGINT:   // Strg + C
            printf("\n(SIGINT empfangen – Shell bleibt aktiv. Zum Beenden 'exit' verwenden)\n");
            printf("sh> ");
            fflush(stdout);
            break;
        case SIGTSTP:  // Strg + Z
            printf("\n(SIGTSTP empfangen – ignoriert)\n");
            printf("sh> ");
            fflush(stdout);
            break;
        case SIGTERM:
            printf("\n(SIGTERM empfangen – ignoriert)\n");
            break;
        case SIGCONT:
            printf("\n(SIGCONT empfangen – Shell fortgesetzt)\n");
            break;
        case SIGKILL:
            // SIGKILL kann NICHT vom Programm selbst behandelt werden
            // (wird immer vom Kernel sofort ausgeführt)
            printf("\n(SIGKILL kann nicht abgefangen werden)\n");
            break;
        default:
            printf("\n(Unbekanntes Signal %d empfangen)\n", sig);
            break;
    }
}

// MQ/Pthread-Add-on
static void *mq_listener(void *arg) {
    (void)arg;
    // Blockierendes Lesen aus der MQ; robust gegen EINTR
    while (__atomic_load_n(&g_mq_thread_running, __ATOMIC_RELAXED)) {
        char buf[64];
        unsigned int prio = 0;
        ssize_t n = mq_receive(g_mq, buf, sizeof(buf), &prio);
        if (n >= 0) {
            buf[sizeof(buf)-1] = '\0';
            int val = atoi(buf);
            if (val < 0)   val = 0;
            if (val > 100) val = 100;

            pthread_mutex_lock(&g_cpu_mtx);
            current_cpu_load = val;
            pthread_mutex_unlock(&g_cpu_mtx);
        } else {
            if (errno == EINTR) continue;   // unterbrochen durch Signal → weiter
            // Bei anderen Fehlern kurz „atmen“, nicht crashen
            usleep(100000);
        }
    }
    return NULL;
}

static void mq_start_if_available(void) {
    // Ohne O_CREAT, damit die Shell auch ohne cpuloadd einfach weiterläuft.
    g_mq = mq_open("/cpuload", O_RDONLY);
    if (g_mq == (mqd_t)-1) {
        fprintf(stderr, "[Hinweis] /cpuload nicht verfügbar (cpuloadd läuft?). CPU-Anzeige = n/a\n");
        return;
    }

    // Listener starten
    g_mq_thread_running = 1;
    if (pthread_create(&g_mq_tid, NULL, mq_listener, NULL) != 0) {
        fprintf(stderr, "[Warnung] Konnte MQ-Listener-Thread nicht starten.\n");
        g_mq_thread_running = 0;
        mq_close(g_mq);
        g_mq = (mqd_t)-1;
        return;
    }
    g_mq_thread_started = 1;
    // Detach optional, hier  joinbar für sauberes Beenden lassen
}

static void mq_stop_and_close(void) {
    if (g_mq_thread_started) {
        g_mq_thread_running = 0;
        // Wakeup: mq_receive blockiert; wir könnten ein Dummy-Signal schicken
        // aber ein kurzer Timeout/Signal reicht hier -> Notfalls minimal warten
        // sleep, dann join
        usleep(200000);
        pthread_cancel(g_mq_tid);      // falls noch blockiert
        pthread_join(g_mq_tid, NULL);
        g_mq_thread_started = 0;
    }
    if (g_mq != (mqd_t)-1) {
        mq_close(g_mq);
        g_mq = (mqd_t)-1;
    }
}

// Hilfsfunktionen
// entfernt das abschließende '\n' von fgets
void trim_newline(char *s) {
    size_t n = strlen(s);
    if (n && s[n - 1] == '\n')
        s[n - 1] = '\0';
}

// Zerlegt eine Befehlszeile in Tokens
int parse_line(char *line, char *args[]) {
    int i = 0;
    char *token = strtok(line, " \t");
    while (token != NULL && i < MAX_ARGS - 1) {
        args[i++] = token;
        token = strtok(NULL, " \t");
    }
    args[i] = NULL;
    return i;
}

// Prüft, ob Prozess im Hintergrund laufen soll ("&")
int is_background(char *args[], int argc) {
    if (argc > 0 && strcmp(args[argc - 1], "&") == 0) {
        args[argc - 1] = NULL;
        return 1;
    }
    return 0;
}

// Built-In-Befehle
int run_builtin(char *args[]) {
    if (strcmp(args[0], "pwd") == 0) {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd)) != NULL)
            printf("%s\n", cwd);
        else
            perror("pwd");
        return 1;
    }

    if (strcmp(args[0], "cd") == 0) {
        if (args[1] == NULL)
            fprintf(stderr, "cd: Pfad fehlt\n");
        else if (chdir(args[1]) != 0)
            perror("cd");
        return 1;
    }

    if (strcmp(args[0], "exit") == 0) {
        char ans[8];
        printf("Shell wirklich beenden? (y/n): ");
        if (fgets(ans, sizeof(ans), stdin) && ans[0] == 'y') {
            printf("Shell wird beendet.\n");
            exit(0);
        }
        return 1;
    }

    return 0;
}

// Prozess starten (Foreground / Background)
void run_process(char *args[], int background) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        // Kindprozess
        execvp(args[0], args);
        perror("execvp");
        exit(1);
    } else {
        // Elternprozess
        printf("[PID %d] gestartet%s\n", pid, background ? " (Hintergrund)" : "");
        if (!background) {
            int status;
            waitpid(pid, &status, 0);
        }
    }
}

// Zwei Prozesse mit Pipe verbinden (cmd1 | cmd2)
void run_pipe(char *left[], char *right[]) {
    int fd[2];
    if (pipe(fd) == -1) {
        perror("pipe");
        return;
    }

    pid_t pid1 = fork();
    if (pid1 < 0) {
        perror("fork");
        return;
    }

    if (pid1 == 0) {
        // Kind 1 (Producer)
        dup2(fd[1], STDOUT_FILENO);
        close(fd[0]);
        close(fd[1]);
        execvp(left[0], left);
        perror("execvp left");
        exit(1);
    }

    pid_t pid2 = fork();
    if (pid2 < 0) {
        perror("fork");
        return;
    }

    if (pid2 == 0) {
        // Kind 2 (Consumer)
        dup2(fd[0], STDIN_FILENO);
        close(fd[0]);
        close(fd[1]);
        execvp(right[0], right);
        perror("execvp right");
        exit(1);
    }

    // Elternprozess
    close(fd[0]);
    close(fd[1]);
    printf("[Pipe] Prozesse %d → %d gestartet\n", pid1, pid2);

    int status;
    waitpid(pid1, &status, 0);
    waitpid(pid2, &status, 0);
}

// Hauptprogramm
int main(void) {
    // Signalbehandlung aktivieren
    signal(SIGINT, signal_handler);
    signal(SIGTSTP, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGCONT, signal_handler);

    // MQ/ Pthread-Add-on -> Versuchen, die MQ zu öffnen und Listener zu starten
    mq_start_if_available();

    char line[MAX_LINE];
    char *args[MAX_ARGS];
    char cwd[256];

    printf("Willkommen in der Mini-Shell (mit Signals, Background & Pipes)\n");

    while (1) {
        // Prompt: Pfad + CPU-Last
        int load_snapshot;
        pthread_mutex_lock(&g_cpu_mtx);
        load_snapshot = current_cpu_load; // -1 = n/a
        pthread_mutex_unlock(&g_cpu_mtx);

        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            if (load_snapshot >= 0) printf("%s [CPU %d%%]> ", cwd, load_snapshot);
            else                    printf("%s [CPU n/a]> ",   cwd);
        } else {
            if (load_snapshot >= 0) printf("sh [CPU %d%%]> ", load_snapshot);
            else                    printf("sh [CPU n/a]> ");
        }
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin))
            break;
        trim_newline(line);

        if (strlen(line) == 0)
            continue;

        // Prüfen auf Pipe
        char *pipe_pos = strchr(line, '|');
        if (pipe_pos != NULL) {
            *pipe_pos = '\0';
            char *left_cmd = line;
            char *right_cmd = pipe_pos + 1;
            while (*right_cmd == ' ') right_cmd++;

            char *left_args[MAX_ARGS];
            char *right_args[MAX_ARGS];
            parse_line(left_cmd, left_args);
            parse_line(right_cmd, right_args);

            if (left_args[0] && right_args[0])
                run_pipe(left_args, right_args);
            else
                fprintf(stderr, "Fehlerhafte Pipe-Syntax.\n");
            continue;
        }

        // Normales Parsing
        int argc = parse_line(line, args);
        if (argc == 0)
            continue;

        // Built-In-Befehle
        if (run_builtin(args))
            continue;

        // Hintergrund prüfen
        int background = is_background(args, argc);

        // Externes Programm starten
        run_process(args, background);
    }

    // Sauber aufräumen, falls REPL verlassen wurde (EOF/ Fehler)
    mq_stop_and_close();

    printf("Shell beendet.\n");
    return 0;
}