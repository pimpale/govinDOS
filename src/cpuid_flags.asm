[GLOBAL CPUID_EDX_FPU]
[GLOBAL CPUID_EDX_VME]
[GLOBAL CPUID_EDX_DE]
[GLOBAL CPUID_EDX_PSE]
[GLOBAL CPUID_EDX_TSC]
[GLOBAL CPUID_EDX_MSR]
[GLOBAL CPUID_EDX_PAE]
[GLOBAL CPUID_EDX_MCE]
[GLOBAL CPUID_EDX_CX8]
[GLOBAL CPUID_EDX_APIC]
[GLOBAL CPUID_EDX_SEP]
[GLOBAL CPUID_EDX_MTRR]
[GLOBAL CPUID_EDX_PGE]
[GLOBAL CPUID_EDX_MCA]
[GLOBAL CPUID_EDX_CMOV]
[GLOBAL CPUID_EDX_PAT]
[GLOBAL CPUID_EDX_PSE_36]
[GLOBAL CPUID_EDX_PSN]
[GLOBAL CPUID_EDX_CLFSH]
[GLOBAL CPUID_EDX_DS]
[GLOBAL CPUID_EDX_ACPI]
[GLOBAL CPUID_EDX_MMX]
[GLOBAL CPUID_EDX_FXSR]
[GLOBAL CPUID_EDX_SSE]
[GLOBAL CPUID_EDX_SSE2]
[GLOBAL CPUID_EDX_SS]
[GLOBAL CPUID_EDX_HTT]
[GLOBAL CPUID_EDX_TM]
[GLOBAL CPUID_EDX_IA64]
[GLOBAL CPUID_EDX_PBE]
[GLOBAL CPUID_ECX_SSE3]
[GLOBAL CPUID_ECX_PCLMULQDQ]
[GLOBAL CPUID_ECX_DTES64]
[GLOBAL CPUID_ECX_MONITOR]
[GLOBAL CPUID_ECX_DS_CPL]
[GLOBAL CPUID_ECX_VMX]
[GLOBAL CPUID_ECX_SMX]
[GLOBAL CPUID_ECX_EST]
[GLOBAL CPUID_ECX_TM2]
[GLOBAL CPUID_ECX_SSSE3]
[GLOBAL CPUID_ECX_CNXT_ID]
[GLOBAL CPUID_ECX_SDBG]
[GLOBAL CPUID_ECX_FMA]
[GLOBAL CPUID_ECX_CX16]
[GLOBAL CPUID_ECX_XTPR]
[GLOBAL CPUID_ECX_PDCM]
[GLOBAL CPUID_ECX_PCID]
[GLOBAL CPUID_ECX_DCA]
[GLOBAL CPUID_ECX_SSE4_1]
[GLOBAL CPUID_ECX_SSE4_2]
[GLOBAL CPUID_ECX_X2APIC]
[GLOBAL CPUID_ECX_MOVBE]
[GLOBAL CPUID_ECX_POPCNT]
[GLOBAL CPUID_ECX_TSC_DEADLINE]
[GLOBAL CPUID_ECX_AES]
[GLOBAL CPUID_ECX_XSAVE]
[GLOBAL CPUID_ECX_OSXSAVE]
[GLOBAL CPUID_ECX_AVX]
[GLOBAL CPUID_ECX_F16C]
[GLOBAL CPUID_ECX_RDRND]
[GLOBAL CPUID_ECX_HYPERVISOR]



