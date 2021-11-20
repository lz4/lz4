.global LZ4_decompress_asm_loop
.text
#define ip	rcx
#define op	rdx
#define ilimit	r8
#define olimit	r9
#define ostart	r10

#define LIMIT	64

#ifndef NDEBUG

#define assert_le(r1, r2)

#else

#define assert_le(r1, r2)

#endif



LZ4_decompress_asm_loop:
	/* push %rax */
	push %rbx
	/* push %rcx */
	/* push %rdx */
	push %rbp
	/* push %rsi */
	/* push %rdi */
	/* push %r8 */
	/* push %r9 */
	/* push %r10 */
	/* push %r11 */
	push %r12
	push %r13
	push %r14
	push %r15

	movq 0(%rdi), %ip
	movq 8(%rdi), %op
	movq 16(%rdi), %ilimit
	movq 24(%rdi), %olimit

	push %rdi

	subq $LIMIT, %ilimit
	subq $LIMIT, %olimit

	leaq pattern_forward(%rip), %r14
	leaq pattern(%rip), %r15

	movq %op, %ostart

.L_match_copied:
	movq %ip, %r12
	movq %op, %r13
	/* Load the token */
	movzbq 0(%ip), %rax
	addq $1, %ip

	/* Load the literal length and match length */
	/* %rax = literal length */
	/* %rbx = match length */
	movq %rax, %rbx
	shrq $4, %rax
	andq $0xF, %rbx

	cmpq $0xF, %rax
	je .L_long_literal_length

	/* %rax = literal length */
	/* There is enough space to decode the literals < 15 bytes */
	movdqu 0(%ip), %xmm0
	movups %xmm0, 0(%op)

	addq %rax, %ip
	addq %rax, %op

	cmpq %ip, %ilimit
	jb .L_rewind_sequence

.L_literals_copied:
	movq %op, %rsi
	movzwq 0(%ip), %r11
	addq $2, %ip
	subq %r11, %rsi

	cmpq %ostart, %rsi
	jb .L_output_error

	cmpq $0xF, %rbx
	je .L_long_match_length

	cmpq $16, %r11
	jb copy_short_offset

	movdqu 0(%rsi), %xmm0
	movups %xmm0, 0(%op)
	movzwq 16(%rsi), %rdi
	movw %di, 16(%op)

	addq %rbx, %op
	addq $4,   %op

	cmpq %op, %olimit
	jb .L_exit_success

	jmp .L_match_copied

copy_short_offset:
	salq $4, %r11
	movdqu 0(%rsi), %xmm0
	vpshufb (%r15, %r11), %xmm0, %xmm0
	addq (%r14, %r11), %rsi

	movdqu %xmm0, 0(%op)
	movzwq 0(%rsi), %rdi
	movw %di, 16(%op)

	addq %rbx, %op
	addq $4,   %op

	cmpq %op, %olimit
	jb .L_exit_success

	jmp .L_match_copied

.L_long_literal_length:
	movzbq 0(%ip), %rsi
	addq %rsi, %rax
	addq $1, %ip

	cmpq $0xFF, %rsi
	je very_long_literal_length

.L_literal_length_computed:
	movq %ip, %rsi
	movq %op, %rdi
	addq %rax, %ip
	addq %rax, %op

	cmpq %ip, %ilimit
	jb .L_rewind_sequence
	cmpq %op, %olimit
	jb .L_rewind_sequence

.L_literal_copy_loop:
	movdqu  0(%rsi), %xmm0
	movdqu 16(%rsi), %xmm1
	addq $32, %rsi
	movups %xmm0,  0(%rdi)
	movups %xmm1, 16(%rdi)
	addq $32, %rdi

	cmpq %ip, %rsi
	jb .L_literal_copy_loop

	jmp .L_literals_copied

very_long_literal_length:
	movzbq 0(%ip), %rdi
	addq %rdi, %rax
	addq $1, %ip
	cmpq %ip, %ilimit
	jb .L_rewind_sequence
	cmpq $0xFF, %rdi
	je very_long_literal_length

	jmp .L_literal_length_computed

.L_long_match_length:
	movzbq 0(%ip), %rdi
	addq %rdi, %rbx
	addq $4, %rbx
	addq $1, %ip

	cmpq $0xFF, %rdi
	je very_long_match_length

.L_match_length_computed:
	movq %op, %rdi
	addq %rbx, %op

	cmpq %op, %olimit
	jb .L_rewind_sequence

	cmpq $16, %r11
	jb copy_short_offset_loop_header

	/* TODO: Handle short offsets */

.L_match_copy_loop:
	movdqu  0(%rsi), %xmm0
	movups %xmm0,  0(%rdi)
	movdqu 16(%rsi), %xmm0
	movups %xmm0, 16(%rdi)
	addq $32, %rsi
	addq $32, %rdi

	cmpq %op, %rdi
	jb .L_match_copy_loop

	jmp .L_match_copied

