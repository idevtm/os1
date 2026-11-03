[bits 16]
; Real-hardware boot log support for the BIOS loader.
;
; The kernel appends its debug output to a RAM window at BOOTLOG_PHYSICAL_ADDRESS.
; When a boot fails, the kernel (or the hardware watchdog) resets the machine,
; and the next run of this loader finds the previous log still in RAM. While the
; BIOS is still usable in real mode, the loader copies that log into one of the
; reserved slots on the boot disk, records the outcome in the journal sector,
; and re-arms a fresh header for the boot that follows.
;
; On-disk and in-RAM formats are described in doc/2026-11-02-real-hardware-bootlog.md
; and decoded by tools/bootlog_extract.py.

bootlog_journal_buffer		equ load_range_bounce_buffer + 512

BOOTLOG_MAGIC_LO			equ (BOOTLOG_MAGIC & 0xFFFFFFFF)
BOOTLOG_MAGIC_HI			equ (BOOTLOG_MAGIC >> 32)
BOOTLOG_CAPACITY_BYTES		equ (BOOTLOG_REGION_BYTES - BOOTLOG_HEADER_BYTES)

; RAM header field offsets.
BOOTLOG_H_MAGIC				equ 0
BOOTLOG_H_VERSION			equ 8
BOOTLOG_H_HEADER_BYTES		equ 12
BOOTLOG_H_CAPACITY			equ 16
BOOTLOG_H_FLAGS				equ 20
BOOTLOG_H_WRITE_COUNT		equ 24
BOOTLOG_H_BOOT_SEQUENCE		equ 32
BOOTLOG_H_SAVED_SEQUENCE	equ 36

; Journal sector ("OS1BJRN1") field offsets.
BOOTLOG_JOURNAL_MAGIC_LO	equ 0x4231534F
BOOTLOG_JOURNAL_MAGIC_HI	equ 0x314E524A
BOOTLOG_JOURNAL_VERSION		equ 1
BOOTLOG_J_MAGIC				equ 0
BOOTLOG_J_VERSION			equ 8
BOOTLOG_J_BOOT_COUNT		equ 12
BOOTLOG_J_LAST_SEQUENCE		equ 16
BOOTLOG_J_NEXT_SLOT			equ 20
BOOTLOG_J_LAST_RESULT		equ 24
BOOTLOG_J_DRIVE				equ 25
BOOTLOG_J_SECTORS_PER_TRACK	equ 26
BOOTLOG_J_HEAD_COUNT		equ 28
BOOTLOG_J_LAST_SLOT			equ 30
BOOTLOG_J_RAW_MAGIC			equ 32
BOOTLOG_J_RAW_WRITE_COUNT	equ 40
BOOTLOG_J_RAW_FLAGS			equ 48
BOOTLOG_J_RAW_BOOT_SEQUENCE	equ 52
BOOTLOG_J_HISTORY			equ 64
BOOTLOG_J_HISTORY_ENTRIES	equ 32

; Per-boot outcome codes recorded in the journal.
BOOTLOG_RESULT_NONE				equ 0
BOOTLOG_RESULT_SAVED			equ 1
BOOTLOG_RESULT_BAD_HEADER		equ 2
BOOTLOG_RESULT_WRITE_FAILED		equ 3
BOOTLOG_RESULT_REGION_UNUSABLE	equ 4

bootlog_enabled			db 0
bootlog_result			db BOOTLOG_RESULT_NONE
bootlog_save_slot		db 0xFF
bootlog_journal_failed	db 0
bootlog_save_sectors	dw 0
bootlog_save_index		dw 0
bootlog_save_lba		dw 0

; Load FS with a flat 4 GiB data descriptor and drop back to real mode so the
; loader can reach the RAM log above 1 MiB with 32-bit offsets.
enter_unreal16:
	pushf
	push eax
	push bx
	cli
	lgdt [unreal_gdt_pointer]
	mov eax, cr0
	or al, 1
	mov cr0, eax
	jmp short .protected
.protected:
	mov bx, 0x08
	mov fs, bx
	and al, 0xFE
	mov cr0, eax
	jmp short .real
.real:
	xor bx, bx
	mov fs, bx
	pop bx
	pop eax
	popf
	ret

; CF clear when the whole RAM log window lies inside one usable E820 entry.
bootlog_region_usable16:
	push eax
	push cx
	push edx
	push si
	mov cx, [boot_info + boot_info_struct.memory_map_count]
	mov si, boot_memory_map
