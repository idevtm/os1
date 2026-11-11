; Common trap-entry path for IRQs and exceptions. It saves
; registers into either the current Thread frame or the per-CPU interrupt frame,
; calls C++ trap_dispatch, then resumes or switches threads.

%include "cpu.inc"
%include "thread_layout.inc"
%include "trapframe.inc"

extern trap_dispatch
extern restore_thread
extern restore_frame_ptr

%define USER_DATA_SEGMENT 0x1b
%define USER_CODE_SEGMENT 0x23
%define THREAD_STATE_RUNNING 2
%define IA32_GS_BASE 0xC0000101
%define FIRST_IRQ_VECTOR 32

global trap_entry_common
trap_entry_common:
	push rax
	push rdx

	test byte [rsp + 40], 3
	jnz .swap_user_gs
	cmp dword [rsp + 16], FIRST_IRQ_VECTOR
	jae .kernel_gs

	; A kernel exception can interrupt a GS transition. This includes the
	; IST entries for NMI, double fault, and machine check. The active GS
	; base is high for the CPU record and low for user mode.
	push rcx
	mov ecx, IA32_GS_BASE
	rdmsr
	pop rcx
	test edx, edx
	js .kernel_gs
	; A kernel exception is fatal. It does not resume the interrupted code.

.swap_user_gs:
	swapgs
.kernel_gs:
	lfence
	mov rax, [gs:CPU_CURRENT_THREAD]
	test rax, rax
	jz .cpu_frame

	mov rdx, [rsp + 40]
	test dl, 3
	jnz .thread_frame
	cmp byte [rax + THREAD_USER_MODE], 0
	jne .cpu_frame

.thread_frame:
	lea rax, [rax + THREAD_FRAME]
	jmp .save_frame

.cpu_frame:
	mov rax, [gs:CPU_SELF]
	add rax, CPU_INTERRUPT_FRAME

.save_frame:
	mov rdx, [rsp]
	mov [rax + TF_RDX], rdx
	mov rdx, [rsp + 8]
	mov [rax + TF_RAX], rdx
	mov [rax + TF_RBX], rbx
	mov [rax + TF_RCX], rcx
	mov [rax + TF_RSI], rsi
	mov [rax + TF_RDI], rdi
	mov [rax + TF_RBP], rbp
	mov [rax + TF_R8], r8
	mov [rax + TF_R9], r9
	mov [rax + TF_R10], r10
	mov [rax + TF_R11], r11
	mov [rax + TF_R12], r12
	mov [rax + TF_R13], r13
	mov [rax + TF_R14], r14
	mov [rax + TF_R15], r15

	mov rdx, [rsp + 16]
	mov [rax + TF_VECTOR], rdx
	mov rdx, [rsp + 24]
	mov [rax + TF_ERROR_CODE], rdx
	mov rdx, [rsp + 32]
	mov [rax + TF_RIP], rdx
	mov rdx, [rsp + 40]
	mov [rax + TF_CS], rdx
	mov rdx, [rsp + 48]
	mov [rax + TF_RFLAGS], rdx

	mov rdx, [rax + TF_CS]
	test dl, 3
	jz .kernel_rsp
	mov rdx, [rsp + 56]
	mov [rax + TF_RSP], rdx
	mov rdx, [rsp + 64]
	mov [rax + TF_SS], rdx
	jmp .dispatch

.kernel_rsp:
	lea rdx, [rsp + 56]
	mov [rax + TF_RSP], rdx
	mov qword [rax + TF_SS], 0

.dispatch:
	mov r12, rax
	mov rdi, rax
	mov r13, rsp
	and rsp, -16
	call trap_dispatch
	mov rsp, r13
	test rax, rax
	jz .resume_frame
	mov rbx, [gs:CPU_CURRENT_THREAD]
	cmp rax, rbx
	jne .switch_thread
	mov rdx, [r12 + TF_CS]
	test dl, 3
	jnz .switch_thread
	jmp .resume_frame

.switch_thread:
	mov rdi, rax
	jmp restore_thread

.resume_frame:
	mov rdi, r12
	jmp restore_frame_ptr

