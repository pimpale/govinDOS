; Embeds the initfs (initdata.cpio) into init.exe as read-only data.
; Same trick as the kernel's old user_pe_blob.asm, moved to the userspace
; link: the kernel never learns the archive exists — init finds it via
; these two symbols (docs/technical/boot-init-design.md §0).

[SECTION .rdata rdata align=8]
[GLOBAL bootfs_start]
[GLOBAL bootfs_end]

bootfs_start:
        incbin "out/initdata.cpio"
bootfs_end:
