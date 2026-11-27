/* guest_cpu_hostcall(fn, GuestHostArgs *a): calls host function fn with a guest call's arguments.
 * GuestHostArgs (cpu_internal.h): gpr[8] @0, stack[16] @64, xmm[8] (32-byte slots) @192,
 * rax @448, rdx @456, xmm0 @464, xmm1 @496. */
#if defined(__APPLE__)
#define SYMBOL(name) _##name
#else
#define SYMBOL(name) name
#endif

#if defined(__x86_64__)
    .text
    .globl SYMBOL(guest_cpu_hostcall)
SYMBOL(guest_cpu_hostcall):
    push %rbp
    mov %rsp, %rbp
    push %rbx
    push %r12
    mov %rsi, %r12
    mov %rdi, %r11
    sub $128, %rsp
    xor %ecx, %ecx
1:  mov 64(%r12,%rcx,8), %rdx
    mov %rdx, (%rsp,%rcx,8)
    inc %ecx
    cmp $16, %ecx
    jne 1b
    movdqu 192(%r12), %xmm0
    movdqu 224(%r12), %xmm1
    movdqu 256(%r12), %xmm2
    movdqu 288(%r12), %xmm3
    movdqu 320(%r12), %xmm4
    movdqu 352(%r12), %xmm5
    movdqu 384(%r12), %xmm6
    movdqu 416(%r12), %xmm7
    mov 0(%r12), %rdi
    mov 8(%r12), %rsi
    mov 16(%r12), %rdx
    mov 24(%r12), %rcx
    mov 32(%r12), %r8
    mov 40(%r12), %r9
    mov $8, %eax /* vector registers used, for variadic callees */
    call *%r11
    mov %rax, 448(%r12)
    mov %rdx, 456(%r12)
    movdqu %xmm0, 464(%r12)
    movdqu %xmm1, 496(%r12)
    add $128, %rsp
    pop %r12
    pop %rbx
    pop %rbp
    ret

/* guest_cpu_hostcall_regs(fn, uint64_t r[16], GuestVec v[16], const uint64_t *stack, slots): the
 * same call with the arguments read in place: rdi rsi rdx rcx r8 r9 from r (GuestCpu.r), xmm0-7
 * from v (32-byte slots), `slots` (at most 16) stack arguments from `stack`; rax and rdx go to r,
 * xmm0 and xmm1 to v. */
    .globl SYMBOL(guest_cpu_hostcall_regs)
SYMBOL(guest_cpu_hostcall_regs):
    push %rbp
    mov %rsp, %rbp
    push %rbx
    push %r12
    mov %rsi, %rbx
    mov %rdx, %r12
    mov %rdi, %r11
    sub $128, %rsp
    cmp $16, %r8
    jne 3f
    /* All 16 slots, the usual count: 16 bytes at a time through the vector registers the
     * arguments fill next. */
    movdqu 0(%rcx), %xmm0
    movdqu 16(%rcx), %xmm1
    movdqu 32(%rcx), %xmm2
    movdqu 48(%rcx), %xmm3
    movdqu 64(%rcx), %xmm4
    movdqu 80(%rcx), %xmm5
    movdqu 96(%rcx), %xmm6
    movdqu 112(%rcx), %xmm7
    movdqu %xmm0, 0(%rsp)
    movdqu %xmm1, 16(%rsp)
    movdqu %xmm2, 32(%rsp)
    movdqu %xmm3, 48(%rsp)
    movdqu %xmm4, 64(%rsp)
    movdqu %xmm5, 80(%rsp)
    movdqu %xmm6, 96(%rsp)
    movdqu %xmm7, 112(%rsp)
    jmp 2f
3:  xor %eax, %eax
    test %r8, %r8
    jz 2f
1:  mov (%rcx,%rax,8), %rdx
    mov %rdx, (%rsp,%rax,8)
    inc %rax
    cmp %r8, %rax
    jne 1b
