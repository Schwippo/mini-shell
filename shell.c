// mini_shell_handler.c
// gcc -Wall -Wextra -O2 -o mini_shell_handler mini_shell_handler.c

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>      // fork, execvp, getcwd, chdir, setpgid
#include <sys/types.h>   // pid_t
#include <sys/wait.h>    // waitpid, WUNTRACED, WCONTINUED
#include <signal.h>      // signal, kill, SIG*
#include <errno.h>
#include <termios.h>     // tcsetpgrp, tcgetpgrp

static pid_t shell_pgid;
static int   shell_is_interactive = 0;

// Ein Handler für "Kind hat sich geändert" (gestoppt, fortgesetzt, beendet)
static void signal_handler(int sig) {
    if (sig != SIGCHLD) return;
    int saved = errno;
    int status;
    pid_t pid;
    // Nicht blockierend & Statusänderungen mitnehmen:
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        if (WIFSTOPPED(status)) {
            // Kind wurde z.B. per Ctrl+Z angehalten
            fprintf(stderr, "[gestoppt: PID %d]\n", pid);
        } else if (WIFCONTINUED(status)) {
            // Kind läuft wieder (SIGCONT)
            fprintf(stderr, "[weiter:   PID %d]\n", pid);
        } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
            // Beendet -> stillschweigend aufgeräumt (kein Zombie)
            // fprintf(stderr, "[ende:     PID %d]\n", pid);
        }
    }
    errno = saved;
}

static void give_terminal_to(pid_t pgid) {
    if (shell_is_interactive) {
        tcsetpgrp(STDIN_FILENO, pgid);
    }
}

int main(void) {
    shell_is_interactive = isatty(STDIN_FILENO);
    if (shell_is_interactive) {
        shell_pgid = getpid();
        // eigene Prozessgruppe für die Shell
        if (setpgid(shell_pgid, shell_pgid) == -1 && errno != EACCES) perror("setpgid(shell)");
        give_terminal_to(shell_pgid);

        // Shell soll nicht von Ctrl+C / Ctrl+Z gestoppt werden
        signal(SIGINT,  SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);

        // Unser EINZIGER Handler: Kindstatus verarbeiten
        signal(SIGCHLD, signal_handler);
    }

    char line[256], *args[32], cwd[256], answer[8];

    while (1) {
        printf("sh> "); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\nWirklich beenden (y/n)? ");
            if (fgets(answer, sizeof(answer), stdin) && (answer[0]=='y'||answer[0]=='Y'||answer[0]=='j'||answer[0]=='J'))
                break;
            else
                continue;
        }
        line[strcspn(line, "\n")] = '\0';
        if (!line[0]) continue;

        // Tokenize (nur Leerzeichen)
        int i = 0;
        for (char *t = strtok(line, " "); t && i < 31; t = strtok(NULL, " ")) args[i++] = t;
        args[i] = NULL;
        if (!args[0]) continue;

        // Hintergrund?
        int background = 0;
        if (i > 0 && strcmp(args[i-1], "&") == 0) { background = 1; args[--i] = NULL; if (!i) continue; }

        // Built-Ins
        if (strcmp(args[0], "pwd") == 0) {
            if (getcwd(cwd, sizeof(cwd))) puts(cwd); else perror("pwd");
            continue;
        }
        if (strcmp(args[0], "cd") == 0) {
            if (i < 2) fprintf(stderr, "cd: Pfad angeben\n");
            else if (chdir(args[1]) == 0) { // Wechsel erfolgreich
                char cwd[256];
                if (getcwd(cwd, sizeof(cwd)) != NULL) { // neuen Pfad holen
                    printf("Neues Verzeichnis: %s\n", cwd);
                } else {
                    perror("getcwd");
                }
            }
            perror("cd");
            continue;
        }
        if (strcmp(args[0], "exit") == 0) {
            printf("Wirklich beenden (y/n)? ");
            if (fgets(answer, sizeof(answer), stdin) && (answer[0]=='y'||answer[0]=='Y'||answer[0]=='j'||answer[0]=='J')) break;
            else continue;
        }
        if (strcmp(args[0], "stop") == 0) {            // BG- oder beliebigen Prozess anhalten
            if (i < 2) fprintf(stderr, "stop: PID angeben\n");
            else if (kill((pid_t)atoi(args[1]), SIGTSTP) == -1) perror("stop");
            continue;
        }
        if (strcmp(args[0], "const") == 0) {           // fortsetzen
            if (i < 2) fprintf(stderr, "const: PID angeben\n");
            else if (kill((pid_t)atoi(args[1]), SIGCONT) == -1) perror("const");
            continue;
        }

        // Externes Programm starten
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); continue; }

        if (pid == 0) {
            // Kind: eigene Prozessgruppe (PGID = PID)
            setpgid(0, 0);

            // FG: Terminalrechte ans Kind geben und Default-Signalverhalten herstellen
            if (!background && shell_is_interactive) {
                give_terminal_to(getpid());
                signal(SIGINT,  SIG_DFL);
                signal(SIGTSTP, SIG_DFL);
                signal(SIGQUIT, SIG_DFL);
                signal(SIGTTIN, SIG_DFL);
                signal(SIGTTOU, SIG_DFL);
            }

            execvp(args[0], args);
            perror("execvp");
            _exit(127);
        } else {
            // Eltern: PGID setzen (Absicherung)
            setpgid(pid, pid);
            printf("[PID: %d]%s\n", pid, background ? " (Background)" : "");

            if (!background) {
                // Vordergrund: Terminal ans Kind & warten bis beendet oder gestoppt
                give_terminal_to(pid);
                int status = 0;
                if (waitpid(pid, &status, WUNTRACED) == -1) perror("waitpid");
                // Hinweis: Die Ausgabe „[gestoppt: PID ...]“ übernimmt unser signal_handler().
                // Terminal an Shell zurück
                give_terminal_to(shell_pgid);
            }
            // Hintergrund: kein wait() hier; SIGCHLD-Handler räumt auf und meldet Status
        }
    }
    return 0;
}