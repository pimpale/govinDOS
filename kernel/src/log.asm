%include "call.mac"
%include "log.mac"
[BITS 64]
[SECTION .text]

; LogEntry:
;   priority: U64
;   time: U64
;   text: U8[0x30]

; Initializes debug log ring buffer. Must be called before debug log put
; no args
; returns void
[GLOBAL dbg_log_init]
dbg_log_init: proc
  ; TODO get lock on ring

  ; fills ring buffer with zero
  mov rax, 0
  mov rcx, DBG_LOG_BUF_LEN
  rep stosb 

  ; initialize ring write head
  mov rax, ring_buffer
  mov [ring_write_head], rax

  ; TODO release lock on ring

endproc


; Places log message in ring log and prints it out to the serial port
; arg0 priority
; arg1 pointer to string message
[GLOBAL dbg_put]
dbg_put: proc
  ; save args
  push rax
  push rbx
  ; print to serial out
  call dbg_serial_put

  ; restore args
  pop rbx
  pop rax

  ; put it in log
  call dbg_log_put
  
endproc 

; Places debug message in serial port
; arg0 priority
; arg1 pointer to string message 
; returns void
[GLOBAL dbg_serial_put]
dbg_serial_put: proc
  mov rdx, DBG_SERIAL_PORT ; print to correct port
  mov rsi, rbx             ; move arg1 to source
  mov rcx, DBG_MSG_SIZE    ; print out fixed num of bytes
  cld                      ; copy forward
  rep outsb                ; output bytes
endproc


; Places debug messge in ring log with priority (arg0) 
; arg0 priority
; arg1 pointer to string message
; returns void
[GLOBAL dbg_log_put]
dbg_log_put: proc 
  ; TODO get lock on ring

  mov rdi, [ring_write_head]
  mov [rdi], rax ; write priority
  
  ; rdi points at slot for priority
  add rdi, QWORD_SIZE
  ; write time
  ; TODO get time, write it
  ; mov [rdi], rax
  
  ; rdi points at slot for message 
  add rdi, QWORD_SIZE

  ; set the source to the string message pointer
  ; rdi already contains source
  mov rsi, rbx

  mov rcx, DBG_MSG_SIZE      ; set size of message
  cld                        ; copy forward
  rep movsb                  ; write message

  ; move the pointer back to if it's too large
  cmp rdi, DBG_LOG_BUF_LEN
  jb .no_overflow 
  sub rdi, DBG_LOG_BUF_LEN

  .no_overflow:
  mov [ring_write_head], rdi ; update write_head
  
  ; TODO release lock
endproc

[SECTION .data]
ring_lock: dq 0
ring_write_head: dq 0


; This reserves 16 kb of space for the kernel ring buffer
[SECTION .bss]
ring_buffer: resb DBG_LOG_BUF_LEN
