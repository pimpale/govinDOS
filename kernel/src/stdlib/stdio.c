#include "stdio.h"

#include <stdarg.h>
#include <stdint.h>

#include "cpu_state.h"
#include "serial.h"
#include "spinlock.h"

void putchar_(char c) { serial_putchar((uint8_t)c); }

// Console output lock. spinlock_lock masks IRQs on this CPU so an
// interrupt handler trying to printf can't deadlock against the
// interrupted code.
static struct spinlock g_print_lock = SPINLOCK_INITIALIZER;

int vprintf_locked(const char *fmt, va_list args) {
  if (cpu_state_table_initialized()) {
    spinlock_lock(&g_print_lock);
    int n = vprintf_(fmt, args);
    spinlock_unlock(&g_print_lock);
    return n;
  } else {
    return vprintf_(fmt, args);
  }
}

int printf_locked(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vprintf_locked(fmt, args);
  va_end(args);
  return n;
}
