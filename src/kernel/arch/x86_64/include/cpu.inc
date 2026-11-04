; Assembly-visible cpu offsets. Keep synchronized with arch/x86_64/cpu/cpu.h
; static asserts.

%define CPU_SELF 0
%define CPU_CURRENT_THREAD 8
%define CPU_INTERRUPT_FRAME 16
%define CPU_TSS 248
%define CPU_TSS_RSP0 252
%define CPU_STACK_TOP 4096