copy_short_offset_loop_header:
	salq $4, %r11
	movdqu 0(%rsi), %xmm0
	vpshufb (%r15, %r11), %xmm0, %xmm0
	movdqu %xmm0, 0(%rdi)
	addq (%r14, %r11), %rsi
	addq $16, %rdi
	jmp .L_match_copy_loop

very_long_match_length:
	movzbq 0(%ip), %rdi
	addq %rdi, %rbx
	addq $1, %ip
	cmpq %ip, %ilimit
	jb .L_rewind_sequence
	cmpq $0xFF, %rdi
	je very_long_match_length

	jmp .L_match_length_computed

.L_very_long_match_length_too_long:
	movq %r12, %ip
	subq $4, %ip
	subq %rax, %ip
	subq %rax, %op
	jmp .L_exit_success

.L_rewind_sequence:
	movq %r12, %ip
	movq %r13, %op
	jmp .L_exit_success

.L_output_error:
	movq $1, %rax
	jmp .L_exit

.L_exit_success:
	movq $0, %rax
	jmp .L_exit

.L_exit:
	pop %rdi
	movq %ip, 0(%rdi)
	movq %op, 8(%rdi)

	/* Restore registers */
	pop %r15
	pop %r14
	pop %r13
	pop %r12
	/* pop %r11
	 * pop %r10
	 * pop %r9
	 * pop %r8
	 * pop %rdi
	 * pop %rsi
	 */
	pop %rbp
	/* pop %rdx
	 * pop %rcx
	 */
	pop %rbx
	/* pop %rax */

	ret

#ifndef NDEBUG
.L_assert_fail:
#endif
        .section        .rodata
	.align 8
pattern_forward:
	.quad 0 /* 0 */
	.quad 0 /* padding */
	.quad 0 /* 1 */
	.quad 0 /* padding */
	.quad 0 /* 2 */
	.quad 0 /* padding */
	.quad 1 /* 3 */
	.quad 0 /* padding */
	.quad 0 /* 4 */
	.quad 0 /* padding */
	.quad 1 /* 5 */
	.quad 0 /* padding */
	.quad 4 /* 6 */
	.quad 0 /* padding */
	.quad 2 /* 7 */
	.quad 0 /* padding */
	.quad 0 /* 8 */
	.quad 0 /* padding */
	.quad 7 /* 9 */
	.quad 0 /* padding */
	.quad 6 /* 10 */
	.quad 0 /* padding */
	.quad 5 /* 11 */
	.quad 0 /* padding */
	.quad 4 /* 12 */
	.quad 0 /* padding */
	.quad 3 /* 13 */
	.quad 0 /* padding */
	.quad 2 /* 14 */
	.quad 0 /* padding */
	.quad 1 /* 15 */
	.align 16
pattern:
	.quad   0 /* padding */
	.quad   0 /* padding */
        .byte   0
        .byte   0
        .byte   0
        .byte   0
        .byte   0
        .byte   0
        .byte   0
        .byte   0
        .byte   0
        .byte   0
        .byte   0
        .byte   0
        .byte   0
        .byte   0
        .byte   0
        .byte   0
        .byte   0
        .byte   1
        .byte   0
        .byte   1
        .byte   0
        .byte   1
        .byte   0
        .byte   1
        .byte   0
        .byte   1
        .byte   0
        .byte   1
        .byte   0
        .byte   1
        .byte   0
        .byte   1
        .byte   0
        .byte   1
        .byte   2
        .byte   0
        .byte   1
        .byte   2
        .byte   0
        .byte   1
        .byte   2
        .byte   0
        .byte   1
        .byte   2
        .byte   0
        .byte   1
        .byte   2
        .byte   0
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   0
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   5
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   5
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   5
        .byte   6
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   5
        .byte   6
        .byte   0
        .byte   1
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   5
        .byte   6
        .byte   7
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   5
        .byte   6
        .byte   7
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   5
        .byte   6
        .byte   7
        .byte   8
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   5
        .byte   6
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   5
        .byte   6
        .byte   7
        .byte   8
        .byte   9
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   5
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   5
        .byte   6
        .byte   7
        .byte   8
        .byte   9
        .byte   10
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   5
        .byte   6
        .byte   7
        .byte   8
        .byte   9
        .byte   10
        .byte   11
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   5
        .byte   6
        .byte   7
        .byte   8
        .byte   9
        .byte   10
        .byte   11
        .byte   12
        .byte   0
        .byte   1
        .byte   2
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   5
        .byte   6
        .byte   7
        .byte   8
        .byte   9
        .byte   10
        .byte   11
        .byte   12
        .byte   13
        .byte   0
        .byte   1
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   5
        .byte   6
        .byte   7
        .byte   8
        .byte   9
        .byte   10
        .byte   11
        .byte   12
        .byte   13
        .byte   14
        .byte   0
        .byte   0
        .byte   1
        .byte   2
        .byte   3
        .byte   4
        .byte   5
        .byte   6
        .byte   7
        .byte   8
        .byte   9
        .byte   10
        .byte   11
        .byte   12
        .byte   13
        .byte   14
        .byte   15
	.text
