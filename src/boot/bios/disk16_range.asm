; Load a contiguous raw-image range with classic BIOS CHS reads.
; EDI = flat destination memory address (must stay below 1 MiB)
; AX = start LBA address within the raw image
; DL = drive
; CX = count sectors
;
; This intentionally reads one sector at a time. It is slower than packet
; reads, but it matches the code path that already succeeds while loading
; kernel16.bin itself on BIOS USB boot.

load_range_state		equ LOADER_DISK_RANGE_STATE_ADDRESS
load_range_lba			equ load_range_state + 0
load_range_dest			equ load_range_state + 2
load_range_count		equ load_range_state + 6
load_range_drive		equ load_range_state + 8
load_range_error_status		equ load_range_state + 9
load_range_error_lba		equ load_range_state + 10
load_range_error_cylinder	equ load_range_state + 12
load_range_error_head		equ load_range_state + 14
load_range_error_sector		equ load_range_state + 15
load_range_error_attempt		equ load_range_state + 16
load_range_retry_count		equ load_range_state + 17
load_range_bounce_buffer		equ (LOADER16_LOAD_ADDRESS + (LOADER16_IMAGE_SECTOR_COUNT * 512))
load_range_bounce_segment		equ (load_range_bounce_buffer >> 4)

disk_read_chs_retry_limit	equ 3

disk_read_chs_range:
	mov [load_range_lba], ax
	mov [load_range_dest], edi
	mov [load_range_count], cx
	mov [load_range_drive], dl

.next:
	mov cx, [load_range_count]
	or cx, cx
	jz .done
	mov byte [load_range_retry_count], disk_read_chs_retry_limit

.retry:

	mov ax, [load_range_lba]
	mov [load_range_error_lba], ax
	xor dx, dx
	div word [loader_sectors_per_track]
	mov cl, dl
	inc cl
	mov [load_range_error_sector], cl

	xor dx, dx
	div word [loader_head_count]
	mov [load_range_error_cylinder], ax
	mov dh, dl
	mov [load_range_error_head], dh
	mov ch, al
	and ah, 0x03
	shl ah, 6
	or cl, ah

	mov ax, load_range_bounce_segment
	mov es, ax
	xor bx, bx
	mov ax, 0x0201
	mov dl, [load_range_drive]
	int 0x13
	jc .error

	push ds
	push si
	push di
	push cx

	mov edi, [load_range_dest]
	mov bx, di
	and bx, 0x000F
	shr edi, 4

	mov ax, load_range_bounce_segment
	mov ds, ax
	xor si, si

	mov ax, di
	mov es, ax
	mov di, bx
	mov cx, 256
	cld
	rep movsw

	pop cx
	pop di
	pop si
	pop ds

	add dword [load_range_dest], 512
	inc word [load_range_lba]
	dec word [load_range_count]
	jmp .next

.done:
	clc
	ret

.error:
	mov [load_range_error_status], ah
	mov al, disk_read_chs_retry_limit
	sub al, [load_range_retry_count]
	inc al
	mov [load_range_error_attempt], al
	dec byte [load_range_retry_count]
	jz .failed
	xor ah, ah
	mov dl, [load_range_drive]
	int 0x13
	jmp .retry

.failed:
	stc
	ret

; Transfer one sector between the drive and the bounce buffer with classic CHS.
; AX = LBA address within the raw image
; BL = BIOS function (0x02 read, 0x03 write)
; DL = drive
; Returns CF set on failure after the same retry/reset policy as the range read.
chs_io_lba			dw 0
chs_io_function		db 0
chs_io_drive		db 0
chs_io_retry_count	db 0

disk_chs_bounce_sector:
	push es
	push ax
	push bx
	push cx
	push dx
	mov [chs_io_lba], ax
	mov [chs_io_function], bl
	mov [chs_io_drive], dl
	mov byte [chs_io_retry_count], disk_read_chs_retry_limit

.retry:
	mov ax, [chs_io_lba]
	xor dx, dx
	div word [loader_sectors_per_track]
	mov cl, dl
	inc cl

	xor dx, dx
	div word [loader_head_count]
	mov dh, dl
	mov ch, al
	and ah, 0x03
	shl ah, 6
	or cl, ah

	mov ax, load_range_bounce_segment
	mov es, ax
	xor bx, bx
	mov ah, [chs_io_function]
	mov al, 1
	mov dl, [chs_io_drive]
	int 0x13
	jnc .done

	dec byte [chs_io_retry_count]
	jz .failed
	xor ah, ah
	mov dl, [chs_io_drive]
	int 0x13
	jmp .retry

.failed:
	pop dx
	pop cx
	pop bx
	pop ax
	pop es
	stc
	ret

.done:
	pop dx
	pop cx
	pop bx
	pop ax
	pop es
	clc
	ret
