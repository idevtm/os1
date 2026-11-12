; Thread restore and first scheduler entry for x86_64. C++ owns Thread and
; TrapFrame layout; this file consumes the generated offset constants and turns
; a selected Thread into an iretq return path.

%include "cpu.inc"
%include "thread_layout.inc"
%include "trapframe.inc"

%define KERNEL_DATA_SEGMENT 0x10

global start_multi_task
global enter_first_thread
global kernel_thread_start
global restore_thread
global restore_frame_ptr

enter_first_thread:
	; Keep the old current-thread identity paired with its stack until handoff.
	cli
	mov rsp, rsi
	and rsp, -16
	jmp start_multi_task

start_multi_task:
	jmp restore_thread

kernel_thread_start:
	; Synthetic kernel threads arrive via iretq, so align the stack before
	; calling their C++ entry point in RDI.
	sub rsp, 8
	call rdi
.kernel_thread_returned:
	cli
	hlt
	jmp .kernel_thread_returned

; Install the selected thread into per-CPU state, load its CR3/RSP0, and resume
; from the embedded TrapFrame.
restore_thread:
	; Direct kernel callers must not expose a partially restored context to IRQs.
	cli
	; Leave the outgoing stack AND address space before publishing that it is
	; no longer current. The BSP may then complete its read or reap it. Use the
	; permanent per-CPU stack for restore scratch, never the outgoing stack.
	mov rsp, [gs:CPU_SELF]
	add rsp, CPU_STACK_TOP
	mov rax, [rdi + THREAD_KERNEL_STACK_TOP]
	mov [gs:CPU_TSS_RSP0], rax
	mov rax, [rdi + THREAD_ADDRESS_SPACE_CR3]
	mov cr3, rax
	mov [gs:CPU_CURRENT_THREAD], rdi
	lea rdi, [rdi + THREAD_FRAME]
	jmp restore_frame_ptr

; Restore a TrapFrame already addressed by RDI. Used both for direct resume and
; after trap_dispatch decides to keep the current thread.
restore_frame_ptr:
	mov r11, rdi
	mov rax, [r11 + TF_CS]
	test al, 3
	jnz .resume_user

.kernel_return:
	; IRETQ restores SS:RSP even when it returns to ring 0.
	push qword [r11 + TF_SS]
	push qword [r11 + TF_RSP]
	push qword [r11 + TF_RFLAGS]
	push qword [r11 + TF_CS]
	push qword [r11 + TF_RIP]
	jmp .resume_kernel

.resume_user:
	; IRETQ can fault before it switches stacks. Keep RSP on a kernel stack.
	push qword [r11 + TF_SS]
	push qword [r11 + TF_RSP]
	push qword [r11 + TF_RFLAGS]
	push qword [r11 + TF_CS]
	push qword [r11 + TF_RIP]

	mov r15, [r11 + TF_R15]
	mov r14, [r11 + TF_R14]
	mov r13, [r11 + TF_R13]
	mov r12, [r11 + TF_R12]
	mov r10, [r11 + TF_R10]
	mov r9,  [r11 + TF_R9]
	mov r8,  [r11 + TF_R8]
	mov rbp, [r11 + TF_RBP]
	mov rsi, [r11 + TF_RSI]
	mov rdi, [r11 + TF_RDI]
	mov rdx, [r11 + TF_RDX]
	mov rcx, [r11 + TF_RCX]
	mov rbx, [r11 + TF_RBX]
	mov rax, [r11 + TF_RAX]
	mov r11, [r11 + TF_R11]
	swapgs
	iretq

.resume_kernel:
	mov ax, KERNEL_DATA_SEGMENT
	mov ds, ax
	mov es, ax
	mov ss, ax

	mov r15, [r11 + TF_R15]
	mov r14, [r11 + TF_R14]
	mov r13, [r11 + TF_R13]
	mov r12, [r11 + TF_R12]
	mov r10, [r11 + TF_R10]
	mov r9,  [r11 + TF_R9]
	mov r8,  [r11 + TF_R8]
	mov rbp, [r11 + TF_RBP]
	mov rsi, [r11 + TF_RSI]
	mov rdi, [r11 + TF_RDI]
	mov rdx, [r11 + TF_RDX]
	mov rcx, [r11 + TF_RCX]
	mov rbx, [r11 + TF_RBX]
	mov rax, [r11 + TF_RAX]
	mov r11, [r11 + TF_R11]
	iretq