.next:
	jcxz .unusable
	cmp dword [si + boot_memory_region_struct.type], 1
	jne .skip
	cmp dword [si + boot_memory_region_struct.physical_start + 4], 0
	jne .skip
	mov eax, [si + boot_memory_region_struct.physical_start]
	cmp eax, BOOTLOG_PHYSICAL_ADDRESS
	ja .skip
	mov edx, [si + boot_memory_region_struct.length + 4]
	add eax, [si + boot_memory_region_struct.length]
	adc edx, 0
	jnz .usable
	cmp eax, BOOTLOG_PHYSICAL_ADDRESS + BOOTLOG_REGION_BYTES
	jae .usable
.skip:
	add si, boot_memory_region_struct_size
	dec cx
	jmp .next
.unusable:
	stc
	jmp .out
.usable:
	clc
.out:
	pop si
	pop edx
	pop cx
	pop eax
	ret

bootlog_reset_journal16:
	push ax
	push cx
	push di
	mov di, bootlog_journal_buffer
	mov cx, 256
	xor ax, ax
	cld
	rep stosw
	mov dword [bootlog_journal_buffer + BOOTLOG_J_MAGIC], BOOTLOG_JOURNAL_MAGIC_LO
	mov dword [bootlog_journal_buffer + BOOTLOG_J_MAGIC + 4], BOOTLOG_JOURNAL_MAGIC_HI
	mov dword [bootlog_journal_buffer + BOOTLOG_J_VERSION], BOOTLOG_JOURNAL_VERSION
	pop di
	pop cx
	pop ax
	ret

; Copy 512 bytes between the bounce buffer and the journal buffer.
; SI = source, DI = destination (both low-memory offsets with DS = ES = 0)
bootlog_copy_sector16:
	push cx
	push si
	push di
	mov cx, 256
	cld
	rep movsw
	pop di
	pop si
	pop cx
	ret

; Save the previous boot's RAM log (if any), update the journal, and arm a fresh
; RAM log header for this boot. Requires the E820 map in boot_info. Every disk
; failure is reported but non-fatal: logging must never prevent a boot attempt.
bootlog_prepare16:
	pushad
	push es
	xor ax, ax
	mov es, ax
	mov byte [bootlog_enabled], 0
	mov byte [bootlog_result], BOOTLOG_RESULT_NONE
	mov byte [bootlog_save_slot], 0xFF
	mov byte [bootlog_journal_failed], 0
	mov word [bootlog_save_sectors], 0

	mov ax, BOOTLOG_DISK_JOURNAL_LBA
	mov bl, 0x02
	mov dl, [boot_device]
	call disk_chs_bounce_sector
	jnc .journal_read
	mov byte [bootlog_journal_failed], 1
	call bootlog_reset_journal16
	jmp .journal_ready
.journal_read:
	mov si, load_range_bounce_buffer
	mov di, bootlog_journal_buffer
	call bootlog_copy_sector16
	cmp dword [bootlog_journal_buffer + BOOTLOG_J_MAGIC], BOOTLOG_JOURNAL_MAGIC_LO
	jne .journal_init
	cmp dword [bootlog_journal_buffer + BOOTLOG_J_MAGIC + 4], BOOTLOG_JOURNAL_MAGIC_HI
	jne .journal_init
	cmp dword [bootlog_journal_buffer + BOOTLOG_J_VERSION], BOOTLOG_JOURNAL_VERSION
	je .journal_ready
.journal_init:
	call bootlog_reset_journal16
.journal_ready:
	inc dword [bootlog_journal_buffer + BOOTLOG_J_BOOT_COUNT]
	mov al, [boot_device]
	mov [bootlog_journal_buffer + BOOTLOG_J_DRIVE], al
	mov ax, [loader_sectors_per_track]
	mov [bootlog_journal_buffer + BOOTLOG_J_SECTORS_PER_TRACK], ax
	mov ax, [loader_head_count]
	mov [bootlog_journal_buffer + BOOTLOG_J_HEAD_COUNT], ax

	call bootlog_region_usable16
	jnc .region_ok
	mov byte [bootlog_result], BOOTLOG_RESULT_REGION_UNUSABLE
	jmp .record

