/* Dev tool where there is no Windows ([debug] sampler=1): where the game thread is. Once a
 * second a signal stops it; its handler notes the program counter, the link register and the
 * return addresses up the frame-pointer chain, and this thread logs them by symbol (the
 * generated functions are exported, so dladdr names them). For a thread stuck in a loop the
 * same few lines repeat. The thread that calls sampler_start first is the one sampled. */
#ifndef _WIN32
#include <windows.h>
#include <dlfcn.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#include "log.h"
#include "sampler.h"

#define DEPTH 10

static pthread_t g_target;
static sem_t g_taken;
static uintptr_t g_frames[DEPTH + 2];
static int g_count;

static void on_signal(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = (ucontext_t *)uc_;
    int n = 0;
    (void)sig; (void)si;
#ifdef __aarch64__
    {
        uintptr_t fp = (uintptr_t)uc->uc_mcontext.regs[29];
        g_frames[n++] = (uintptr_t)uc->uc_mcontext.pc;
        g_frames[n++] = (uintptr_t)uc->uc_mcontext.regs[30];
        /* each frame record: the caller's frame pointer, then the return address; stop at
           anything that doesn't look like one (a frame higher up, aligned) */
        while (n < DEPTH + 2 && fp && !(fp & 15)) {
            uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];
            g_frames[n++] = ret;
            if (next <= fp || next - fp > (1u << 20))
                break;
            fp = next;
        }
    }
#else
    (void)uc;
#endif
    g_count = n;
    sem_post(&g_taken);
}

static void name_of(uintptr_t pc, char *out, size_t size)
{
    Dl_info d;
    if (dladdr((void *)pc, &d) && d.dli_sname)
        snprintf(out, size, "%s+%X", d.dli_sname, (unsigned)(pc - (uintptr_t)d.dli_saddr));
    else if (dladdr((void *)pc, &d) && d.dli_fname)
        snprintf(out, size, "%s!%llX", strrchr(d.dli_fname, '/') ? strrchr(d.dli_fname, '/') + 1 : d.dli_fname,
                 (unsigned long long)(pc - (uintptr_t)d.dli_fbase));
    else
        snprintf(out, size, "%llX", (unsigned long long)pc);
}

static void *sampler_thread(void *p)
{
    unsigned k;
    (void)p;
    for (k = 0;; k++) {
        char line[1024], name[160];
        size_t len = 0;
        int i;
        struct timespec ts;
        sleep(k < 30 ? 1 : 10);
        if (pthread_kill(g_target, SIGUSR2))
            return NULL;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        if (sem_timedwait(&g_taken, &ts))
            continue;
        for (i = 0; i < g_count && len < sizeof line - 200; i++) {
            name_of(g_frames[i], name, sizeof name);
            len += (size_t)snprintf(line + len, sizeof line - len, "%s%s", i ? " < " : "", name);
        }
        log_printf("sampler: %s", line);
    }
}

void sampler_start(void)
{
    static int started;
    struct sigaction sa;
    pthread_t t;
    if (started++)
        return;
    g_target = pthread_self();
    sem_init(&g_taken, 0, 0);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
    if (pthread_create(&t, NULL, sampler_thread, NULL) == 0) {
        pthread_detach(t);
        log_printf("sampler: where the game thread is, once a second");
    }
}
#else
typedef int sampler_posix_unused;   /* Windows: sampler.c */
#endif
