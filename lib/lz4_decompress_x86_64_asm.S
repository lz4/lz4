.global LZ4_decompress_asm_loop
.text

#define ip	rcx
#define op	rdx
#define ilimit	r8
#define olimit	r9
#define ostart	r10
#define oend	r11

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
	movq 24(%rdi), %oend

	push %rdi

	movq %oend, %olimit
	subq $LIMIT, %ilimit
	subq $LIMIT, %olimit

	movq %op, %ostart

.L_match_copied:
	movq %ip, %r13
	movq %op, %r14
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
	movzwq 0(%ip), %rdi
	addq $2, %ip
	subq %rdi, %rsi

	cmpq %ostart, %rsi
	jb .L_output_error

	/* TOOD: Handle short offsets */
	cmpq $0xF, %rbx
	je .L_long_match_length

	addq $4, %rbx

	movdqu 0(%rsi), %xmm0
	movups %xmm0, 0(%op)
	movzwq 16(%rsi), %rdi
	movw %di, 16(%op)

	addq %rbx, %op

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
	movq %r13, %ip
	movq %r14, %op
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
.assert_fail:
#endif
