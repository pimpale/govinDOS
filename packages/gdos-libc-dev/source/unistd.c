#include "unistd.h"

#include <errno.h>
#include <gdos/sys.h>

pid_t getpid(void) { return sys_getpid(); }

ssize_t write(int fd, const void *buf, size_t count) {
  if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
    errno = EBADF;
    return -1;
  }
  uint64_t rc = sys_debug_write(buf, count);
  if (sys_iserr(rc)) {
    errno = EFAULT;
    return -1;
  }
  return (ssize_t)count;
}

[[noreturn]] void _exit(int status) { sys_proc_exit((uint32_t)status); }

int sched_yield(void) {
  sys_yield();
  return 0;
}