global syscall_entry
syscall_entry:
	; SYSCALL changes privilege without changing RSP or the GS base.
	swapgs
	lfence
	cld
	mov [gs:CPU_INTERRUPT_FRAME + TF_R10], r10
	mov r10, [gs:CPU_CURRENT_THREAD]
	test r10, r10
	jz .no_thread
	lea r10, [r10 + THREAD_FRAME]

	mov [r10 + TF_R15], r15
	mov [r10 + TF_R14], r14
	mov [r10 + TF_R13], r13
	mov [r10 + TF_R12], r12
	mov [r10 + TF_RDX], rdx
	mov rdx, [gs:CPU_INTERRUPT_FRAME + TF_R10]
	mov [r10 + TF_R10], rdx
	mov [r10 + TF_R11], r11
	mov [r10 + TF_R9], r9
	mov [r10 + TF_R8], r8
	mov [r10 + TF_RBP], rbp
	mov [r10 + TF_RDI], rdi
	mov [r10 + TF_RSI], rsi
	mov [r10 + TF_RCX], rcx
	mov [r10 + TF_RBX], rbx
	mov [r10 + TF_RAX], rax

	mov qword [r10 + TF_VECTOR], 0x80
	mov qword [r10 + TF_ERROR_CODE], 0
	mov [r10 + TF_RIP], rcx
	mov qword [r10 + TF_CS], USER_CODE_SEGMENT
	mov [r10 + TF_RFLAGS], r11
	mov [r10 + TF_RSP], rsp
	mov qword [r10 + TF_SS], USER_DATA_SEGMENT

	mov r12, r10
	mov r13, [gs:CPU_CURRENT_THREAD]
	mov rsp, [r13 + THREAD_KERNEL_STACK_TOP]
	and rsp, -16
	mov rdi, r12
	call trap_dispatch
	test rax, rax
	jz .resume_frame

	mov rbx, [gs:CPU_CURRENT_THREAD]
	cmp rax, rbx
	jne .switch_thread
	cmp dword [rax + THREAD_STATE], THREAD_STATE_RUNNING
	jne .switch_thread

	; Keep the kernel stack for any return outside the lower canonical half.
	; SYSRET can fault in ring 0 after RSP contains the user stack pointer.
	mov rcx, [r12 + TF_RIP]
	shr rcx, 47
	jnz .resume_frame

	sub rsp, 8
	mov rbx, [r12 + TF_RSP]
	mov [rsp], rbx

	mov r15, [r12 + TF_R15]
	mov r14, [r12 + TF_R14]
	mov r13, [r12 + TF_R13]
	mov r10, [r12 + TF_R10]
	mov r9,  [r12 + TF_R9]
	mov r8,  [r12 + TF_R8]
	mov rbp, [r12 + TF_RBP]
	mov rdi, [r12 + TF_RDI]
	mov rsi, [r12 + TF_RSI]
	mov rdx, [r12 + TF_RDX]
	mov rbx, [r12 + TF_RBX]
	mov rax, [r12 + TF_RAX]
	mov rcx, [r12 + TF_RIP]
	mov r11, [r12 + TF_RFLAGS]
	mov r12, [r12 + TF_R12]
	swapgs
	mov rsp, [rsp]
	o64 sysret

.switch_thread:
	mov rdi, rax
	jmp restore_thread

.resume_frame:
	mov rdi, r12
	jmp restore_frame_ptr

.no_thread:
	cli
	hlt
	jmp .no_thread

%macro LEGACY_IRQ_STUB 2
global irq%1
global irq_vector_%2
irq%1:
irq_vector_%2:
	push qword 0
	push qword %2
	jmp trap_entry_common
%endmacro

LEGACY_IRQ_STUB 0, 32
LEGACY_IRQ_STUB 1, 33
LEGACY_IRQ_STUB 2, 34
LEGACY_IRQ_STUB 3, 35
LEGACY_IRQ_STUB 4, 36
LEGACY_IRQ_STUB 5, 37
LEGACY_IRQ_STUB 6, 38
LEGACY_IRQ_STUB 7, 39
LEGACY_IRQ_STUB 8, 40
LEGACY_IRQ_STUB 9, 41
LEGACY_IRQ_STUB 10, 42
LEGACY_IRQ_STUB 11, 43
LEGACY_IRQ_STUB 12, 44
LEGACY_IRQ_STUB 13, 45
LEGACY_IRQ_STUB 14, 46
LEGACY_IRQ_STUB 15, 47

%assign vector 48
%rep 208
%if vector <> 128
global irq_vector_%+vector
irq_vector_%+vector:
	push qword 0
	push qword vector
	jmp trap_entry_common
%endif
%assign vector vector + 1
%endrep

section .rodata
align 8
global interrupt_vector_stub_table
interrupt_vector_stub_table:
%assign vector 0
%rep 256
%if (vector < 32) || (vector = 128)
	dq 0
%else
	dq irq_vector_%+vector
%endif
%assign vector vector + 1
%endrep
