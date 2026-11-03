[bits 64]
VIDEO_MEM equ 0xB8000
VIDEO_ROW_STRIDE equ BOOT_TEXT_COLUMNS * 2
; print64 console output (also appended to the RAM boot log when armed)
print64:
	call bootlog_append64
	mov edi, VIDEO_MEM
	; compose edi	
	movzx eax, word [boot_info + boot_info_struct.text_console + boot_text_console_info_struct.cursor_y]
	imul eax, VIDEO_ROW_STRIDE
	add edi, eax
	movzx eax, word [boot_info + boot_info_struct.text_console + boot_text_console_info_struct.cursor_x]
	shl eax, 1
	add edi, eax

.go:
	mov al, [esi]
	cmp al, 0
	je .done
	
	; \n
	cmp al, 0xa
	jne .l1
	mov eax, edi
	sub eax, VIDEO_MEM
	mov ecx, VIDEO_ROW_STRIDE
	xor edx, edx
	idiv ecx
	inc eax
	cmp eax, BOOT_TEXT_ROWS
	jne .l2
	; scroll up
	mov ecx, (BOOT_TEXT_ROWS - 1) * VIDEO_ROW_STRIDE / 8
	push rsi
	push rdi
	push rax
	mov esi, VIDEO_MEM + VIDEO_ROW_STRIDE
	mov edi, VIDEO_MEM
	rep movsq 
	mov ecx, VIDEO_ROW_STRIDE / 8
	mov rax, 0x0720072007200720
	rep stosq
	pop rax
	pop rdi
	pop rsi
	dec eax
.l2:	
	imul eax, VIDEO_ROW_STRIDE
	add eax, VIDEO_MEM
	mov edi, eax
	jmp .next
.l1:	
	mov [edi], al
	inc edi
	inc edi
	; if edi overflows, scroll up
	cmp edi, VIDEO_MEM + VIDEO_ROW_STRIDE * BOOT_TEXT_ROWS
	jne .next
	sub edi, VIDEO_ROW_STRIDE
	; scroll up
	mov ecx, (BOOT_TEXT_ROWS - 1) * VIDEO_ROW_STRIDE / 8
	push rsi
	push rdi
	mov esi, VIDEO_MEM + VIDEO_ROW_STRIDE
	mov edi, VIDEO_MEM
	rep movsq 
	mov ecx, VIDEO_ROW_STRIDE / 8
	mov rax, 0x0720072007200720
	rep stosq
	pop rdi
	pop rsi	
.next:
	inc esi
	jmp .go
.done:
	mov eax, edi
	sub eax, VIDEO_MEM
	mov ebx, eax
	shr ebx, 1
	mov ecx, VIDEO_ROW_STRIDE
	xor edx, edx
	idiv ecx
	mov word [boot_info + boot_info_struct.text_console + boot_text_console_info_struct.cursor_y], ax
	shr edx, 1
	mov word [boot_info + boot_info_struct.text_console + boot_text_console_info_struct.cursor_x], dx

	; cursor low port to VGA index register
	mov al, 0Fh
	mov dx, 3D4h
	out dx, al
 
	; cursor low position to VGA data register
	mov ax, bx
	mov dx, 3D5h
	out dx, al
 
	; cursor high port to VGA index register
	mov al, 0Eh
	mov dx, 3D4h
	out dx, al
 
	; cursor high position to VGA data register
	mov ax, bx
	shr ax, 8
	mov dx, 3D5h
	out dx, al
	ret
	
; print rax as hex	
print64_qhex:
	push rbx
	push rcx
	push rdi
	push rsi
	
	mov rdi, .buffer
	add rdi, 16
	xor bl, bl
	mov byte [rdi], bl
	dec rdi
	mov rcx, 16
.l1:
	mov rsi, rax
	and rsi, 0xF
	mov bl, byte [hexdigit + rsi]
	mov byte [rdi], bl
	dec rdi
	shr rax, 4
	loop .l1
	
	mov rsi, .buffer
	call print64
	
	pop rsi
	pop rdi
	pop rcx
	pop rbx

	ret
.buffer: times 17 db 0
