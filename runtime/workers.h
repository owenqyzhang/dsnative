/* dsnative runtime: a few helper threads for work that splits into independent jobs (the 3D
 * rasterizer's screen bands). The jobs of one run must not depend on each other or share what they
 * write, so the result never depends on the number of threads or on which thread ran a job. */
#ifndef WORKERS_H
#define WORKERS_H

typedef void (*WorkFn)(void *arg, int job);

/* Threads to use in all, the caller's included: 1 means none (the jobs run in the caller). The default
 * is the number of cores, at most 4. */
void workers_set_threads(int n);
int workers_threads(void);

/* Runs fn(arg, 0) .. fn(arg, jobs - 1), on the helper threads and the caller, and returns when all are done. */
void workers_run(int jobs, WorkFn fn, void *arg);
#endif
