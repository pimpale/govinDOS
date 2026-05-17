[BITS 64]
default rel

[GLOBAL asm_load_gdt]

[SECTION .text]

; void asm_load_gdt(const struct gdtr *gdtr_ptr, uint16_t code_sel)
;
; Loads the GDT pointed to by gdtr_ptr, then far-returns into the new
; kernel code segment and reloads the data segment registers.
;
; The GDT contents themselves are built in C (see archsrc/x86_64/gdt.c).
; This routine is the small slice that must be assembly because reloading
; CS in 64-bit mode requires lretq, which is awkward to express in C.
;
; Win64 ABI:
;   rcx = pointer to a packed { uint16_t limit; uint64_t base; }
;   dx  = kernel code selector
;
; The data selector is assumed to be (code_sel + 8) — i.e. kernel CS and
; kernel DS are adjacent in the GDT. This matches the layout in gdt.c and
; the layout required by syscall/sysret (STAR MSR), so user CS/DS will fit
; the same convention later.
asm_load_gdt:
    lgdt [rcx]
    movzx rdx, dx              ; zero-extend selector for the push
    push  rdx                  ; CS for lretq
    lea   rax, [rel .reload_CS]
    push  rax                  ; RIP for lretq
    retfq
.reload_CS:
    add   dx, 8                ; data selector = code + 8
    mov   ds, dx
    mov   es, dx
    mov   fs, dx
    mov   gs, dx
    mov   ss, dx
    ret
