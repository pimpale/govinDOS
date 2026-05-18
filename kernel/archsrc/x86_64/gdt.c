#include "gdt.h"

#include <stdint.h>

#include "debug.h"
#include "paging.h"
#include "stdlib/stdlib.h"



// ---------------------------------------------------------------------------
// Hardware structures
// ---------------------------------------------------------------------------

// Long-mode TSS (104 bytes). The "task" parts are vestigial in long mode;
// what the CPU still reads from this struct is RSP0 (on ring 3 -> ring 0
// entry) and IST1..IST7 (when an IDT gate selects an IST slot).
struct [[gnu::packed]] tss64 {
  uint32_t reserved0;
  uint64_t rsp0;
  uint64_t rsp1;
  uint64_t rsp2;
  uint64_t reserved1;
  uint64_t ist[7];           // ist[0] is IST1, ist[6] is IST7
  uint64_t reserved2;
  uint16_t reserved3;
  uint16_t iopb_offset;
};

// Eight-byte segment descriptor (code/data). In long mode base and limit
// are ignored for CS/DS/ES/SS, but the descriptor still has to be
// well-formed and the L bit must be set on the kernel CS.
struct [[gnu::packed]] gdt_seg {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t  base_mid;
  uint8_t  access;
  uint8_t  flags_limit_hi;
  uint8_t  base_hi;
};

// Sixteen-byte system descriptor (used for the TSS in long mode).
struct [[gnu::packed]] gdt_sys {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t  base_mid;
  uint8_t  access;
  uint8_t  flags_limit_hi;
  uint8_t  base_hi;
  uint32_t base_upper;
  uint32_t reserved;
};

// The per-CPU GDT layout. Must match the selector constants in gdt.h.
struct [[gnu::packed]] gdt_layout {
  struct gdt_seg null;        // 0x00
  struct gdt_seg kernel_cs;   // 0x08
  struct gdt_seg kernel_ds;   // 0x10
  struct gdt_seg user_cs32;   // 0x18
  struct gdt_seg user_ds;     // 0x20
  struct gdt_seg user_cs64;   // 0x28
  struct gdt_sys tss;         // 0x30 (16 bytes, two slots)
};

struct [[gnu::packed]] gdtr {
  uint16_t limit;
  uint64_t base;
};

// ---------------------------------------------------------------------------
// Descriptor builders
// ---------------------------------------------------------------------------

// Access byte: P=1, DPL, S, type. For long-mode code/data segments base and
// limit are ignored but we still set a sane limit so 32-bit compatibility
// segments would behave.
//
// Flags nibble (high 4 bits of flags_limit_hi): G, D/B, L, AVL.

static struct gdt_seg make_seg(uint8_t access, uint8_t flags_nibble) {
  return (struct gdt_seg){
      .limit_low      = 0xFFFF,
      .base_low       = 0,
      .base_mid       = 0,
      .access         = access,
      .flags_limit_hi = (uint8_t)((flags_nibble << 4) | 0x0F),
      .base_hi        = 0,
  };
}

static struct gdt_sys make_tss_desc(uint64_t base, uint32_t limit) {
  return (struct gdt_sys){
      .limit_low      = (uint16_t)(limit & 0xFFFF),
      .base_low       = (uint16_t)(base & 0xFFFF),
      .base_mid       = (uint8_t)((base >> 16) & 0xFF),
      // P=1 (0x80), DPL=0, S=0, type=0x9 (available 64-bit TSS) -> 0x89
      .access         = 0x89,
      // G=0, limit[19:16]
      .flags_limit_hi = (uint8_t)((limit >> 16) & 0x0F),
      .base_hi        = (uint8_t)((base >> 24) & 0xFF),
      .base_upper     = (uint32_t)(base >> 32),
      .reserved       = 0,
  };
}

// ---------------------------------------------------------------------------
// CPU instructions
// ---------------------------------------------------------------------------

// Defined in gdt.asm. Loads the GDT, far-returns into the new kernel CS,
// and reloads the data segment registers. The data selector is assumed to
// be code_sel + 8 (matches our layout below).
extern void asm_load_gdt(const struct gdtr *g, uint16_t code_sel);

static void ltr(uint16_t selector) {
  __asm__ volatile("ltr %0" : : "r"(selector));
}

// ---------------------------------------------------------------------------
// Per-CPU bring-up
// ---------------------------------------------------------------------------

void *cpu_install_gdt_tss(
    void *ist_double_fault_stack_top,
    void *ist_nmi_stack_top,
    void *ist_page_fault_stack_top,
    void *ist_machine_check_stack_top) {
  // One GDT and one TSS per CPU. Leaking the malloc'd pointer is intentional
  // for now — these live for the lifetime of the CPU.
  struct gdt_layout *gdt = malloc(sizeof(*gdt));
  struct tss64      *tss = malloc(sizeof(*tss));
  asserts(gdt != nullptr && tss != nullptr,
          "gdt: failed to allocate GDT/TSS");

  // Kernel/user segment descriptors.
  //   access: P=1 (0x80), S=1 (0x10), DPL (<<5), type (low 4 bits)
  //   code type = 0xA (exec, readable), data type = 0x2 (writable)
  //   flags nibble: G=1 (0x8), L=1 for 64-bit code (0x2), D=1 for 32-bit code (0x4)
  *gdt = (struct gdt_layout){
      .null      = make_seg(0,    0),
      .kernel_cs = make_seg(0x9A, 0xA),       // P|S|DPL0|exec|read, G|L
      .kernel_ds = make_seg(0x92, 0xC),       // P|S|DPL0|write,     G|D
      .user_cs32 = make_seg(0xFA, 0xC),       // P|S|DPL3|exec|read, G|D  (32-bit)
      .user_ds   = make_seg(0xF2, 0xC),       // P|S|DPL3|write,     G|D
      .user_cs64 = make_seg(0xFA, 0xA),       // P|S|DPL3|exec|read, G|L  (64-bit)
      .tss       = make_tss_desc((uint64_t)tss, sizeof(*tss) - 1),
  };

  // RSP0 is left at 0 here — it's per-thread and gets written by
  // arch_thread_install() each time a thread is scheduled. The IST entries
  // are loaded automatically when an IDT gate names them. Each pointer is
  // the top of a caller-owned, guard-paged kernel stack.
  *tss = (struct tss64){
      .rsp0                          = 0,
      .ist[IST_DOUBLE_FAULT  - 1]    = (uint64_t)ist_double_fault_stack_top,
      .ist[IST_NMI           - 1]    = (uint64_t)ist_nmi_stack_top,
      .ist[IST_PAGE_FAULT    - 1]    = (uint64_t)ist_page_fault_stack_top,
      .ist[IST_MACHINE_CHECK - 1]    = (uint64_t)ist_machine_check_stack_top,
      // iopb_offset == sizeof(tss) means "no I/O bitmap, deny all ring-3 I/O".
      .iopb_offset = sizeof(struct tss64),
  };

  struct gdtr g = {
      .limit = (uint16_t)(sizeof(*gdt) - 1),
      .base  = (uint64_t)gdt,
  };

  asm_load_gdt(&g, GDT_SEL_KERNEL_CS);
  ltr(GDT_SEL_TSS);

  return tss;
}

void cpu_tss_set_rsp0(void *tss, void *rsp0) {
  ((struct tss64 *)tss)->rsp0 = (uint64_t)rsp0;
}
