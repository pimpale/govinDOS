; Embeds the AP trampoline flat binary as a data blob in the kernel image.
; The trampoline itself is built separately with `nasm -f bin`; this wrapper
; only exists to give the C code symbol-level access to that blob.

[SECTION .data]
[GLOBAL ap_trampoline_blob]
[GLOBAL ap_trampoline_blob_end]

ap_trampoline_blob:
        incbin "bin/archsrc/x86_64/blobs/ap_trampoline.asm.bin"
ap_trampoline_blob_end:
