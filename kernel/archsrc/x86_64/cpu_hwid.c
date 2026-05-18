#include "cpu_hwid.h"
#include "lapic.h"

uint64_t cpu_hwid() {
    return x86_lapic_id();
}