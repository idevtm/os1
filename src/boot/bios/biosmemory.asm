[bits 16]
; Collect valid E820 entries into ES:DI. The caller supplies one scratch
; entry after BOOT_MEMORY_REGION_CAPACITY. Return the retained count in BP.
do_e820:
    xor ebx, ebx
    xor bp, bp
    mov [cs:.map_start], di
    mov word [cs:.calls_left], 4096
    mov byte [cs:.truncated], 0
.next:
    sub word [cs:.calls_left], 1
    jc .failed
    mov edx, 0x534D4150
    mov eax, 0xe820
    mov dword [es:di + 20], 1
    mov ecx, 24
    int 0x15
    jc .finish
    cmp eax, 0x534D4150
    jne .failed
    cmp cx, 20
    jb .continue
    cmp cx, 24
    jb .length
    test byte [es:di + 20], 1
    jz .continue
.length:
    mov eax, [es:di + 8]
    or eax, [es:di + 12]
    jz .continue
    mov eax, [es:di]
    mov edx, [es:di + 4]
    add eax, [es:di + 8]
    adc edx, [es:di + 12]
    jc .failed

    ; Stop coalescing after an omission so a later entry cannot restore
    ; a part of RAM excluded by an earlier omitted reservation.
    cmp byte [cs:.truncated], 0
    jne .append
    test bp, bp
    jz .append
    mov eax, [es:di - 8]
    cmp eax, [es:di + 16]
    jne .append
    mov eax, [es:di - 4]
    cmp eax, [es:di + 20]
    jne .append
    mov eax, [es:di - 24]
    mov edx, [es:di - 20]
    add eax, [es:di - 16]
    adc edx, [es:di - 12]
    cmp edx, [es:di + 4]
    jne .reverse
    cmp eax, [es:di]
    je .merge
.reverse:
    mov eax, [es:di]
    mov edx, [es:di + 4]
    add eax, [es:di + 8]
    adc edx, [es:di + 12]
    cmp edx, [es:di - 20]
    jne .append
    cmp eax, [es:di - 24]
    jne .append
    mov eax, [es:di]
    mov edx, [es:di + 4]
    mov [es:di - 24], eax
    mov [es:di - 20], edx
.merge:
    mov eax, [es:di + 8]
    mov edx, [es:di + 12]
    add [es:di - 16], eax
    adc [es:di - 12], edx
    jmp .continue

.append:
    cmp bp, BOOT_MEMORY_REGION_CAPACITY
    jb .retain
    cmp byte [cs:.truncated], 0
    jne .omitted
    mov byte [cs:.truncated], 1
    ; Firmware can supply overlaps within the retained E820 records too.
    push di
    mov di, [cs:.map_start]
    mov cx, bp
.retained:
    cmp dword [es:di + 16], 1
    je .retained_next
    call .exclude_reserved
.retained_next:
    add di, 24
    loop .retained
    pop di
.omitted:
    cmp dword [es:di + 16], 1
    je .continue
    call .exclude_reserved
    jmp .continue

    ; ES:DI names a reserved record, retained or omitted.
.exclude_reserved:
    push ebx
    push cx
    push si
    push di
    mov si, di
    mov eax, [es:si]
    mov edx, [es:si + 4]
    add eax, [es:si + 8]
    adc edx, [es:si + 12]
    mov [cs:.reserved_end], eax
    mov [cs:.reserved_end + 4], edx
    mov di, [cs:.map_start]
    mov cx, bp
.clip:
    cmp dword [es:di + 16], 1
    jne .advance

    mov eax, [es:di]
    mov edx, [es:di + 4]
    cmp edx, [cs:.reserved_end + 4]
    ja .advance
    jb .check_end
    cmp eax, [cs:.reserved_end]
    jae .advance
.check_end:
    add eax, [es:di + 8]
    adc edx, [es:di + 12]
    cmp edx, [es:si + 4]
    jb .advance
    ja .overlap
    cmp eax, [es:si]
    jbe .advance
