// cpuloadd.c
// Reads real CPU load from /proc/stat and publishes it to POSIX MQ "/cpuload" every 10 s.
// Fallback: if /proc/stat isn't readable or parse fails repeatedly, switch to simulation.
//
// Force simulation via environment: CPULOAD_SIM=1
// Or compile-time: add -DUSE_SIMULATION
//
// Link: gcc -O2 -Wall -pthread cpuloadd.c -o cpuloadd -lrt

#define _GNU_SOURCE
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifndef MQ_NAME
#define MQ_NAME "/cpuload"
#endif

// ----- Simulation -----
static double simulated_cpu_load(void) {
    // Simple bounded random walk for smoother values.
    static int init = 0;
    static double v = 35.0;
    if (!init) { srand((unsigned)time(NULL)); init = 1; }
    double step = (rand() % 1101 - 550) / 100.0; // -5.50 .. +5.50
    v += step;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

// ----- Real measurement from /proc/stat -----
// Parse the aggregate "cpu" line. Fields: user nice system idle iowait irq softirq steal guest guest_nice
// We use the first 8 which are stable across kernels.
struct cpu_sample {
    unsigned long long user, nice_, system, idle, iowait, irq, softirq, steal;
};

static int read_cpu_sample(struct cpu_sample *s) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    // Read first non-empty line and ensure it starts with "cpu "
    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    if (strncmp(line, "cpu ", 4) != 0) return -1;

    // Parse 8 integers
    // guest fields ignored
    unsigned long long user, nice_, system, idle, iowait, irq, softirq, steal;
    int n = sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice_, &system, &idle, &iowait, &irq, &softirq, &steal);
    if (n < 4) return -1; // need at least total+idle
    s->user = user; s->nice_ = nice_; s->system = system; s->idle = idle;
    s->iowait = (n >= 5) ? iowait : 0;
    s->irq = (n >= 6) ? irq : 0;
    s->softirq = (n >= 7) ? softirq : 0;
    s->steal = (n >= 8) ? steal : 0;
    return 0;
}

static double cpu_usage_percent_once(int delay_sec) {
    struct cpu_sample a, b;
    if (read_cpu_sample(&a) != 0) return -1.0;
    sleep(delay_sec);
    if (read_cpu_sample(&b) != 0) return -1.0;

    unsigned long long idle_a = a.idle + a.iowait;
    unsigned long long idle_b = b.idle + b.iowait;

    unsigned long long nonidle_a = a.user + a.nice_ + a.system + a.irq + a.softirq + a.steal;
    unsigned long long nonidle_b = b.user + b.nice_ + b.system + b.irq + b.softirq + b.steal;

    unsigned long long total_a = idle_a + nonidle_a;
    unsigned long long total_b = idle_b + nonidle_b;

    unsigned long long totald = (total_b >= total_a) ? (total_b - total_a) : 0;
    unsigned long long idled  = (idle_b  >= idle_a)  ? (idle_b  - idle_a)  : 0;

    if (totald == 0) return -1.0;

    double used = (double)(totald - idled);
    double pct = 100.0 * used / (double)totald;
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    return pct;
}

static double get_cpu_load_real_or_sim(int *using_sim, int *fail_budget, int *sample_from_sim) {
#if defined(USE_SIMULATION)
    (void)fail_budget;
    *using_sim = 1;
    if (sample_from_sim) *sample_from_sim = 1;
    return simulated_cpu_load();
#else
    if (*using_sim) {
        if (sample_from_sim) *sample_from_sim = 1;
        return simulated_cpu_load();
    }
    // Try real measurement with a short baseline sleep to reduce jitter.
    double v = cpu_usage_percent_once(1);
    if (v < 0.0) {
        if (sample_from_sim) *sample_from_sim = 1;
        if (*fail_budget > 0) (*fail_budget)--;
        if (*fail_budget == 0) {
            *using_sim = 1; // Switch to simulation permanently until restart.
        }
        return simulated_cpu_load();
    }
    if (sample_from_sim) *sample_from_sim = 0;
    // Reset failure budget on success.
    *fail_budget = 3;
    return v;
#endif
}

int main(void) {
    // POSIX MQ setup
    struct mq_attr attr = {0};
    attr.mq_flags = 0;
    attr.mq_maxmsg = 8;
    attr.mq_msgsize = 64;

    mqd_t q = mq_open(MQ_NAME, O_CREAT | O_WRONLY, 0666, &attr);
    if (q == (mqd_t)-1) {
        perror("mq_open");
        return 1;
    }

    // Allow env override for simulation
    int using_sim = 0;
    const char *env_sim = getenv("CPULOAD_SIM");
    if (env_sim && *env_sim == '1') using_sim = 1;

    printf("[cpuloadd] mq_open OK for '%s'; starting in %s mode\n",
           MQ_NAME, using_sim ? "simulation" : "real");
    fflush(stdout);

    // Number of consecutive real-read failures before switching to simulation.
    int fail_budget = 3;

    char buf[64];
    for (;;) {
        int sample_from_sim = 0;
        double val = get_cpu_load_real_or_sim(&using_sim, &fail_budget, &sample_from_sim);
        printf("[cpuloadd] %s CPU load: %.1f%%\n",
               sample_from_sim ? "simulated" : "real", val);
        fflush(stdout);
        int n = snprintf(buf, sizeof(buf), "%.1f", val);
        if (n < 0) n = 0;

        if (mq_send(q, buf, (size_t)n + 1, 0) == -1) {
            perror("mq_send");
            // If the queue is full or absent, do not exit. Try next tick.
        }

        // Every 10 seconds as required.
        sleep(10);
    }

    // Not reached in daemon-style loop
    // mq_close(q); mq_unlink(MQ_NAME);
    return 0;
}

/*
Notes:
- If /proc/stat is unavailable (containers with restricted /proc, non-Linux), the code
  will automatically fall back to simulated values after 3 failed attempts.
- To keep pure-simulation behavior, compile with -DUSE_SIMULATION
  or run with CPULOAD_SIM=1.
- Mini shell must reads from the same MQ and prints the most recent value every 10 s.
*/
