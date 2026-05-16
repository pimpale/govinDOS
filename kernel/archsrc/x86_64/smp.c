#include "smp.h"

#include <stdint.h>

#include "panic.h"
#include "stdlib/stdio.h"

// TODO: INIT IPI + SIPI through the LAPIC. Requires a sub-1MiB real-mode
// trampoline that walks the AP through real -> protected -> long mode and
// hands off to `entry` with SP set to `stack`.
void cpu_start(uint64_t hw_id, void (*entry)(void), void *stack) {
  (void)hw_id;
  (void)entry;
  (void)stack;
  printf("cpu_start: SMP bringup not yet implemented on x86_64\n");
  panic();
}
