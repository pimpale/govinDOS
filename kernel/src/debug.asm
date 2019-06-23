%include "call.mac"
%include "debug.mac"

; LogEntry:
;   priority: U64
;   time: U64
;   text: U8[48]

; Initializes debug log ring buffer. Must be called before debug log put
; no args
; returns void
dbg_log_init: proc
  ; TODO get lock on ring

  ; fills ring buffer with zero
  mov rax, 0
  mov rcx, DBG_RING_BUFFER_LEN
  rep stosb 

  ; TODO release lock on ring

endproc

; Places debug messge (48 bytes) in ring log with priority (arg0) 
; arg0 priority
; arg1 pointer to string message
; returns void
dbg_log_put: proc 
  ; TODO get lock on ring

  mov rbx, [ring_write_head]

  ; write priority
  mov [rbx], rax

  add rbx, QWORD_SIZE
  ; write time
  ; TODO get time, write it
  ; mov [rbx], rax

  ; write message

  

  

  
  mov [ring_buffer+*DBG_MSG_SIZE], 0



[SECTION .data]
ring_lock: dq 0
ring_write_head: dq 0

; This reserves 16 kb of space for the kernel ring buffer
[SECTION .bss]

ring_buffer:
resb DBG_RING_BUFFER_LEN
