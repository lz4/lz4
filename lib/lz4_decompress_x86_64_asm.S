.global LZ4_decompress_asm_loop
.text

#define ip	rcx
#define op	rdx
#define ilimit	r8
#define olimit	r9
#define ostart	r10

#define LIMIT	64

LZ4_decompress_asm_loop:
	# push %rax
	push %rbx
	# push %rcx
	# push %rdx
	push %rbp
	# push %rsi
	# push %rdi
	# push %r8
	# push %r9
	# push %r10
	# push %r11
	push %r12
	push %r13
	push %r14
	push %r15

	movq 0(%rdi), %ip
	movq 8(%rdi), %op
	movq 16(%rdi), %iend
	movq 24(%rdi), %oend

	push %rdi

	subq $LIMIT, %iend
	subq $LIMIT, %oend

	movq %op, %ostart

.loop:
	# Load the token
	movzbl 0(%ip), %rax
	addq $1, %ip

	# Load the literal length and match length
	# %rax = literal length
	# %rbx = match length
	movq %rax, %rbx
	shrq $4, %rax
	andq $0xF, %rbx

	cmpq %rax, $0xF
	je .long_literal_length

	# %rax = literal length
	# There is enough space to decode the literals < 15 bytes
	movdqu 0(%ip), %xmm0
	movups %xmm0, 0(%op)

	addq %rax, %ip
	addq %rax, %op

	cmpq %ip, %ilimit
	jb .exit

.literals_copied:
	movq %op, %rsi
	movzwl %ip, %rdi
	addq $2, %ip
	subq %rdi, %rsi

	cmpq %ostart, %rsi
	jb .output_error

	cmpq %rbx, $0xF
	je .long_match_length

	movdqu 0(%rsi), %xmm0
	movzwl 16(%rsi), %rdi
	movups %xmm0, 0(%op)
	movw %rdi, 16(%op)

	addq %rbx, %op

	cmpq %op, %olimit
	jb .exit

.match_copied:

	movq $0, %rax
	jmp .exit

.long_literal_length:
	jmp .literals_copied

.long_match_length:
	jmp .match_copied


.output_error:
	movq $1, %rax
	jmp .exit

.exit:
	pop %rdi
	movq %ip, 0(%rdi)
	movq %op, 8(%rdi)

	# Restore registers
	pop %r15
	pop %r14
	pop %r13
	pop %r12
	# pop %r11
	# pop %r10
	# pop %r9
	# pop %r8
	# pop %rdi
	# pop %rsi
	pop %rbp
	# pop %rdx
	# pop %rcx
	pop %rbx
	# pop %rax
