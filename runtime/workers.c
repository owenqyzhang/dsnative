/* dsnative runtime: helper threads (workers.h). One pool for the process, started at the first run that
 * wants it. All state is under one mutex: the jobs are hundreds of microseconds each, so taking it per
 * job costs nothing, and its hand-offs give the caller the helpers' writes. */
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include "workers.h"

static struct {
    pthread_mutex_t mu;
    pthread_cond_t wake, done;
    int threads;                /* wanted, the caller included; 0 until first asked */
    int started;                /* helpers running */
    WorkFn fn;
    void *arg;
    int jobs, next, pending;    /* the run in progress: jobs, the next to take, those not yet finished */
} W = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0, 0, 0};

static int default_threads(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : n > 4 ? 4 : (int)n;
}

void workers_set_threads(int n) {
    pthread_mutex_lock(&W.mu);
    W.threads = n < 1 ? 1 : n > 16 ? 16 : n;
    pthread_mutex_unlock(&W.mu);
}

int workers_threads(void) {
    pthread_mutex_lock(&W.mu);
    if (!W.threads) W.threads = default_threads();
    int n = W.threads;
    pthread_mutex_unlock(&W.mu);
    return n;
}

static void *helper(void *unused) {
    (void)unused;
    pthread_mutex_lock(&W.mu);
    for (;;) {
        while (W.next >= W.jobs) pthread_cond_wait(&W.wake, &W.mu);
        int j = W.next++;
        WorkFn fn = W.fn; void *arg = W.arg;
        pthread_mutex_unlock(&W.mu);
        fn(arg, j);
        pthread_mutex_lock(&W.mu);
        if (--W.pending == 0) pthread_cond_signal(&W.done);
    }
    return 0;
}

void workers_run(int jobs, WorkFn fn, void *arg) {
    int threads = workers_threads();
    if (threads <= 1 || jobs <= 1) {
        for (int j = 0; j < jobs; j++) fn(arg, j);
        return;
    }
    pthread_mutex_lock(&W.mu);
    while (W.started < threads - 1) {   /* start the helpers: detached, they live as long as the process */
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        int ok = pthread_create(&t, &at, helper, 0) == 0;
        pthread_attr_destroy(&at);
        if (!ok) break;
        W.started++;
    }
    W.fn = fn; W.arg = arg; W.jobs = jobs; W.next = 0; W.pending = jobs;
    pthread_cond_broadcast(&W.wake);
    while (W.next < W.jobs) {           /* the caller takes jobs too */
        int j = W.next++;
        pthread_mutex_unlock(&W.mu);
        fn(arg, j);
        pthread_mutex_lock(&W.mu);
        W.pending--;
    }
    while (W.pending) pthread_cond_wait(&W.done, &W.mu);
    W.jobs = W.next = 0;
    pthread_mutex_unlock(&W.mu);
}
