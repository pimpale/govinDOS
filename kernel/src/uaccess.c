#include "uaccess.h"

#include "thread.h"

bool user_range_ok(const struct process *p, uint64_t addr, uint64_t len,
                   bool need_write) {
  if (len == 0) {
    return true;
  }
  uint64_t end;
  if (__builtin_add_overflow(addr, len, &end)) {
    return false;
  }
  uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);
  for (; page < end; page += PAGE_SIZE) {
    paging_flags_t flags = 0;
    bool present = false;
    as_getinfo(p->as, page, &flags, &present);
    if (!present || !(flags & PAGE_U) || !(flags & PAGE_R)) {
      return false;
    }
    if (need_write && !(flags & PAGE_W)) {
      return false;
    }
  }
  return true;
}
