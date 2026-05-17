; AP startup trampoline. Assembled as a flat binary and copied to physical
; AP_TRAMPOLINE_BASE (0x8000) before SIPI. SIPI vector 0x08 lands the AP at
; CS=0x0800, IP=0, i.e. physical 0x8000.
;
; Layout of the first 0x28 bytes is shared with the BSP:
;
;   +0x00  jmp short ap_real_mode (with padding so data starts at +0x08)
;   +0x08  cr3       (filled by BSP — CR3 to load on the AP)
;   +0x10  entry     (filled by BSP — long-mode C entry point)
;   +0x18  stack     (filled by BSP — top of per-AP stack)
;   +0x20  alive     (set to 1 by the AP once it reaches C code)
;   +0x28  ap_real_mode: start of executable code
;
; The AP shares the BSP's CR3, so the kernel image and the trampoline page
; itself must be identity-mapped (UEFI does this for the low 4 GiB).

[BITS 16]
[ORG 0x8000]
default abs

%define TRAMPOLINE_BASE 0x8000

ap_trampoline:
        jmp     short ap_real_mode
        times 8 - ($ - ap_trampoline) db 0

ap_cr3:         dq 0
ap_entry:       dq 0
ap_stack:       dq 0
ap_alive:       dq 0

; ---------------------------------------------------------------------------
; Real mode: cs=0x0800, ip=0. We point ds at segment 0 and use absolute
; offsets so [label] addresses physical 0x8000+offset directly.
; ---------------------------------------------------------------------------
ap_real_mode:
        cli
        cld

        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     esp, TRAMPOLINE_BASE + 0xFF0  ; small temp stack inside the page

        lgdt    [gdt_desc]

        mov     eax, cr0
        or      eax, 1                         ; CR0.PE
        mov     cr0, eax

        jmp     dword 0x08:ap_protect32

; ---------------------------------------------------------------------------
; 32-bit protected mode. Turn on PAE, set LME, load CR3, enable paging, then
; far-jump into a 64-bit code segment to enter long mode.
; ---------------------------------------------------------------------------
[BITS 32]
ap_protect32:
        mov     ax, 0x10                       ; 32-bit data
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax

        mov     eax, cr4
        or      eax, 1 << 5                    ; CR4.PAE
        mov     cr4, eax

        mov     eax, [ap_cr3]                  ; PML4 phys (low 32 bits)
        mov     cr3, eax

        mov     ecx, 0xC0000080                ; EFER
        rdmsr
        or      eax, 1 << 8                    ; EFER.LME
        wrmsr

        mov     eax, cr0
        or      eax, 1 << 31                   ; CR0.PG
        mov     cr0, eax

        jmp     0x18:ap_long64

; ---------------------------------------------------------------------------
; Long mode. Switch to per-AP stack and hand control to the C entry. We do
; not return — the entry function halts.
; ---------------------------------------------------------------------------
[BITS 64]
ap_long64:
        mov     ax, 0x20                       ; 64-bit data
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax

        mov     rsp, [ap_stack]
        and     rsp, -16                       ; 16-byte align before call
        sub     rsp, 32                        ; MS x64 shadow space

        ; ap_alive is set by the C entry point once it has finished any
        ; bring-up output it wants to do, so the BSP serializes AP starts.
        mov     rax, [ap_entry]
        call    rax

.hang:
        cli
        hlt
        jmp     .hang

; ---------------------------------------------------------------------------
; GDT. Flat 0..4GiB segments for the trampoline's own mode transitions.
;   0x00 null
;   0x08 32-bit code
;   0x10 32-bit data
;   0x18 64-bit code (L=1)
;   0x20 64-bit data
; ---------------------------------------------------------------------------
align 16
gdt:
        dq 0
        ; 32-bit code: base=0, limit=0xFFFFF, 4K, exec/read, P=1, DPL=0, S=1, D=1
        dw 0xFFFF, 0x0000
        db 0x00, 10011010b, 11001111b, 0x00
        ; 32-bit data: base=0, limit=0xFFFFF, 4K, R/W
        dw 0xFFFF, 0x0000
        db 0x00, 10010010b, 11001111b, 0x00
        ; 64-bit code: L=1, D=0
        dw 0xFFFF, 0x0000
        db 0x00, 10011010b, 10101111b, 0x00
        ; 64-bit data
        dw 0xFFFF, 0x0000
        db 0x00, 10010010b, 11001111b, 0x00
gdt_end:

align 4
gdt_desc:
        dw      gdt_end - gdt - 1
        dd      gdt                            ; 32-bit base, fine for lgdt in real mode
