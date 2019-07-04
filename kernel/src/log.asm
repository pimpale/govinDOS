%include "call.mac"
%include "log.mac"
[BITS 64]
[SECTION .text]

; Initializes log ring buffer. Must be called before debug log put
; no args
; returns void
[GLOBAL log_init]
log_init: proc
  ; TODO get lock on ring

  ; fills ring buffer with zero
  mov rax, 0
  mov rcx, LOG_BUF_LEN
  rep stosb

  ; initialize ring write head
  mov rax, log_buf_ptr
  mov [log_write_head_ptr_ptr], rax

  ; TODO release lock on ring

endproc

; Places message (arg1) with length (arg0) onto the debug log
; arg0 string len
; arg1 pointer to string message
; returns void
[GLOBAL log_write]
log_write: proc
  ; TODO get lock on ring

  mov rsi, rbx                       ; set string source
  mov rdi, [log_write_head_ptr_ptr]  ; set string destination
  mov rcx, rax                       ; set size of message

  .loop:
    ; check if we've hit the end
    cmp rcx, 0
    jz .end

    ; if there's going to be an overflow move to the end
    cmp rdi, log_buf_end_ptr
    jb .no_overflow

    sub rdi, LOG_BUF_LEN ; if there is an overflow,

    .no_overflow:
    ; now move it
    movsb
    jmp .loop

  .end:
  mov [log_write_head_ptr_ptr], rdi ; update write_head
  ; TODO release lock
endproc

[SECTION .data]
log_lock_ptr: dq 0
log_write_head_ptr_ptr: dq 0


; This reserves 16 kb of space for the log ring buffer
[SECTION .bss]
log_buf_ptr:
resb LOG_BUF_LEN
log_buf_end_ptr:
