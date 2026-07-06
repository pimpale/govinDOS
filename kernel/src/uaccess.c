#include "uaccess.h"

#include "allocator.h"
#include "debug.h"
#include "stdlib/stdlib.h"
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

void *umem_alloc(struct process *p, size_t len, paging_flags_t prot) {
  if (len == 0) {
    return nullptr;
  }
  size_t npages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
  // Buddy blocks of >= a page are page-aligned, so flagging the exact
  // range below can't catch unrelated kernel allocations in the same page
  // (same property the stack guard-page logic relies on).
  uint8_t *base = calloc(npages, PAGE_SIZE);
  if (base == nullptr) {
    return nullptr;
  }
  for (size_t i = 0; i < npages; i++) {
    struct frame_info *fi = frame_for((uint64_t)base + i * PAGE_SIZE);
    asserts(fi != nullptr, "umem_alloc: frame outside tracked range");
    fi->kind = FRAME_USER;
    fi->owner_id = (uint16_t)p->pid; // truncation fine at toy scale
  }
  as_flag(p->as, (uint64_t)base, (uint64_t)base + npages * PAGE_SIZE,
          prot | PAGE_U);
  as_flush(p->as);
  return base;
}

int umem_free(struct process *p, uint64_t base, size_t len) {
  if (base % PAGE_SIZE != 0 || len == 0 || len % PAGE_SIZE != 0) {
    return -1;
  }
  size_t npages = len / PAGE_SIZE;
  // Validate the whole range before mutating anything.
  for (size_t i = 0; i < npages; i++) {
    struct frame_info *fi = frame_for(base + i * PAGE_SIZE);
    if (fi == nullptr || fi->kind != FRAME_USER ||
        fi->owner_id != (uint16_t)p->pid) {
      return -1;
    }
  }
  as_flag(p->as, base, base + len, PAGE_R | PAGE_W);
  as_flush(p->as);
  for (size_t i = 0; i < npages; i++) {
    struct frame_info *fi = frame_for(base + i * PAGE_SIZE);
    fi->kind = FRAME_FREE;
    fi->owner_id = 0;
  }
  free((void *)base);
  return 0;
}