.overlap:
    mov ebx, [es:di + 4]
    cmp ebx, [es:si + 4]
    jb .keep_prefix
    ja .try_suffix
    mov ebx, [es:di]
    cmp ebx, [es:si]
    jb .keep_prefix
.try_suffix:
    sub eax, [cs:.reserved_end]
    sbb edx, [cs:.reserved_end + 4]
    jc .reserve
    mov ebx, eax
    or ebx, edx
    jz .reserve
    mov [es:di + 8], eax
    mov [es:di + 12], edx
    mov eax, [cs:.reserved_end]
    mov edx, [cs:.reserved_end + 4]
    mov [es:di], eax
    mov [es:di + 4], edx
    jmp .advance
.keep_prefix:
    mov eax, [es:si]
    mov edx, [es:si + 4]
    sub eax, [es:di]
    sbb edx, [es:di + 4]
    mov [es:di + 8], eax
    mov [es:di + 12], edx
    jmp .advance
.reserve:
    mov dword [es:di + 16], 2
.advance:
    add di, 24
    dec cx
    jnz .clip
    pop di
    pop si
    pop cx
    pop ebx
    ret
.retain:
    inc bp
    add di, 24
.continue:
    test ebx, ebx
    jnz .next
.finish:
    test bp, bp
    jz .failed
.success:
    clc
    ret
.failed:
    stc
    ret

.map_start: dw 0
.calls_left: dw 0
.truncated: db 0
.reserved_end: dq 0

; CF clear when the retained usable ranges cover [EAX, EDX).
; BP is the retained record count. The input map has excluded reservations.
e820_usable32:
    push eax
    push ebx
    push ecx
    push edi
    push si
    mov ebx, eax
.scan:
    mov edi, ebx
    mov si, [cs:do_e820.map_start]
    mov cx, bp
.next:
    jcxz .pass_done
    cmp dword [es:si + 16], 1
    jne .skip
    cmp dword [es:si + 4], 0
    jne .skip
    mov eax, [es:si]
    cmp eax, ebx
    ja .skip
    cmp dword [es:si + 12], 0
    jne .covered
    add eax, [es:si + 8]
    jc .covered
    cmp eax, edx
    jae .covered
    cmp eax, edi
    jbe .skip
    mov edi, eax
.skip:
    add si, 24
    dec cx
    jmp .next
.pass_done:
    cmp edi, ebx
    jbe .uncovered
    mov ebx, edi
    jmp .scan
.uncovered:
    stc
    jmp .restore
.covered:
    clc
.restore:
    pop si
    pop edi
    pop ecx
    pop ebx
    pop eax
    ret

; Function: check_a20
;
; Purpose: to check the status of the a20 line in a completely self-contained state-preserving way.
;
; Returns: 0 in ax if the a20 line is disabled (memory wraps around)
;          1 in ax if the a20 line is enabled (memory does not wrap around)
 
check_a20:
    pushf
    push bx
    push ds
    push es
    push di
    push si
 
    cli
 
    xor ax, ax ; ax = 0
    mov es, ax
 
    not ax ; ax = 0xFFFF
    mov ds, ax
 
    mov di, BOOT_INFO_ADDRESS
    mov si, BOOT_INFO_ADDRESS + 0x10
 
    mov al, byte [es:di]
    push ax
 
    mov al, byte [ds:si]
    push ax
 
    mov byte [es:di], 0x00
    mov byte [ds:si], 0xFF
 
    cmp byte [es:di], 0xFF
 	je .disabled
 	mov bx, 1
 	jmp .exit
.disabled:
 	mov bx, 0

.exit:
    pop ax
    mov byte [ds:si], al
    pop ax
    mov byte [es:di], al
	mov ax, bx

    pop si
    pop di
    pop es
    pop ds
    pop bx
    popf
 
    ret

; Enable A20: fast method
enable_a20_fast:
	push ax
	in al, 0x92
	or al, 2
	out 0x92, al
	pop ax
	ret
