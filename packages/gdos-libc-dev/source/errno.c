#include "errno.h"

static _Thread_local int g_errno;

int *__errno_location(void) { return &g_errno; }