2:  movdqu 0(%r12), %xmm0
    movdqu 32(%r12), %xmm1
    movdqu 64(%r12), %xmm2
    movdqu 96(%r12), %xmm3
    movdqu 128(%r12), %xmm4
    movdqu 160(%r12), %xmm5
    movdqu 192(%r12), %xmm6
    movdqu 224(%r12), %xmm7
    mov 56(%rbx), %rdi
    mov 48(%rbx), %rsi
    mov 16(%rbx), %rdx
    mov 8(%rbx), %rcx
    mov 64(%rbx), %r8
    mov 72(%rbx), %r9
    mov $8, %eax /* vector registers used, for variadic callees */
    call *%r11
    mov %rax, 0(%rbx)
    mov %rdx, 16(%rbx)
    movdqu %xmm0, 0(%r12)
    movdqu %xmm1, 32(%r12)
    add $128, %rsp
    pop %r12
    pop %rbx
    pop %rbp
    ret

#elif defined(__aarch64__)
    .text
    .p2align 2
    .globl SYMBOL(guest_cpu_hostcall)
SYMBOL(guest_cpu_hostcall):
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x19, [sp, #16]
    mov x19, x1
    mov x16, x0
    sub sp, sp, #112          /* stack[2..15]: SysV stack arguments past the 8 AAPCS registers */
    add x10, x19, #80          /* &stack[2] */
    mov x9, #0
1:  ldr x11, [x10, x9, lsl #3]
    str x11, [sp, x9, lsl #3]
    add x9, x9, #1
    cmp x9, #14
    b.ne 1b
    ldr q0, [x19, #192]
    ldr q1, [x19, #224]
    ldr q2, [x19, #256]
    ldr q3, [x19, #288]
    ldr q4, [x19, #320]
    ldr q5, [x19, #352]
    ldr q6, [x19, #384]
    ldr q7, [x19, #416]
    ldp x0, x1, [x19, #0]
    ldp x2, x3, [x19, #16]
    ldp x4, x5, [x19, #32]
    ldp x6, x7, [x19, #48]
    blr x16
    str x0, [x19, #448]
    str x1, [x19, #456]
    str q0, [x19, #464]
    str q1, [x19, #496]
    add sp, sp, #112
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

/* guest_cpu_hostcall_regs (the x86-64 one's comment): x0-x5 from rdi rsi rdx rcx r8 r9, x6 and x7
 * the first two stack arguments, the rest on the stack. */
    .p2align 2
    .globl SYMBOL(guest_cpu_hostcall_regs)
SYMBOL(guest_cpu_hostcall_regs):
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    mov x19, x1
    mov x20, x2
    mov x16, x0
    sub sp, sp, #112          /* stack[2..15] */
    mov x6, #0
    mov x7, #0
    cbz x4, 2f
    ldr x6, [x3]
    cmp x4, #1
    b.eq 2f
    ldr x7, [x3, #8]
    mov x9, #2
1:  cmp x9, x4
    b.hs 2f
    ldr x11, [x3, x9, lsl #3]
    sub x10, x9, #2
    str x11, [sp, x10, lsl #3]
    add x9, x9, #1
    b 1b
2:  ldr q0, [x20, #0]
    ldr q1, [x20, #32]
    ldr q2, [x20, #64]
    ldr q3, [x20, #96]
    ldr q4, [x20, #128]
    ldr q5, [x20, #160]
    ldr q6, [x20, #192]
    ldr q7, [x20, #224]
    ldr x0, [x19, #56]
    ldr x1, [x19, #48]
    ldr x2, [x19, #16]
    ldr x3, [x19, #8]
    ldr x4, [x19, #64]
    ldr x5, [x19, #72]
    blr x16
    str x0, [x19, #0]
    str x1, [x19, #16]
    str q0, [x20, #0]
    str q1, [x20, #32]
    add sp, sp, #112
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
#else
#error guest_cpu_hostcall: unsupported host architecture
#endif
