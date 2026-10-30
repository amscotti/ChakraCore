// sigbus-trace: DYLD_INSERT_LIBRARIES diagnostic for the agent-startup SIGBUS.
// Records munmap / PROT_NONE-refault / vm_deallocate operations (with shallow
// backtraces + thread ids) in a lock-protected ring; installs a SA_SIGINFO
// SIGBUS/SIGSEGV handler that, on fault, prints siginfo, the faulting
// thread's backtrace, mach region state at si_addr, and every recorded op
// whose range covers si_addr (i.e. who unmapped the page), then _exit(66).
// Unhandled faults chain to the previous handler (PAL SEH).
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <execinfo.h>

#define DYLD_INTERPOSE(_replacement, _replacee)                          \
  static struct { const void* replacement; const void* replacee; }       \
  _interpose_##_replacee __attribute__((used, section("__DATA,__interpose"))) = { \
    (const void*)&_replacement, (const void*)&_replacee };

#define MAX_BT 14
#define RING 65536

typedef struct {
  char kind[24];       // "munmap" / "decommit-map" / "vm_deallocate" / "mprotect-none"
  uintptr_t start;
  size_t size;
  uint64_t tid;
  uint64_t ts;
  int nframes;
  void *frames[MAX_BT];
} oprec_t;

static oprec_t ring[RING];
static volatile unsigned ring_head = 0; // monotonically increasing index
static pthread_mutex_t log_mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile int ready = 0;
static struct sigaction prev_segv, prev_bus;
static volatile int in_fault = 0;

static uint64_t now_ns(void) {
  return (uint64_t)mach_absolute_time();
}

static void record2(const char *kind, uintptr_t a, uintptr_t b) {
  if (!ready) return;
  pthread_mutex_lock(&log_mtx);
  oprec_t *r = &ring[__sync_fetch_and_add(&ring_head, 1) & (RING - 1)];
  strlcpy(r->kind, kind, sizeof(r->kind));
  r->start = a; r->size = b;
  r->tid = (uint64_t)pthread_mach_thread_np(pthread_self());
  r->ts = now_ns();
  r->nframes = 0;
  pthread_mutex_unlock(&log_mtx);
}

static void record(const char *kind, uintptr_t start, size_t size, int with_bt) {
  if (!ready) return;
  if (__sync_lock_test_and_set(&in_fault, 1)) { __sync_lock_release(&in_fault); return; }
  pthread_mutex_lock(&log_mtx);
  oprec_t *r = &ring[__sync_fetch_and_add(&ring_head, 1) & (RING - 1)];
  strlcpy(r->kind, kind, sizeof(r->kind));
  r->start = start; r->size = size;
  r->tid = (uint64_t)pthread_mach_thread_np(pthread_self());
  r->ts = now_ns();
  r->nframes = with_bt ? backtrace(r->frames, MAX_BT) : 0;
  pthread_mutex_unlock(&log_mtx);
  __sync_lock_release(&in_fault);
}

static int munmap_hook_c(caddr_t addr, size_t len, void *ra) {
  (void)ra;
  record("munmap", (uintptr_t)addr, len, 1);
  return munmap(addr, len);
}
static int munmap_hook(void *addr, size_t len) {
  return munmap_hook_c((caddr_t)addr, len, __builtin_return_address(0));
}
DYLD_INTERPOSE(munmap_hook, munmap)

static void *mmap_hook(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
  if (len >= 0x4000) {
    if ((flags & MAP_FIXED) && (prot == PROT_NONE)) {
      record("decommit-map", (uintptr_t)addr, len, 0);
    } else {
      char kind[32];
      snprintf(kind, sizeof(kind), "mmap%s%s",
               (flags & MAP_FIXED) ? "-fixed" : "",
               (prot == PROT_NONE) ? "-none" : ((prot & PROT_WRITE) ? "-rw" : ""));
      record(kind, (uintptr_t)addr, len, 0);
    }
  }
  return mmap(addr, len, prot, flags, fd, off);
}
DYLD_INTERPOSE(mmap_hook, mmap)

static int mprotect_hook(void *addr, size_t len, int prot) {
  if (prot == PROT_NONE) {
    record("mprotect-none", (uintptr_t)addr, len, 1);
  }
  return mprotect(addr, len, prot);
}
DYLD_INTERPOSE(mprotect_hook, mprotect)

static kern_return_t vm_deallocate_hook(vm_map_t target, vm_address_t address, vm_size_t size) {
  if (target == mach_task_self()) {
    record("vm_deallocate", (uintptr_t)address, size, 1);
  }
  return vm_deallocate(target, address, size);
}
DYLD_INTERPOSE(vm_deallocate_hook, vm_deallocate)

static kern_return_t vm_allocate_hook(vm_map_t target, vm_address_t *address, vm_size_t size, int flags) {
  vm_address_t requested = *address;
  kern_return_t kr = vm_allocate(target, address, size, flags);
  if (kr == KERN_SUCCESS && target == mach_task_self() && size >= 0x4000) {
    record((flags & VM_FLAGS_ANYWHERE) ? "vm_allocate-any" : "vm_allocate-fix",
           (uintptr_t)*address, size, 0);
    (void)requested;
  }
  return kr;
}
DYLD_INTERPOSE(vm_allocate_hook, vm_allocate)

static void print_bt(void **frames, int n) {
  for (int i = 0; i < n; i++) {
    Dl_info info;
    char buf[512];
    if (dladdr(frames[i], &info) && info.dli_fname) {
      const char *base = strrchr(info.dli_fname, '/');
      base = base ? base + 1 : info.dli_fname;
      snprintf(buf, sizeof(buf), "%s%+lld", base,
               (long long)((uintptr_t)frames[i] - (uintptr_t)info.dli_fbase));
    } else {
      snprintf(buf, sizeof(buf), "%p", frames[i]);
    }
    fprintf(stderr, "    #%d %p  %s\n", i, frames[i], buf);
  }
}

static void dump_region(uintptr_t addr) {
  mach_vm_address_t vaddr = addr & ~0xfffULL;
  mach_vm_size_t vsize = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t obj;
  if (mach_vm_region(mach_task_self(), &vaddr, &vsize, VM_REGION_BASIC_INFO_64,
                     (vm_region_info_t)&info, &cnt, &obj) == KERN_SUCCESS) {
    fprintf(stderr, "  region containing-or-after fault: base=%llx size=%llx (fault is %s)\n",
            (unsigned long long)vaddr, (unsigned long long)vsize,
            vaddr <= addr ? "INSIDE a mapped region" : "in unmapped gap");
  } else {
    fprintf(stderr, "  no mach region at/after fault address (address space end?)\n");
  }
}

static uintptr_t resolve_sym(const char *imageSub, uintptr_t symOff) {
  for (uint32_t i = 0; i < _dyld_image_count(); i++) {
    const char *name = _dyld_get_image_name(i);
    if (name && strstr(name, imageSub)) {
      return (uintptr_t)_dyld_get_image_vmaddr_slide(i) + symOff;
    }
  }
  return 0;
}

static const char *trace_image(void) {
  const char *img = getenv("SIGBUS_TRACE_IMAGE");
  return img ? img : "libChakraCore.dylib";
}

static void dump_obj_ops(const char *what, uintptr_t lo, uintptr_t hi) {
  pthread_mutex_lock(&log_mtx);
  unsigned head = ring_head;
  int found = 0;
  for (unsigned i = 1; i <= RING; i++) {
    oprec_t *r = &ring[(head - i) & (RING - 1)];
    if (r->kind[0] == 0) break;
    uintptr_t rend = r->start + r->size;
    if (r->start < hi && rend > lo) {
      found++;
      fprintf(stderr, "  [%u] %s-COVER %s range=%p..%p size=0x%zx tid=%llu ts=%llu\n",
              head - i, what, r->kind, (void *)r->start, (void *)rend, r->size, r->tid, r->ts);
    }
  }
  if (!found) fprintf(stderr, "  (%s: no recorded op covers %p..%p)\n", what, (void *)lo, (void *)hi);
  pthread_mutex_unlock(&log_mtx);
}

static void dump_pal_regions(uintptr_t faultCardAddr) {
  // Resolve image slide and walk the PAL PCMI list
  // (symbol offsets from nm; override per binary via env).
  uintptr_t SYM_pVirtualMemory = 0xa0dc58;
  uintptr_t SYM_cardTable = 0xb4b310;
  const char *e;
  if ((e = getenv("SIGBUS_TRACE_PVMOFF")) != NULL) SYM_pVirtualMemory = strtoull(e, NULL, 16);
  if ((e = getenv("SIGBUS_TRACE_CTOFF")) != NULL) SYM_cardTable = strtoull(e, NULL, 16);
  uintptr_t slide = 0;
  int foundImg = 0;
  for (uint32_t i = 0; i < _dyld_image_count(); i++) {
    const char *name = _dyld_get_image_name(i);
    if (name && strstr(name, trace_image())) {
      slide = (uintptr_t)_dyld_get_image_vmaddr_slide(i);
      foundImg = 1;
      break;
    }
  }
  if (!foundImg) { fprintf(stderr, "  (trace image %s not found)\n", trace_image()); return; }
  typedef struct _CMIW { struct _CMIW *pNext; struct _CMIW *pLast; uintptr_t startBoundary; size_t memSize; unsigned accessProtection, allocationType; unsigned char *pAllocState, *pProtectionState; } CMIW;
  CMIW *list = *(CMIW **)(slide + SYM_pVirtualMemory);
  unsigned char *cardTable = *(unsigned char **)(slide + SYM_cardTable);
  fprintf(stderr, "  PAL cardTable=%p (fault card addr=%p cardIndex=0x%llx -> heapObj=0x%llx)\n",
          cardTable, (void *)faultCardAddr,
          (unsigned long long)(faultCardAddr - (uintptr_t)cardTable),
          (unsigned long long)(((uintptr_t)faultCardAddr - (uintptr_t)cardTable) << 14));
  // 1. PCMI region containing the faulting card address
  // 2. PCMI regions containing the implied heap object address
  uintptr_t heapObj = ((uintptr_t)faultCardAddr - (uintptr_t)cardTable) << 14;
  fprintf(stderr, "  heap-object section probe: %p..%p\n", (void *)heapObj, (void *)(heapObj + 0x10000000));
  {
    mach_vm_address_t vaddr = heapObj;
    mach_vm_size_t vsize = 0;
    vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
    if (mach_vm_region(mach_task_self(), &vaddr, &vsize, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) == KERN_SUCCESS && vaddr <= heapObj) {
      fprintf(stderr, "  mach region at heapObj: base=%llx size=%llx prot=%x (heapObj %s)\n",
              (unsigned long long)vaddr, (unsigned long long)vsize, info.protection,
              (vaddr + vsize > heapObj) ? "INSIDE" : "AT-EDGE");
    } else {
      fprintf(stderr, "  mach region at heapObj: unmapped or starts after\n");
    }
  }
  dump_obj_ops("heapobj", heapObj, heapObj + 0x1000);
  // Manager diag: _lastSegment*/_stackbase/_stacklimit (X64_WB_DIAG)
  {
    uintptr_t mgr = slide + 0xb4b258;
    uint64_t *q = (uint64_t *)mgr;
    fprintf(stderr, "  mgr diag:");
    for (int k = 10; k < 20; k++) fprintf(stderr, " [+%02x]=%llx", k * 8, (unsigned long long)q[k]);
    fprintf(stderr, "\n");
  }
  // All card-table-range ops ever recorded (who committed which card sections)
  {
    pthread_mutex_lock(&log_mtx);
    unsigned head = ring_head;
    fprintf(stderr, "  --- ALL card-range ops (cardTable..+0x100000) ---\n");
    for (unsigned i = 1; i <= RING && i <= head; i++) {
      oprec_t *r = &ring[(head - i) & (RING - 1)];
      if (r->kind[0] == 0) break;
      if (r->start >= (uintptr_t)cardTable && r->start < (uintptr_t)cardTable + 0x100000) {
        fprintf(stderr, "  [%u] %s range=%p..%p size=0x%zx tid=%llu ts=%llu\n",
                head - i, r->kind, (void *)r->start, (void *)(r->start + r->size), r->size, r->tid, r->ts);
      }
    }
    pthread_mutex_unlock(&log_mtx);
  }
  int n = 0;
  for (CMIW *e = list; e && n < 4000; e = e->pNext, n++) {
    int hits = 0;
    if (faultCardAddr >= e->startBoundary && faultCardAddr < e->startBoundary + e->memSize) hits |= 1;
    if (heapObj >= e->startBoundary && heapObj < e->startBoundary + e->memSize) hits |= 2;
    if (!hits) continue;
    fprintf(stderr, "  PCMI[%p] region=%p..%p size=0x%zx hits=%s%s pAllocState=%p\n",
            (void *)e, (void *)e->startBoundary, (void *)(e->startBoundary + e->memSize), e->memSize,
            (hits & 1) ? "CARD" : "", (hits & 2) ? " HEAP" : "", (void *)e->pAllocState);
    uintptr_t probe[2] = { faultCardAddr, heapObj };
    const char *pn[2] = { "card-page", "heap-page" };
    for (int k = 0; k < 2; k++) {
      if (!(hits & (1 << k))) continue;
      size_t pgIdx = (probe[k] - e->startBoundary) / 0x4000;
      fprintf(stderr, "    %s idx=%zu PALcommit-bits around:", pn[k], pgIdx);
      for (size_t j = (pgIdx >= 8 ? pgIdx - 8 : 0); j < pgIdx + 8 && j < e->memSize / 0x4000; j++) {
        fprintf(stderr, " [%zu]=%d", j, (e->pAllocState[j / 8] >> (j % 8)) & 1);
      }
      fprintf(stderr, "\n");
    }
  }
  if (n >= 4000) fprintf(stderr, "  (PCMI list >4000 entries, truncated)\n");
  // 3. Actually-committed card pages via mach (first 0x100000 of card table)
  mach_vm_address_t va = (mach_vm_address_t)cardTable;
  mach_vm_size_t vsz = 0;
  fprintf(stderr, "  card-table mach regions (first 1MB):");
  int printed = 0;
  while (printed < 24) {
    vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
    if (mach_vm_region(mach_task_self(), &va, &vsz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) break;
    if (va >= (mach_vm_address_t)cardTable + 0x100000) break;
    fprintf(stderr, " [%llx+%llx prot=%x]", (unsigned long long)va, (unsigned long long)vsz, info.protection);
    printed++;
    va += vsz;
  }
  fprintf(stderr, "\n");
}

static void fault_handler(int sig, siginfo_t *si, void *uc) {
  // Reentrancy guard: if our own reporting faults, die silently.
  static volatile int handling = 0;
  if (__sync_lock_test_and_set(&handling, 1)) {
    // Cannot safely report; restore default and re-raise.
    signal(sig, SIG_DFL);
    return;
  }
  if (sig == SIGTERM || sig == SIGINT) {
    // Driver kill: dump card-range ops summary, then die.
    fprintf(stderr, "\n=== sigbus-trace SIGTERM dump pid=%d ===\n", getpid());
    uintptr_t cardTable = 0;
    {
      const char *e = getenv("SIGBUS_TRACE_CTOFF");
      uintptr_t off = e ? strtoull(e, NULL, 16) : 0xb4b310;
      for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *name = _dyld_get_image_name(i);
        if (name && strstr(name, trace_image())) {
          cardTable = *(uintptr_t *)((uintptr_t)_dyld_get_image_vmaddr_slide(i) + off);
          break;
        }
      }
    }
    pthread_mutex_lock(&log_mtx);
    unsigned head = ring_head;
    fprintf(stderr, "  cardTable=%p ring head=%u; ALL card-range ops:\n", (void *)cardTable, head);
    for (unsigned i = 1; i <= RING && i <= head; i++) {
      oprec_t *r = &ring[(head - i) & (RING - 1)];
      if (r->kind[0] == 0) break;
      if (cardTable && r->start >= cardTable && r->start < cardTable + 0x100000) {
        fprintf(stderr, "  [%u] %s range=%p..%p size=0x%zx tid=%llu\n",
                head - i, r->kind, (void *)r->start, (void *)(r->start + r->size), r->size, r->tid);
      }
      if (strncmp(r->kind, "stk", 3) == 0) {
        fprintf(stderr, "  [%u] %s a=%p b=0x%zx tid=%llu\n",
                head - i, r->kind, (void *)r->start, r->size, r->tid);
      }
    }
    pthread_mutex_unlock(&log_mtx);
    // Dump X64WriteBarrierCardTableManager diag block (X64_WB_DIAG=1):
    // _cardTable, _cardTableNumEntries, CS, _lastSegmentAddress..., _stackbase/_stacklimit
    {
      const char *e2 = getenv("SIGBUS_TRACE_MGROFF");
      uintptr_t mgroff = e2 ? strtoull(e2, NULL, 16) : 0xb4b258;
      for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *name = _dyld_get_image_name(i);
        if (name && strstr(name, trace_image())) {
          uintptr_t mgr = (uintptr_t)_dyld_get_image_vmaddr_slide(i) + mgroff;
          uint64_t *q = (uint64_t *)mgr;
          fprintf(stderr, "  mgr@%p:", (void *)mgr);
          for (int k = 0; k < 20; k++) fprintf(stderr, " [+%02x]=%llx", k * 8, (unsigned long long)q[k]);
          fprintf(stderr, "\n");
          break;
        }
      }
    }

    fflush(stderr);
    _exit(sig == SIGTERM ? 143 : 130);
  }
  void *frames[MAX_BT];
  int n = backtrace(frames, MAX_BT);
  ucontext_t *ctx = (ucontext_t *)uc;
  fprintf(stderr, "\n=== sigbus-trace CAUGHT sig=%d si_code=%d si_addr=%p pc=%llx tid=%llu ===\n",
          sig, si->si_code, si->si_addr,
          (unsigned long long)ctx->uc_mcontext->__ss.__pc,
          (unsigned long long)pthread_mach_thread_np(pthread_self()));
  {
    arm_thread_state64_t *s = &ctx->uc_mcontext->__ss;
    fprintf(stderr, "  regs: x0=%llx x1=%llx x2=%llx x3=%llx x4=%llx x5=%llx\n",
            (unsigned long long)s->__x[0], (unsigned long long)s->__x[1], (unsigned long long)s->__x[2],
            (unsigned long long)s->__x[3], (unsigned long long)s->__x[4], (unsigned long long)s->__x[5]);
    fprintf(stderr, "        x6=%llx x7=%llx x8=%llx x9=%llx x10=%llx x11=%llx\n",
            (unsigned long long)s->__x[6], (unsigned long long)s->__x[7], (unsigned long long)s->__x[8],
            (unsigned long long)s->__x[9], (unsigned long long)s->__x[10], (unsigned long long)s->__x[11]);
    fprintf(stderr, "        x12=%llx x13=%llx x14=%llx x15=%llx x16=%llx x17=%llx\n",
            (unsigned long long)s->__x[12], (unsigned long long)s->__x[13], (unsigned long long)s->__x[14],
            (unsigned long long)s->__x[15], (unsigned long long)s->__x[16], (unsigned long long)s->__x[17]);
    fprintf(stderr, "        x18=%llx lr=%llx sp=%llx fp=%llx\n",
            (unsigned long long)s->__x[18], (unsigned long long)s->__lr,
            (unsigned long long)s->__sp, (unsigned long long)s->__fp);
    {
      // print pc/lr as offsets from the trace image base for offline symbolication
      for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *name = _dyld_get_image_name(i);
        if (name && strstr(name, trace_image())) {
          uintptr_t b = (uintptr_t)_dyld_get_image_header(i);
          fprintf(stderr, "  image %s base=%p pcOff=%llx lrOff=%llx\n", name, (void *)b,
                  (unsigned long long)((uintptr_t)s->__pc - b),
                  (unsigned long long)((uintptr_t)s->__lr - b));
          break;
        }
      }
    }
    // Peek at memory around x0 (barrier target) if readable
    uintptr_t x0 = (uintptr_t)s->__x[0];
    mach_vm_address_t vaddr = x0 & ~0xfffull;
    mach_vm_size_t vsize = 0;
    vm_region_basic_info_data_64_t rinfo; mach_msg_type_number_t rcnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t robj;
    if (mach_vm_region(mach_task_self(), &vaddr, &vsize, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&rinfo, &rcnt, &robj) == KERN_SUCCESS &&
        vaddr <= x0 && vaddr + vsize > x0 && (rinfo.protection & VM_PROT_READ)) {
      const uint64_t *p = (const uint64_t *)(x0 & ~0x7ull);
      fprintf(stderr, "  mem@x0 %p..%p:", (void *)(p - 4), (void *)(p + 6));
      for (int i = -4; i <= 6; i++) fprintf(stderr, " %llx", (unsigned long long)p[i]);
      fprintf(stderr, "\n  x0 region: base=%llx size=%llx prot=%x\n",
              (unsigned long long)vaddr, (unsigned long long)vsize, rinfo.protection);
    } else {
      fprintf(stderr, "  x0=%p not in a readable region\n", (void *)x0);
    }
  }
  print_bt(frames, n);
  dump_region((uintptr_t)si->si_addr);
  dump_pal_regions((uintptr_t)si->si_addr);

  pthread_mutex_lock(&log_mtx);
  unsigned head = ring_head;
  fprintf(stderr, "  --- recorded ops covering/near si_addr (ring head=%u) ---\n", head);
  int found = 0;
  for (unsigned i = 1; i <= RING; i++) {
    oprec_t *r = &ring[(head - i) & (RING - 1)];
    if (r->kind[0] == 0) break;
    uintptr_t end = r->start + r->size;
    uintptr_t f = (uintptr_t)si->si_addr;
    long dist = -1;
    if (f >= r->start && f < end) dist = 0;
    else if (f < r->start && (long)(r->start - f) < 0x400000) dist = (long)(r->start - f);
    else if (f >= end && (long)(f - end) < 0x400000) dist = (long)(f - end);
    if (dist == 0) {
      found++;
      fprintf(stderr, "  [%u] COVERING %s range=%p..%p size=0x%zx tid=%llu ts=%llu\n",
              head - i, r->kind, (void *)r->start, (void *)end, r->size, r->tid, r->ts);
      print_bt(r->frames, r->nframes);
    }
  }
  if (!found) fprintf(stderr, "  (no recorded op covers si_addr)\n");
  fprintf(stderr, "  --- last 12 ops (tail) ---\n");
  for (unsigned i = 1; i <= 12 && i <= head; i++) {
    oprec_t *r = &ring[(head - i) & (RING - 1)];
    if (r->kind[0] == 0) break;
    fprintf(stderr, "  [%u] %s range=%p..%p size=0x%zx tid=%llu ts=%llu\n",
            head - i, r->kind, (void *)r->start, (void *)(r->start + r->size), r->size, r->tid, r->ts);
  }
  pthread_mutex_unlock(&log_mtx);
  fflush(stderr);
  _exit(66);
  (void)handling;
}

static size_t get_stacksize_hook(pthread_t t) {
  return pthread_get_stacksize_np(t);
}
DYLD_INTERPOSE(get_stacksize_hook, pthread_get_stacksize_np)

static void *probe_thread(void *arg) {
  // Call the engine's exported OnThreadInit on this fresh secondary thread,
  // so we can observe (in the mmap ring) whether it commits this thread's
  // stack card sections.
  void (*onThreadInit)() = (void (*)())arg;
  mach_vm_address_t vaddr = (mach_vm_address_t)&arg & ~0xfffull;
  mach_vm_size_t vsize = 0;
  vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
  char where[64] = "unknown";
  if (mach_vm_region(mach_task_self(), &vaddr, &vsize, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) == KERN_SUCCESS) {
    snprintf(where, sizeof(where), "%llx+%llx", (unsigned long long)vaddr, (unsigned long long)vsize);
  }
  static volatile int seq = 0;
  int mine = __sync_add_and_fetch(&seq, 1);
  if (mine % 25 == 1) fprintf(stderr, "[probe] thread#%d stack~%s\n", mine, where);
  onThreadInit();
  return NULL;
}

static void *probe_deferred(void *unused);

__attribute__((constructor)) static void init_trace(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = fault_handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigfillset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &prev_segv);
  sigaction(SIGBUS, &sa, &prev_bus);
  // On SIGTERM/SIGINT (driver kill), dump the card-range ops for offline analysis.
  struct sigaction st;
  memset(&st, 0, sizeof(st));
  st.sa_sigaction = fault_handler;
  st.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGTERM, &st, NULL);
  sigaction(SIGINT, &st, NULL);
  memset(ring, 0, sizeof(ring));
  ready = 1;
  fprintf(stderr, "[sigbus-trace] active pid=%d\n", getpid());
  // Deferred probe: after the engine dylib loads, resolve and call
  // OnThreadInit on a fresh secondary thread.
  pthread_t pt;
  pthread_create(&pt, NULL, probe_deferred, NULL);
  pthread_detach(pt);
}

static void *probe_deferred(void *unused) {
  (void)unused;
  if (getenv("SIGBUS_TRACE_PROBE") == NULL) return NULL; // probe wave is opt-in
  for (int i = 0; i < 200; i++) {
    usleep(20000); // total ~4s
    void *fn = NULL;
    for (uint32_t k = 0; k < _dyld_image_count(); k++) {
      const char *name = _dyld_get_image_name(k);
      if (name && strstr(name, "libChakraCore.dylib")) {
        fn = dlsym(RTLD_DEFAULT, "ZN6Memory27RecyclerWriteBarrierManager12OnThreadInitEv");
        break;
      }
    }
    if (fn == NULL) {
      for (uint32_t k = 0; k < _dyld_image_count(); k++) {
        const char *name = _dyld_get_image_name(k);
        if (name && strstr(name, "libChakraCore.dylib")) {
          void *h = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
          if (h) fn = dlsym(h, "_ZN6Memory27RecyclerWriteBarrierManager12OnThreadInitEv");
          break;
        }
      }
    }
    if (fn) {
      // Spawn many threads so their stacks march down across card-section
      // boundaries; each calls OnThreadInit. If OnThreadInit works for
      // secondary threads, a card commit must appear for the first stack
      // that lands in an uncommitted section.
      for (int t = 0; t < 700; t++) {
        pthread_t pt2;
        pthread_create(&pt2, NULL, probe_thread, fn);
      }
      sleep(3); // let them run; stacks stay alive until process end
      fprintf(stderr, "[sigbus-trace] probe wave done\n");
      return NULL;
    }
  }
  fprintf(stderr, "[sigbus-trace] probe never found OnThreadInit\n");
  return NULL;
}

