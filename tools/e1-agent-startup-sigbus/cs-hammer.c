// cs-hammer: prove/disprove mutual exclusion of the PAL critical section
// implementation inside libChakraCore.dylib (the engine's CriticalSection).
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>

typedef void (*enter_t)(void *, void *);
typedef void (*leave_t)(void *, void *);
typedef void (*initcs_t)(void *);
typedef void *(*curthread_t)(void);

static enter_t p_enter;
static leave_t p_leave;
static initcs_t p_initcs;
static curthread_t p_curthread;
static void *g_cs;
static volatile int in_cs = 0, max_in_cs = 0, viol = 0;
static volatile long long ops = 0;

static void *hammer(void *unused) {
  (void)unused;
  void *thr = p_curthread ? p_curthread() : NULL;
  for (int i = 0; i < 30000 && !viol; i++) {
    p_enter(thr, g_cs);
    int now = __sync_add_and_fetch(&in_cs, 1);
    if (now > max_in_cs) max_in_cs = now;
    if (now > 1) viol = 1;
    __sync_fetch_and_add(&ops, 1);
    __sync_sub_and_fetch(&in_cs, 1);
    p_leave(thr, g_cs);
  }
  return NULL;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "./bin-pre/libChakraCore.dylib";
  int nthreads = argc > 2 ? atoi(argv[2]) : 16;
  void *h = dlopen(path, RTLD_NOW);
  if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
  p_initcs = (initcs_t)dlsym(h, "InitializeCriticalSection");
  p_enter = (enter_t)dlsym(h, "_ZN7CorUnix28InternalEnterCriticalSectionEPNS_10CPalThreadEP17_CRITICAL_SECTION");
  // InternalLeaveCriticalSection has the same shape
  p_leave = (leave_t)dlsym(h, "_ZN7CorUnix28InternalLeaveCriticalSectionEPNS_10CPalThreadEP17_CRITICAL_SECTION");
  p_curthread = (curthread_t)dlsym(h, "InternalGetCurrentThread");
  printf("initcs=%p enter=%p leave=%p curthread=%p\n", (void*)p_initcs, (void*)p_enter, (void*)p_leave, (void*)p_curthread);
  if (!p_initcs || !p_enter || !p_leave) { printf("symbols missing\n"); return 1; }

  g_cs = calloc(1, 256);
  p_initcs(g_cs);

  pthread_t t[64];
  for (int i = 0; i < nthreads; i++) pthread_create(&t[i], NULL, hammer, NULL);
  for (int i = 0; i < nthreads; i++) pthread_join(t[i], NULL);
  printf("threads=%d ops=%lld max_in_cs=%d VIOLATION=%s\n",
         nthreads, ops, max_in_cs, viol ? "YES — MUTUAL EXCLUSION BROKEN" : "no");
  return viol ? 2 : 0;
}
