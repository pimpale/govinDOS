[GLOBAL CR0_PE]
[GLOBAL CR0_MP]
[GLOBAL CR0_EM]
[GLOBAL CR0_TS]
[GLOBAL CR0_ET]
[GLOBAL CR0_NE]
[GLOBAL CR0_WP]
[GLOBAL CR0_AM]
[GLOBAL CR0_NW]
[GLOBAL CR0_CD]
[GLOBAL CR0_PG]
[GLOBAL CR4_VME]
[GLOBAL CR4_PVI]
[GLOBAL CR4_TSD]
[GLOBAL CR4_DE]
[GLOBAL CR4_PSE]
[GLOBAL CR4_PAE]
[GLOBAL CR4_MCE]
[GLOBAL CR4_PGE]
[GLOBAL CR4_PCE]
[GLOBAL CR4_OSFXSR]
[GLOBAL CR4_OSXMMEXCPT]
[GLOBAL CR4_UMIP]
[GLOBAL CR4_LA57]
[GLOBAL CR4_VMXE]
[GLOBAL CR4_SMXE]
[GLOBAL CR4_FSGSBASE]
[GLOBAL CR4_PCID]
[GLOBAL CR4_OSXSAVE]
[GLOBAL CR4_SME]
[GLOBAL CR4_SMAP]
[GLOBAL CR4_PKE]


; Control register 0
CR0_PE         equ 1 << 0            ; Protected Mode Enable 	
CR0_MP         equ 1 << 1            ; Monitor co-processor 	
CR0_EM         equ 1 << 2            ; Emulation 	            
CR0_TS         equ 1 << 3            ; Task switched 	        
CR0_ET         equ 1 << 4            ; Extension type 	      
CR0_NE         equ 1 << 5            ; Numeric error 	        
CR0_WP         equ 1 << 16           ; Write protect 	        
CR0_AM         equ 1 << 18           ; Alignment mask 	      
CR0_NW         equ 1 << 29           ; Not-write through 	    
CR0_CD         equ 1 << 30           ; Cache disable 	        
CR0_PG         equ 1 << 31           ; Paging 	              

; Control register 4
CR4_VME        equ 1 << 0            ; Virtual 8086 Mode Extensions 	
CR4_PVI        equ 1 << 1            ; Protected-mode Virtual Interrupts
CR4_TSD        equ 1 << 2            ; Time Stamp Disable 	
CR4_DE         equ 1 << 3            ; Debugging Extensions 	
CR4_PSE        equ 1 << 4            ; Page Size Extension 	
CR4_PAE        equ 1 << 5            ; Physical Address Extension 	
CR4_MCE        equ 1 << 6            ; Machine Check Exception 	
CR4_PGE        equ 1 << 7            ; Page Global Enabled 	
CR4_PCE        equ 1 << 8            ; Performance-Monitoring Counter enable 	
CR4_OSFXSR     equ 1 << 9            ; Operating system support for FXSAVE and FXRSTOR instructions 	
CR4_OSXMMEXCPT equ 1 << 10           ; Operating System Support for Unmasked SIMD Floating-Point Exceptions
CR4_UMIP       equ 1 << 11           ; User-Mode Instruction Prevention  
CR4_LA57       equ 1 << 12           ; (none specified) 	
CR4_VMXE       equ 1 << 13           ; Virtual Machine Extensions Enable
CR4_SMXE       equ 1 << 14           ; Safer Mode Extensions Enable
CR4_FSGSBASE   equ 1 << 16           ; Enables the instructions RDFSBASE, RDGSBASE, WRFSBASE, and WRGSBASE.
CR4_PCID       equ 1 << 17           ; PCID Enable
CR4_OSXSAVE    equ 1 << 18           ; XSAVE and Processor Extended States Enable 	
CR4_SME        equ 1 << 20           ; Supervisor Mode Execution Protection Enable 	
CR4_SMAP       equ 1 << 21           ; Supervisor Mode Access Prevention Enable 	
CR4_PKE        equ 1 << 22           ; Protection Key Enable 	
