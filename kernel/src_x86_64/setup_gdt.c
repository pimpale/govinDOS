#include "setup_gdt.h"

#include <stdint.h>

extern /* defined in assembly */
void load_gdt();

void setup_gdt() {
    load_gdt();
}