.region_ok:
	call enter_unreal16
	mov esi, BOOTLOG_PHYSICAL_ADDRESS
	; Record what this boot found in RAM so the journal shows whether memory
	; survived the reset even when no valid log was present.
	mov eax, [fs:esi + BOOTLOG_H_MAGIC]
	mov [bootlog_journal_buffer + BOOTLOG_J_RAW_MAGIC], eax
	mov eax, [fs:esi + BOOTLOG_H_MAGIC + 4]
	mov [bootlog_journal_buffer + BOOTLOG_J_RAW_MAGIC + 4], eax
	mov eax, [fs:esi + BOOTLOG_H_WRITE_COUNT]
	mov [bootlog_journal_buffer + BOOTLOG_J_RAW_WRITE_COUNT], eax
	mov eax, [fs:esi + BOOTLOG_H_WRITE_COUNT + 4]
	mov [bootlog_journal_buffer + BOOTLOG_J_RAW_WRITE_COUNT + 4], eax
	mov eax, [fs:esi + BOOTLOG_H_FLAGS]
	mov [bootlog_journal_buffer + BOOTLOG_J_RAW_FLAGS], eax
	mov eax, [fs:esi + BOOTLOG_H_BOOT_SEQUENCE]
	mov [bootlog_journal_buffer + BOOTLOG_J_RAW_BOOT_SEQUENCE], eax

	cmp dword [fs:esi + BOOTLOG_H_MAGIC], BOOTLOG_MAGIC_LO
	jne .arm
	cmp dword [fs:esi + BOOTLOG_H_MAGIC + 4], BOOTLOG_MAGIC_HI
	jne .arm
	mov byte [bootlog_result], BOOTLOG_RESULT_BAD_HEADER
	cmp dword [fs:esi + BOOTLOG_H_VERSION], BOOTLOG_VERSION
	jne .arm
	cmp dword [fs:esi + BOOTLOG_H_HEADER_BYTES], BOOTLOG_HEADER_BYTES
	jne .arm
	cmp dword [fs:esi + BOOTLOG_H_CAPACITY], BOOTLOG_CAPACITY_BYTES
	jne .arm

	; Save the header plus min(write_count, capacity) bytes of ring data.
	mov ecx, BOOTLOG_CAPACITY_BYTES
	cmp dword [fs:esi + BOOTLOG_H_WRITE_COUNT + 4], 0
	jne .size_ready
	mov eax, [fs:esi + BOOTLOG_H_WRITE_COUNT]
	cmp eax, ecx
	jae .size_ready
	mov ecx, eax
.size_ready:
	add ecx, BOOTLOG_HEADER_BYTES + 511
	shr ecx, 9
	mov [bootlog_save_sectors], cx

	mov eax, [bootlog_journal_buffer + BOOTLOG_J_LAST_SEQUENCE]
	inc eax
	mov [bootlog_journal_buffer + BOOTLOG_J_LAST_SEQUENCE], eax
	mov [fs:esi + BOOTLOG_H_SAVED_SEQUENCE], eax

	mov eax, [bootlog_journal_buffer + BOOTLOG_J_NEXT_SLOT]
	cmp eax, BOOTLOG_DISK_SLOT_COUNT
	jb .slot_ready
	xor eax, eax
.slot_ready:
	mov [bootlog_save_slot], al
	mov cx, BOOTLOG_DISK_SLOT_SECTORS
	mul cx
	add ax, BOOTLOG_DISK_SLOT_BASE_LBA
	mov [bootlog_save_lba], ax
	mov word [bootlog_save_index], 0

.save_next:
	mov cx, [bootlog_save_index]
	cmp cx, [bootlog_save_sectors]
	jae .saved
	call enter_unreal16
	movzx esi, cx
	shl esi, 9
	add esi, BOOTLOG_PHYSICAL_ADDRESS
	mov di, load_range_bounce_buffer
	mov cx, 128
.copy_dword:
	mov eax, [fs:esi]
	mov [di], eax
	add esi, 4
	add di, 4
	dec cx
	jnz .copy_dword
	mov ax, [bootlog_save_lba]
	add ax, [bootlog_save_index]
	mov bl, 0x03
	mov dl, [boot_device]
	call disk_chs_bounce_sector
	jc .save_failed
	inc word [bootlog_save_index]
	jmp .save_next

.save_failed:
	mov byte [bootlog_result], BOOTLOG_RESULT_WRITE_FAILED
	jmp .arm

.saved:
	mov byte [bootlog_result], BOOTLOG_RESULT_SAVED
	movzx eax, byte [bootlog_save_slot]
	mov [bootlog_journal_buffer + BOOTLOG_J_LAST_SLOT], al
	inc eax
	cmp eax, BOOTLOG_DISK_SLOT_COUNT
	jb .next_slot_ready
	xor eax, eax
.next_slot_ready:
	mov [bootlog_journal_buffer + BOOTLOG_J_NEXT_SLOT], eax

.arm:
	; A fresh header also clears the previous magic, so a log is saved once.
	call enter_unreal16
	mov esi, BOOTLOG_PHYSICAL_ADDRESS
	xor eax, eax
	mov ecx, BOOTLOG_HEADER_BYTES / 4
