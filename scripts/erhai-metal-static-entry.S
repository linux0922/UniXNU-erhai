// Independent PID 1 entry control for the temporary Erhai RAM diagnostic.
// Darwin ARM64 syscall numbers are from bsd/kern/syscalls.master.
.text
.p2align 2
.globl _start
_start:
    mov x16, #20                       // getpid
    svc #0x80
    cmp x0, #1
    b.ne unexpected_process

    adrp x0, log_path@PAGE
    add x0, x0, log_path@PAGEOFF
    mov x1, #0x309                     // O_WRONLY | O_APPEND | O_NOFOLLOW | O_CREAT
    mov x2, #0x180                     // mode 0600
    mov x16, #5                        // open
    svc #0x80
    b.cs idle
    mov x19, x0

    adrp x1, started_message@PAGE
    add x1, x1, started_message@PAGEOFF
    adrp x2, started_end@PAGE
    add x2, x2, started_end@PAGEOFF
    sub x2, x2, x1
    mov x16, #4                        // write
    svc #0x80
    mov x0, x19
    mov x16, #6                        // close
    svc #0x80

idle:
    // PID 1 must stay alive. Sleep via select with a one-second timeout.
    sub sp, sp, #16
wait:
    mov x4, #1
    stp x4, xzr, [sp]
    mov x0, #0
    mov x1, #0
    mov x2, #0
    mov x3, #0
    mov x4, sp
    mov x16, #93                       // select
    svc #0x80
    b wait

unexpected_process:
    mov x0, #6
    mov x16, #1                        // exit
    svc #0x80
    brk #0

.section __TEXT,__const
log_path:
    .asciz "/private/var/tmp/erhai-s9-metal-smoke-private/stderr.log"
started_message:
    .ascii "METAL_SMOKE stage=static_entry reached=1\n"
started_end:
