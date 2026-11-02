[bits 64]
; Long-mode half of the BIOS boot log support (see bootlog16.asm).

; Append the ASCIIZ string at RSI to the RAM boot log when the loader armed it.
; The loader's early page tables identity-map the log window with a 2 MiB page.
bootlog_append64:
	cmp byte [bootlog_enabled], 0
	je .out
	push rax
	push rcx
	push rdx
	push rsi
	push rdi
	mov rdi, BOOTLOG_PHYSICAL_ADDRESS
	mov ecx, [rdi + BOOTLOG_H_CAPACITY]
.next:
	mov al, [rsi]
	test al, al
	jz .done
	push rax
	mov rax, [rdi + BOOTLOG_H_WRITE_COUNT]
	xor edx, edx
	div rcx
	pop rax
	mov [rdi + BOOTLOG_HEADER_BYTES + rdx], al
	inc qword [rdi + BOOTLOG_H_WRITE_COUNT]
	inc rsi
	jmp .next
.done:
	pop rdi
	pop rsi
	pop rdx
	pop rcx
	pop rax
.out:
	ret

; Spin for roughly RCX microseconds using legacy port 0x80 writes.
bootlog_io_delay64:
	push rax
	xor eax, eax
.loop:
	out 0x80, al
	dec rcx
	jnz .loop
	pop rax
	ret

; Reset the machine without clearing RAM so the next loader run can save the
; log: keyboard-controller pulse, then PCI reset control, then a triple fault.
bootlog_reset64:
	cli
	wbinvd			; write back dirty cache lines; a reset would drop them
	mov rcx, 5000000
	call bootlog_io_delay64

	mov ecx, 100000
.kbc_wait:
	in al, 0x64
	test al, 0x02
	jz .kbc_ready
	dec ecx
	jnz .kbc_wait
.kbc_ready:
	mov al, 0xFE
	out 0x64, al
	mov rcx, 500000
	call bootlog_io_delay64

	mov dx, 0x0CF9
	mov al, 0x02
	out dx, al
	mov al, 0x06
	out dx, al
	mov rcx, 500000
	call bootlog_io_delay64

	lidt [bootlog_null_idt]
	int3
.hang:
	hlt
	jmp .hang

align 4
bootlog_null_idt:
	dw 0
	dq 0