.zero_header:
	mov [fs:esi + ecx * 4 - 4], eax
	dec ecx
	jnz .zero_header
	mov dword [fs:esi + BOOTLOG_H_MAGIC], BOOTLOG_MAGIC_LO
	mov dword [fs:esi + BOOTLOG_H_MAGIC + 4], BOOTLOG_MAGIC_HI
	mov dword [fs:esi + BOOTLOG_H_VERSION], BOOTLOG_VERSION
	mov dword [fs:esi + BOOTLOG_H_HEADER_BYTES], BOOTLOG_HEADER_BYTES
	mov dword [fs:esi + BOOTLOG_H_CAPACITY], BOOTLOG_CAPACITY_BYTES
	mov eax, [bootlog_journal_buffer + BOOTLOG_J_BOOT_COUNT]
	mov [fs:esi + BOOTLOG_H_BOOT_SEQUENCE], eax
	mov byte [bootlog_enabled], 1

.record:
	mov al, [bootlog_result]
	mov [bootlog_journal_buffer + BOOTLOG_J_LAST_RESULT], al
	mov eax, [bootlog_journal_buffer + BOOTLOG_J_BOOT_COUNT]
	mov bx, ax
	and bx, BOOTLOG_J_HISTORY_ENTRIES - 1
	shl bx, 3
	mov [bootlog_journal_buffer + BOOTLOG_J_HISTORY + bx], eax
	mov al, [bootlog_result]
	mov [bootlog_journal_buffer + BOOTLOG_J_HISTORY + 4 + bx], al
	mov al, [bootlog_save_slot]
	mov [bootlog_journal_buffer + BOOTLOG_J_HISTORY + 5 + bx], al
	mov ax, [bootlog_save_sectors]
	mov [bootlog_journal_buffer + BOOTLOG_J_HISTORY + 6 + bx], ax

	mov si, bootlog_journal_buffer
	mov di, load_range_bounce_buffer
	call bootlog_copy_sector16
	mov ax, BOOTLOG_DISK_JOURNAL_LBA
	mov bl, 0x03
	mov dl, [boot_device]
	call disk_chs_bounce_sector
	jnc .report
	mov byte [bootlog_journal_failed], 1

.report:
	mov si, str_bootlog_boot
	call print16
	mov ax, [bootlog_journal_buffer + BOOTLOG_J_BOOT_COUNT]
	call print16_whex
	mov si, str_bootlog_result
	call print16
	xor ax, ax
	mov al, [bootlog_result]
	call print16_whex
	mov si, str_bootlog_armed
	cmp byte [bootlog_enabled], 0
	jne .print_state
	mov si, str_bootlog_disabled
.print_state:
	call print16
	cmp byte [bootlog_journal_failed], 0
	je .report_result
	mov si, str_bootlog_journal_failed
	call print16

.report_result:
	cmp byte [bootlog_result], BOOTLOG_RESULT_SAVED
	je .report_saved
	cmp byte [bootlog_result], BOOTLOG_RESULT_WRITE_FAILED
	jne .done
	mov si, str_bootlog_write_failed
	call print16
	jmp .wait_key
.report_saved:
	mov si, str_bootlog_saved_slot
	call print16
	xor ax, ax
	mov al, [bootlog_save_slot]
	call print16_whex
	mov si, str_bootlog_saved_seq
	call print16
	mov ax, [bootlog_journal_buffer + BOOTLOG_J_LAST_SEQUENCE]
	call print16_whex
	mov si, str_bootlog_saved_sectors
	call print16
	mov ax, [bootlog_save_sectors]
	call print16_whex
	mov si, crlf
	call print16
.wait_key:
	; Stop here so a crash -> reset -> crash cycle cannot loop unattended.
	mov si, str_bootlog_press_key
	call print16
	xor ax, ax
	int 0x16
.done:
	pop es
	popad
	ret

align 8
unreal_gdt:
	dq 0x0000000000000000
	dq 0x00CF92000000FFFF
unreal_gdt_pointer:
	dw 15
	dd unreal_gdt

str_bootlog_boot			db "[bootlog] boot=0x", 0
str_bootlog_result			db " result=0x", 0
str_bootlog_armed			db " armed", 13, 10, 0
str_bootlog_disabled		db " disabled (RAM window not usable)", 13, 10, 0
str_bootlog_journal_failed	db "[bootlog] WARNING: journal sector I/O failed", 13, 10, 0
str_bootlog_write_failed	db "[bootlog] WARNING: saving the previous log to disk failed", 13, 10, 0
str_bootlog_saved_slot		db "[bootlog] previous boot log saved: slot=0x", 0
str_bootlog_saved_seq		db " seq=0x", 0
str_bootlog_saved_sectors	db " sectors=0x", 0
str_bootlog_press_key		db "[bootlog] press any key to boot again, or power off and collect the stick", 13, 10, 0
