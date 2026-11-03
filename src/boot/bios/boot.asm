[bits 16]
[org 0x7c00]
%include "../kernel/handoff/memory_layout.inc"
%include "image_layout.inc"

loader_main16		equ LOADER16_LOAD_ADDRESS

_start:
	cli
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov sp, loader_main16
	mov [boot_device], dl

	; Stage 0 only needs to pull the contiguous stage-1 image. Using classic
	; CHS reads here avoids the BIOS EDD path that was overwriting the boot
	; sector immediately after INT 13h returned.
	mov ah, 0x08
	mov dl, [boot_device]
	int 0x13
	jc .stop
	and cx, 0x003F
	mov [sectors_per_track], cx
	xor ax, ax
	mov al, dh
	inc ax
	mov [head_count], ax

	mov word [current_lba], LOADER16_IMAGE_START_LBA
	mov bx, loader_main16
	mov si, 1

.load_next:
	or si, si
	jz .loaded

	mov ax, [current_lba]
	xor dx, dx
	div word [sectors_per_track]
	mov di, dx
	inc di

	xor dx, dx
	div word [head_count]

	mov ch, al
	mov cx, di
	mov al, ah
	and al, 0x03
	shl al, 6
	or cl, al
	mov dh, dl

	mov ax, 0x0201
	mov dl, [boot_device]
	int 0x13
	jc .stop

	add bx, 512
	inc word [current_lba]
	dec si
	jmp .load_next

.loaded:
	cmp word [loader_main16], 0x7733
	jne .stop

	mov dl, [boot_device]
	jmp loader_main16 + 2

.stop:
	hlt
	jmp .stop

boot_device			db 0x00
sectors_per_track	dw 0
head_count			dw 0
current_lba			dw 0

; MBR partition table. Some BIOSes (AMI in particular) only offer a USB stick
; as bootable when it carries an active partition, so describe the raw-image
; area (loader, kernel, initrd, boot log) as one active non-filesystem
; partition. The USB image builder adds the EFI System Partition as entry 2.
%define CHS_CYLINDER(lba) ((lba) / (255 * 63))
%define CHS_HEAD(lba) (((lba) / 63) % 255)
%define CHS_SECTOR(lba) (((lba) % 63) + 1)
%macro CHS_ADDRESS 1
	db CHS_HEAD(%1)
	db CHS_SECTOR(%1) | ((CHS_CYLINDER(%1) >> 2) & 0xC0)
	db CHS_CYLINDER(%1) & 0xFF
%endmacro

times 0x1BE-($-$$) db 0x00
	db 0x80
	CHS_ADDRESS 1
	db 0xDA
	CHS_ADDRESS (RAW_IMAGE_SECTOR_COUNT - 1)
	dd 1
	dd RAW_IMAGE_SECTOR_COUNT - 1
times 510-($-$$) db 0x00
dw 0xAA55