CPUID_EDX_FPU          equ 1 << 0   ; Onboard x87 FPU
CPUID_EDX_VME          equ 1 << 1   ; Virtual 8086 mode extensions
CPUID_EDX_DE           equ 1 << 2   ; Debugging extensions (CR4 bit 3)
CPUID_EDX_PSE          equ 1 << 3   ; Page Size Extension
CPUID_EDX_TSC          equ 1 << 4   ; Time stamp counter
CPUID_EDX_MSR          equ 1 << 5   ; Model specific registers
CPUID_EDX_PAE          equ 1 << 6   ; Physical Address Extension
CPUID_EDX_MCE          equ 1 << 7   ; Machine Check Exception
CPUID_EDX_CX8          equ 1 << 8   ; CMPXCGH8 compare and swap instruction
CPUID_EDX_APIC         equ 1 << 9   ; Onboard advanced programmable interrupt controller
CPUID_EDX_SEP          equ 1 << 11  ; SYSENTER and SYSEXIT instructions
CPUID_EDX_MTRR         equ 1 << 12  ; Memory Type Range Registers
CPUID_EDX_PGE          equ 1 << 13  ; Page Global Enable bit in CR4
CPUID_EDX_MCA          equ 1 << 14  ; Machine check architecture
CPUID_EDX_CMOV         equ 1 << 15  ; Conditional move and FCMOV instructions
CPUID_EDX_PAT          equ 1 << 17  ; Page Attribute Table
CPUID_EDX_PSE_36       equ 1 << 18  ; 36-bit page size extension
CPUID_EDX_PSN          equ 1 << 19  ; Processor Serial Number
CPUID_EDX_CLFSH        equ 1 << 20  ; CLFLUST instruction (SSE2)
CPUID_EDX_DS           equ 1 << 21  ; Debug stor: saev trace of executed jumps
CPUID_EDX_ACPI         equ 1 << 22  ; Onboard thermal control MSRs for ACPI
CPUID_EDX_MMX          equ 1 << 23  ; MMX instrictions
CPUID_EDX_FXSR         equ 1 << 24  ; FXSAVE FXRESTOR instructions, CR4 bit 9
CPUID_EDX_SSE          equ 1 << 25  ; SSE instructions
CPUID_EDX_SSE2         equ 1 << 26  ; SSE2 instructions
CPUID_EDX_SS           equ 1 << 27  ; CPU cache implements self-snoop
CPUID_EDX_HTT          equ 1 << 28  ; Hyper-threading
CPUID_EDX_TM           equ 1 << 29  ; Thermal monitor automatically limits temperature
CPUID_EDX_IA64         equ 1 << 30  ; IA64 processor emulating x86
CPUID_EDX_PBE          equ 1 << 31  ; Pending Break Enable (PBE# pin) wakeup capability
CPUID_ECX_SSE3         equ 1 << 0   ; SSE3 instructions
CPUID_ECX_PCLMULQDQ    equ 1 << 1   ; PCMULQDQ
CPUID_ECX_DTES64       equ 1 << 2   ; 64-bit debug store
CPUID_ECX_MONITOR      equ 1 << 3   ; MONITOR and MWAIT instructions (SSE3)
CPUID_ECX_DS_CPL       equ 1 << 4   ; CPL qualified debug store
CPUID_ECX_VMX          equ 1 << 5   ; Virtual Machine eXtensions
CPUID_ECX_SMX          equ 1 << 6   ; Safer Mode Extensions
CPUID_ECX_EST          equ 1 << 7   ; Enhanced SpeedStep
CPUID_ECX_TM2          equ 1 << 8   ; Thermal Monitor 2
CPUID_ECX_SSSE3        equ 1 << 9   ; Suplemental SSE3  Extensions
CPUID_ECX_CNXT_ID      equ 1 << 10  ; L1 Context ID
CPUID_ECX_SDBG         equ 1 << 11  ; Silicon Debug Interface
CPUID_ECX_FMA          equ 1 << 12  ; Fused multiply-add (FMA3)
CPUID_ECX_CX16         equ 1 << 13  ; CMPXCHG16B instruction
CPUID_ECX_XTPR         equ 1 << 14  ; Can disable sending task priority messages
CPUID_ECX_PDCM         equ 1 << 15  ; Perfmon and debug capability
CPUID_ECX_PCID         equ 1 << 17  ; Process context identifiers (CR4 bit 17)
CPUID_ECX_DCA          equ 1 << 18  ; Direct cache access for DMA writes
CPUID_ECX_SSE4_1       equ 1 << 19  ; SSE4.1 instructions
CPUID_ECX_SSE4_2       equ 1 << 20  ; SSE4.2 instructions
CPUID_ECX_X2APIC       equ 1 << 21  ; x2APIC
CPUID_ECX_MOVBE        equ 1 << 22  ; MOVBE instruction (big-endian)
CPUID_ECX_POPCNT       equ 1 << 23  ; POPCNT instruction
CPUID_ECX_TSC_DEADLINE equ 1 << 24  ; APIC implements operation using TSC deadline
CPUID_ECX_AES          equ 1 << 25  ; AES instructin set
CPUID_ECX_XSAVE        equ 1 << 26  ; XSAVE, XRESTOR, XSETBV, XGETBV
CPUID_ECX_OSXSAVE      equ 1 << 27  ; XSAVE enabled by OS
CPUID_ECX_AVX          equ 1 << 28  ; Advanced Vector Extensions
CPUID_ECX_F16C         equ 1 << 29  ; F16C half precision floating point
CPUID_ECX_RDRND        equ 1 << 30  ; RDRAND on chip random number generator feature
CPUID_ECX_HYPERVISOR   equ 1 << 31  ; Hypervisor present
