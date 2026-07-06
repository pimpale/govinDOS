; Embeds the C userspace test program (userspace/hello.c, compiled to
; PE32+ by the kernel build) so the kernel can exercise the PE loader
; before a filesystem exists. Same pattern as ap_trampoline_blob.asm.

[SECTION .data]
[GLOBAL user_pe_blob]
[GLOBAL user_pe_blob_end]

user_pe_blob:
        incbin "bin/userspace/hello.exe"
user_pe_blob_end:
