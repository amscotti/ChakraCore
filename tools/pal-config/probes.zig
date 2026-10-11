//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//! Probe sources for the B1.3 PAL configuration checks.
//!
//! The `run_*` bodies are verbatim copies of the `check_cxx_source_runs` sources in
//! `pal/src/configure.cmake` (CMake escapes converted to plain quotes; defects included per
//! doc/pal-config-contract-mac.md section 5 item 3), validated byte-identical by the
//! sweep harness before conversion. `func_tmpl` is CMake 4.4.0's
//! `Modules/CheckFunctionExists.c` verbatim (BSD 3-clause, copyright Kitware Inc. and
//! contributors; see https://cmake.org/licensing).

pub const func_tmpl =
    \\#ifdef CHECK_FUNCTION_EXISTS
    \\
    \\#  ifdef __cplusplus
    \\extern "C"
    \\#  endif
    \\  char
    \\  CHECK_FUNCTION_EXISTS(void);
    \\#  ifdef __CLASSIC_C__
    \\int main()
    \\{
    \\  int ac;
    \\  char* av[];
    \\#  else
    \\int main(int ac, char* av[])
    \\{
    \\#  endif
    \\  CHECK_FUNCTION_EXISTS();
    \\  if (ac > 1000) {
    \\    return *av[0];
    \\  }
    \\  return 0;
    \\}
    \\
    \\#else /* CHECK_FUNCTION_EXISTS */
    \\
    \\#  error "CHECK_FUNCTION_EXISTS has to specify the function"
    \\
    \\#endif /* CHECK_FUNCTION_EXISTS */
        // Explicit trailing newline: Zig multiline literals carry no final \n,
        // while the CMake source ends with one (433 bytes total).
        ++ "\n";

/// `configure.cmake:121-139`; contract expectation: 0.
pub const run_realpath_supports_nonexistent_files =
    \\
    \\#include <sys/param.h>
    \\#include <stdlib.h>
    \\
    \\int main(void) {
    \\  char *path;
    \\#ifdef PATH_MAX
    \\  char resolvedPath[PATH_MAX];
    \\#elif defined(MAXPATHLEN)
    \\  char resolvedPath[MAXPATHLEN];
    \\#else
    \\  char resolvedPath[1024];
    \\#endif
    \\  path = realpath("a_nonexistent_file", resolvedPath);
    \\  if (path == NULL) {
    \\    exit(1);
    \\  }
    \\  exit(0);
    \\}
;

/// `configure.cmake:140-148`; contract expectation: 1.
pub const run_sscanf_support_ll =
    \\
    \\#include <stdio.h>
    \\#include <stdlib.h>
    \\int main(void)
    \\{
    \\  long long n = 0;
    \\  sscanf("5000000000", "%qu", &n);
    \\  exit (n != 5000000000);
    \\  }
;

/// `configure.cmake:149-163`; contract expectation: 0.
pub const run_sscanf_cannot_handle_missing_exponent =
    \\
    \\#include <stdio.h>
    \\#include <stdlib.h>
    \\
    \\int main()
    \\{
    \\  int ret;
    \\  float f = 0;
    \\  char * strin = "12.34e";
    \\
    \\  ret = sscanf (strin, "%e", &f);
    \\  if (ret <= 0)
    \\    exit (0);
    \\  exit(1);
    \\}
;

/// `configure.cmake:164-175`; contract expectation: 1.
pub const run_have_large_snprintf_support =
    \\
    \\#include <stdio.h>
    \\#include <stdlib.h>
    \\
    \\int main(void) {
    \\  char buf[256] = { 0 };
    \\  snprintf(buf, 0x7fffffff, "%#x", 0x12345678);
    \\  if (buf[0] == 0x0) {
    \\    exit(1);
    \\  }
    \\  exit(0);
    \\}
;

/// `configure.cmake:176-228`; contract expectation: 0.
pub const run_have_broken_fifo_select =
    \\
    \\#include <stdio.h>
    \\#include <stdlib.h>
    \\#include <fcntl.h>
    \\#include <string.h>
    \\#include <sys/stat.h>
    \\#include <sys/types.h>
    \\#include <sys/time.h>
    \\#include <unistd.h>
    \\
    \\int main(void) {
    \\    int fd, numFDs;
    \\    fd_set readFDs, writeFDs, exceptFDs;
    \\    struct timeval time = { 0 };
    \\    char * filename = NULL;
    \\
    \\    filename = (char *)malloc(L_tmpnam * sizeof(char)); /* ok to leak this at exit */
    \\    if (NULL == filename) {
    \\      exit(0);
    \\    }
    \\
    \\    /* On some platforms (e.g. HP-UX) the multithreading c-runtime does not
    \\       support the tmpnam(NULL) semantics, and it returns NULL. Therefore
    \\       we need to use the tmpnam(pbuffer) version.
    \\    */
    \\    if (NULL == tmpnam(filename)) {
    \\      exit(0);
    \\    }
    \\    if (mkfifo(filename, S_IRWXU) != 0) {
    \\      if (unlink(filename) != 0) {
    \\        exit(0);
    \\      }
    \\      if (mkfifo(filename, S_IRWXU) != 0) {
    \\        exit(0);
    \\      }
    \\    }
    \\    fd = open(filename, O_RDWR | O_NONBLOCK);
    \\    if (fd == -1) {
    \\      exit(0);
    \\    }
    \\
    \\    FD_ZERO(&readFDs);
    \\    FD_ZERO(&writeFDs);
    \\    FD_ZERO(&exceptFDs);
    \\    FD_SET(fd, &readFDs);
    \\    numFDs = select(fd + 1, &readFDs, &writeFDs, &exceptFDs, &time);
    \\
    \\    close(fd);
    \\    unlink(filename);
    \\
    \\    /* numFDs is zero if select() works correctly */
    \\    exit(numFD==0);
    \\}
;

/// `configure.cmake:229-286`; contract expectation: 0.
pub const run_have_broken_fifo_kevent =
    \\
    \\#include <stdio.h>
    \\#include <stdlib.h>
    \\#include <unistd.h>
    \\#include <fcntl.h>
    \\#include <string.h>
    \\#include <errno.h>
    \\#include <sys/types.h>
    \\#include <sys/event.h>
    \\#include <sys/time.h>
    \\#include <sys/stat.h>
    \\
    \\int main(void)
    \\{
    \\  int ikq;
    \\  int iRet;
    \\  int fd;
    \\  struct kevent ke, keChangeList;
    \\  struct timespec ts = { 0, 0 };
    \\
    \\  char * filename = NULL;
    \\
    \\  filename = (char *)malloc(L_tmpnam * sizeof(char)); /* ok to leak this at exit */
    \\  if (NULL == filename)
    \\  {
    \\    exit(1);
    \\  }
    \\
    \\  /* On some platforms (e.g. HP-UX) the multithreading c-runtime does not
    \\     support the tmpnam(NULL) semantics, and it returns NULL. Therefore
    \\     we need to use the tmpnam(pbuffer) version.
    \\  */
    \\  if (NULL == tmpnam(filename)) {
    \\    exit(0);
    \\  }
    \\  if (mkfifo(filename, S_IRWXU) != 0) {
    \\    if (unlink(filename) != 0) {
    \\      exit(0);
    \\    }
    \\    if (mkfifo(filename, S_IRWXU) != 0) {
    \\      exit(0);
    \\    }
    \\  }
    \\  fd = open(filename, O_RDWR | O_NONBLOCK);
    \\  if (fd == -1) {
    \\    exit(0);
    \\  }
    \\
    \\  EV_SET(&keChangeList, fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    \\  ikq = kqueue();
    \\  iRet = kevent(ikq, &keChangeList, 1, &ke, 1, &ts);
    \\
    \\  close(fd);
    \\  unlink(filename);
    \\
    \\  /* iRet is zero is kevent() works correctly */
    \\  return(iRet==0);
    \\}
;

/// `configure.cmake:288-310` (links pthread); contract expectation: 1.
pub const run_have_sched_get_priority =
    \\
    \\#include <stdio.h>
    \\#include <stdlib.h>
    \\#include <pthread.h>
    \\#include <sched.h>
    \\
    \\int main(void)
    \\{
    \\  int policy;
    \\  struct sched_param schedParam;
    \\  int max_priority;
    \\  int min_priority;
    \\
    \\  if (0 != pthread_getschedparam(pthread_self(), &policy, &schedParam))
    \\  {
    \\    exit(1);
    \\  }
    \\
    \\  max_priority = sched_get_priority_max(policy);
    \\  min_priority = sched_get_priority_min(policy);
    \\
    \\  exit(-1 == max_priority || -1 == min_priority);
    \\}
;

/// `configure.cmake:311-323` (links pthread); contract expectation: 0.
pub const run_have_sched_getcpu =
    \\
    \\#include <stdlib.h>
    \\#include <sched.h>
    \\
    \\int main(void)
    \\{
    \\  if (sched_getcpu() >= 0)
    \\  {
    \\    exit(0);
    \\  }
    \\  exit(1);
    \\}
;

/// `configure.cmake:325-337`; contract expectation: 1.
pub const run_have_working_gettimeofday =
    \\
    \\#include <stdlib.h>
    \\#include <time.h>
    \\#include <sys/time.h>
    \\
    \\int main()
    \\{
    \\  int ret;
    \\  struct timeval tv;
    \\  ret = gettimeofday(&tv, NULL);
    \\
    \\  exit(ret);
    \\}
;

/// `configure.cmake:338-353`; contract expectation: 0.
pub const run_have_working_clock_gettime =
    \\
    \\#include <stdlib.h>
    \\#include <time.h>
    \\#include <sys/time.h>
    \\
    \\int main()
    \\{
    \\  int ret;
    \\#ifndef __APPLE__
    \\  struct timespec ts;
    \\  ret = clock_gettime(CLOCK_REALTIME, &ts);
    \\#else
    \\  ret = 1; // do not use clock_gettime on osx/ios (backward compatibility)
    \\#endif
    \\  exit(ret);
    \\}
;

/// `configure.cmake:354-369`; contract expectation: 0.
pub const run_have_clock_monotonic =
    \\
    \\#include <stdlib.h>
    \\#include <time.h>
    \\#include <sys/time.h>
    \\
    \\int main()
    \\{
    \\  int ret;
    \\#ifndef __APPLE__
    \\  struct timespec ts;
    \\  ret = clock_gettime(CLOCK_MONOTONIC, &ts);
    \\#else
    \\  ret = 1; // do not use clock_gettime on osx/ios (backward compatibility)
    \\#endif
    \\  exit(ret);
    \\}
;

/// `configure.cmake:370-385`; contract expectation: 0.
pub const run_have_clock_monotonic_coarse =
    \\
    \\#include <stdlib.h>
    \\#include <time.h>
    \\#include <sys/time.h>
    \\
    \\int main()
    \\{
    \\  int ret;
    \\#ifndef __APPLE__
    \\  struct timespec ts;
    \\  ret = clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    \\#else
    \\  ret = 1; // do not use clock_gettime on osx/ios (backward compatibility)
    \\#endif
    \\  exit(ret);
    \\}
;

/// `configure.cmake:386-397`; contract expectation: 1.
pub const run_have_mach_absolute_time =
    \\
    \\#include <stdlib.h>
    \\#include <mach/mach_time.h>
    \\
    \\int main()
    \\{
    \\  int ret;
    \\  mach_timebase_info_data_t timebaseInfo;
    \\  ret = mach_timebase_info(&timebaseInfo);
    \\  mach_absolute_time();
    \\  exit(ret);
    \\}
;

/// `configure.cmake:398-413`; contract expectation: 0.
pub const run_have_clock_thread_cputime =
    \\
    \\#include <stdlib.h>
    \\#include <time.h>
    \\#include <sys/time.h>
    \\
    \\int main()
    \\{
    \\  int ret;
    \\#ifndef __APPLE__
    \\  struct timespec ts;
    \\  ret = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    \\#else
    \\  ret = 1; // do not use clock_gettime on osx/ios (backward compatibility)
    \\#endif
    \\  exit(ret);
    \\}
;

/// `configure.cmake:414-433`; contract expectation: 0.
pub const run_have_mmap_dev_zero =
    \\
    \\#include <stdlib.h>
    \\#include <sys/types.h>
    \\#include <sys/mman.h>
    \\#include <fcntl.h>
    \\
    \\int main(void) {
    \\  int devzero;
    \\  void *retval;
    \\
    \\  devzero = open("/dev/zero", O_RDWR);
    \\  if (-1 == devzero) {
    \\    exit(1);
    \\  }
    \\  retval = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, devzero, 0);
    \\  if (retval == (void *)-1) {
    \\    exit(1);
    \\  }
    \\  exit(0);
    \\}
;

/// `configure.cmake:434-474`; contract expectation: 0.
pub const run_mmap_ignores_hint =
    \\
    \\#include <fcntl.h>
    \\#include <stdlib.h>
    \\#include <sys/types.h>
    \\#include <sys/mman.h>
    \\#include <unistd.h>
    \\
    \\#ifndef MAP_ANON
    \\#define MAP_ANON MAP_ANONYMOUS
    \\#endif
    \\
    \\int main(void) {
    \\  void *hint, *ptr;
    \\  int pagesize;
    \\  int fd;
    \\
    \\  pagesize = getpagesize();
    \\  fd = open("/etc/passwd", O_RDONLY);
    \\  if (fd == -1) {
    \\    exit(0);
    \\  }
    \\  ptr = mmap(NULL, pagesize, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    \\  if (ptr == MAP_FAILED) {
    \\    exit(0);
    \\  }
    \\  hint = mmap(NULL, pagesize, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    \\  if (hint == MAP_FAILED) {
    \\    exit(0);
    \\  }
    \\  if (munmap(ptr, pagesize) != 0) {
    \\    exit(0);
    \\  }
    \\  if (munmap(hint, pagesize) != 0) {
    \\    exit(0);
    \\  }
    \\  ptr = mmap(hint, pagesize, PROT_NONE, MAP_FIXED | MAP_PRIVATE, fd, 0);
    \\  if (ptr == MAP_FAILED || ptr != hint) {
    \\    exit(0);
    \\  }
    \\  exit(1);
    \\}
;

/// `configure.cmake:475-514`; contract expectation: 0.
pub const run_mmap_anon_ignores_protection =
    \\
    \\#include <sys/types.h>
    \\#include <sys/mman.h>
    \\#include <signal.h>
    \\#include <stdlib.h>
    \\#include <unistd.h>
    \\
    \\#ifndef MAP_ANON
    \\#define MAP_ANON MAP_ANONYMOUS
    \\#endif
    \\
    \\void *handle_signal(int signal) {
    \\  /* If we reach this, we've crashed due to mmap honoring
    \\  PROT_NONE. */
    \\  _exit(1);
    \\}
    \\
    \\int main(void) {
    \\  int *ptr;
    \\  struct sigaction action;
    \\
    \\  ptr = (int *) mmap(NULL, getpagesize(), PROT_NONE,
    \\                     MAP_ANON | MAP_PRIVATE, -1, 0);
    \\  if (ptr == (int *) MAP_FAILED) {
    \\    exit(0);
    \\  }
    \\  action.sa_handler = &handle_signal;
    \\  action.sa_flags = 0;
    \\  sigemptyset(&action.sa_mask);
    \\  if (sigaction(SIGBUS, &action, NULL) != 0) {
    \\    exit(0);
    \\  }
    \\  if (sigaction(SIGSEGV, &action, NULL) != 0) {
    \\    exit(0);
    \\  }
    \\  /* This will drop us into the signal handler if PROT_NONE
    \\     is honored. */
    \\  *ptr = 123;
    \\  exit(0);
    \\}
;

/// `configure.cmake:515-542`; contract expectation: 0.
pub const run_mmap_doesnot_allow_remap =
    \\
    \\#include <stdlib.h>
    \\#include <unistd.h>
    \\#include <sys/types.h>
    \\#include <sys/mman.h>
    \\
    \\#ifndef MAP_ANON
    \\#define MAP_ANON MAP_ANONYMOUS
    \\#endif
    \\
    \\int main()
    \\{
    \\  int iRet = 1;
    \\  void * pAddr = MAP_FAILED;
    \\  int MemSize = 1024;
    \\
    \\  MemSize = getpagesize();
    \\  pAddr = mmap(0x0, MemSize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    \\  if (pAddr == MAP_FAILED)
    \\    exit(0);
    \\
    \\  pAddr = mmap(pAddr, MemSize, PROT_WRITE | PROT_READ, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    \\  if (pAddr == MAP_FAILED)
    \\    iRet = 0;
    \\
    \\  munmap(pAddr, MemSize); // don't care of this
    \\  exit (iRet);
    \\}
;

/// `configure.cmake:543-596`; contract expectation: 0.
pub const run_one_shared_mapping_per_fileregion_per_process =
    \\
    \\#include <stdio.h>
    \\#include <unistd.h>
    \\#include <stdlib.h>
    \\#include <string.h>
    \\#include <sys/types.h>
    \\#include <sys/mman.h>
    \\
    \\#define MEM_SIZE 1024
    \\
    \\int main(void)
    \\{
    \\  char * fname;
    \\  int fd;
    \\  int ret;
    \\  void * pAddr0, * pAddr1;
    \\
    \\  fname = (char *)malloc(MEM_SIZE);
    \\  if (!fname)
    \\    exit(1);
    \\  strcpy(fname, "/tmp/name/multiplemaptestXXXXXX");
    \\
    \\  fd = mkstemp(fname);
    \\  if (fd < 0)
    \\    exit(1);
    \\
    \\  ret = write (fd, (void *)fname, MEM_SIZE);
    \\  if (ret < 0)
    \\    exit(1);
    \\
    \\  pAddr0 = mmap(0, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    \\  pAddr1 = mmap(0, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    \\
    \\  /* In theory we should look for (pAddr1 == MAP_FAILED) && (pAddr1 != MAP_FAILED)
    \\   but in case the first test also failed, i.e. we failed to run the test,
    \\   let's assume that the system might not allow multiple shared mapping of the
    \\   same file region in the same process. The code enabled in this case is
    \\   only a fall-back code path. In case the double mmap actually works, virtually
    \\   nothing will change and the normal code path will be executed */
    \\  if (pAddr1 == MAP_FAILED)
    \\    ret = 1;
    \\  else
    \\    ret = 0;
    \\
    \\  if (pAddr0)
    \\    munmap (pAddr0, MEM_SIZE);
    \\  if (pAddr1)
    \\    munmap (pAddr1, MEM_SIZE);
    \\  close(fd);
    \\  unlink(fname);
    \\  free(fname);
    \\
    \\  exit(ret != 1);
    \\}
;

/// `configure.cmake:597-618` (links pthread); contract expectation: 0.
pub const run_pthread_create_modifies_errno =
    \\
    \\#include <errno.h>
    \\#include <pthread.h>
    \\#include <stdlib.h>
    \\
    \\void *start_routine(void *param) { return NULL; }
    \\
    \\int main() {
    \\  int result;
    \\  pthread_t tid;
    \\
    \\  errno = 0;
    \\  result = pthread_create(&tid, NULL, start_routine, NULL);
    \\  if (result != 0) {
    \\    exit(1);
    \\  }
    \\  if (errno != 0) {
    \\    exit(0);
    \\  }
    \\  exit(1);
    \\}
;

/// `configure.cmake:620-641` (links pthread); contract expectation: 0.
pub const run_sem_init_modifies_errno =
    \\
    \\#include <errno.h>
    \\#include <semaphore.h>
    \\#include <stdlib.h>
    \\
    \\int main() {
    \\  int result;
    \\  sem_t sema;
    \\
    \\  errno = 50;
    \\  result = sem_init(&sema, 0, 0);
    \\  if (result != 0)
    \\  {
    \\    exit(1);
    \\  }
    \\  if (errno != 50)
    \\  {
    \\    exit(0);
    \\  }
    \\  exit(1);
    \\}
;

/// `configure.cmake:643-665`; contract expectation: 0.
pub const run_have_procfs_ctl =
    \\
    \\#include <fcntl.h>
    \\#include <stdlib.h>
    \\#include <stdio.h>
    \\#include <unistd.h>
    \\
    \\int main(void) {
    \\  int fd;
    \\#ifdef PATH_MAX
    \\  char path[PATH_MAX];
    \\#elif defined(MAXPATHLEN)
    \\  char path[MAXPATHLEN];
    \\#else
    \\  char path[1024];
    \\#endif
    \\
    \\  sprintf(path, "/proc/%u/$1", getpid());
    \\  fd = open(path, $2);
    \\  if (fd == -1) {
    \\    exit(1);
    \\  }
    \\  exit(0);
    \\}
;

/// `configure.cmake:666-676` (links m); contract expectation: 1.
pub const run_have_compatible_acos =
    \\
    \\#include <math.h>
    \\#include <stdlib.h>
    \\
    \\int main(void) {
    \\  if (!isnan(acos(10))) {
    \\    exit(1);
    \\  }
    \\  exit(0);
    \\}
;

/// `configure.cmake:678-688` (links m); contract expectation: 1.
pub const run_have_compatible_asin =
    \\
    \\#include <math.h>
    \\#include <stdlib.h>
    \\
    \\int main(void) {
    \\  if (!isnan(asin(10))) {
    \\    exit(1);
    \\  }
    \\  exit(0);
    \\}
;

/// `configure.cmake:690-720` (links m); contract expectation: 1.
pub const run_have_compatible_atan2 =
    \\
    \\#include <math.h>
    \\#include <stdlib.h>
    \\
    \\int main(void) {
    \\  double pi = 3.14159265358979323846;
    \\  double result;
    \\
    \\  result = atan2(0.0, -0.0);
    \\  if (fabs(pi - result) > 0.0000001) {
    \\    exit(1);
    \\  }
    \\
    \\  result = atan2(-0.0, -0.0);
    \\  if (fabs(-pi - result) > 0.0000001) {
    \\    exit(1);
    \\  }
    \\
    \\  result = atan2 (-0.0, 0.0);
    \\  if (result != 0.0 || copysign (1.0, result) > 0) {
    \\    exit(1);
    \\  }
    \\
    \\  result = atan2 (0.0, 0.0);
    \\  if (result != 0.0 || copysign (1.0, result) < 0) {
    \\    exit(1);
    \\  }
    \\
    \\  exit (0);
    \\}
;

/// `configure.cmake:722-740` (links m); contract expectation: 0.
pub const run_have_compatible_exp =
    \\
    \\#include <math.h>
    \\#include <stdlib.h>
    \\
    \\int main(void) {
    \\  double d = exp(1.0), e = M_E;
    \\
    \\  /* Used memcmp rather than == to test that the doubles are equal to
    \\   prevent gcc's optimizer from using its 80 bit internal long
    \\   doubles. If you use ==, then on BSD you get a false negative since
    \\   exp(1.0) == M_E to 64 bits, but not 80.
    \\  */
    \\
    \\  if (memcmp (&d, &e, sizeof (double)) == 0) {
    \\    exit(0);
    \\  }
    \\  exit(1);
    \\}
;

/// `configure.cmake:742-752` (links m); contract expectation: 1.
pub const run_have_compatible_log =
    \\
    \\#include <math.h>
    \\#include <stdlib.h>
    \\
    \\int main(void) {
    \\  if (!isnan(log(-10000))) {
    \\    exit(1);
    \\  }
    \\  exit(0);
    \\}
;

/// `configure.cmake:754-764` (links m); contract expectation: 1.
pub const run_have_compatible_log10 =
    \\
    \\#include <math.h>
    \\#include <stdlib.h>
    \\
    \\int main(void) {
    \\  if (!isnan(log10(-10000))) {
    \\    exit(1);
    \\  }
    \\  exit(0);
    \\}
;

/// `configure.cmake:766-791`; contract expectation: 0.
pub const run_ungetc_not_return_eof =
    \\
    \\#include <stdio.h>
    \\#include <stdlib.h>
    \\#include <unistd.h>
    \\
    \\int main(void)
    \\{
    \\  char* szFileName;
    \\  FILE* pFile = NULL;
    \\  int ret = 1;
    \\
    \\  szFileName = tempnam(".", "tmp");
    \\
    \\  /* open the file write-only */
    \\  pFile = fopen(szFileName, "a");
    \\  if (pFile == NULL)
    \\  {
    \\    exit(0);
    \\  }
    \\  if (ungetc('A', pFile) != EOF)
    \\  {
    \\    ret = 0;
    \\  }
    \\  unlink(szFileName);
    \\  exit(ret);
    \\}
;

/// `configure.cmake:792-804` (links pthread); contract expectation: 0.
pub const run_has_posix_semaphores =
    \\
    \\#include <stdlib.h>
    \\#include <errno.h>
    \\#include <semaphore.h>
    \\
    \\int main() {
    \\  sem_t sema;
    \\  if (sem_init(&sema, 0, 0) == -1){
    \\    exit(1);
    \\  }
    \\  exit(0);
    \\}
;

/// `configure.cmake:806-833`; contract expectation: 0.
pub const run_getpwuid_r_sets_errno =
    \\
    \\#include <sys/types.h>
    \\#include <pwd.h>
    \\#include <errno.h>
    \\#include <unistd.h>
    \\#include <stdlib.h>
    \\
    \\int main(void)
    \\{
    \\  struct passwd sPasswd;
    \\  struct passwd *pPasswd;
    \\  char buf[1];
    \\  int bufLen = sizeof(buf)/sizeof(buf[0]);
    \\  int euid = geteuid();
    \\  int ret = 0;
    \\
    \\  errno = 0; // clear errno
    \\  ret = getpwuid_r(euid, &sPasswd, buf, bufLen, &pPasswd);
    \\  if (0 != ret)
    \\  {
    \\    if (ERANGE == errno)
    \\    {
    \\      return 0;
    \\    }
    \\  }
    \\
    \\  return 1; // assume errno is NOT set for all other cases
    \\}
;

/// `configure.cmake:834-868`; contract expectation: 0.
pub const run_file_ops_check_ferror_of_previous_call =
    \\
    \\#include <stdio.h>
    \\#include <stdlib.h>
    \\
    \\int main()
    \\{
    \\  FILE *fp = NULL;
    \\  char *fileName = "/dev/zero";
    \\  char buf[10];
    \\
    \\  /*
    \\   * Open the file in append mode and try to read some text.
    \\   * And, make sure ferror() is set.
    \\   */
    \\  fp = fopen (fileName, "a");
    \\  if ( (NULL == fp) ||
    \\       (fread (buf, sizeof(buf), 1, fp) > 0) ||
    \\       (!ferror(fp))
    \\     )
    \\  {
    \\    return 0;
    \\  }
    \\
    \\  /*
    \\   * Now that ferror() is set, try to close the file.
    \\   * If we get an error, we can conclude that this
    \\   * fgets() depended on the previous ferror().
    \\   */
    \\  if ( fclose(fp) != 0 )
    \\  {
    \\    return 0;
    \\  }
    \\
    \\  return 1;
    \\}
;

/// `Build/CMakeFeatureDetect.cmake:17-27`; contract expectation: pass (1).
pub const run_clang_has_disable_tail_calls =
    \\
    \\#include <stdlib.h>
    \\#include <stdio.h>
    \\
    \\__attribute__((disable_tail_calls))
    \\int inc(int a) { return a + 1; }
    \\
    \\int main(int argc, char **argv) {
    \\  printf("%d", inc(argc + 1));
    \\  exit(0);
    \\}
;
