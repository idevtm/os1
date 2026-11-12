%include "../kernel/handoff/memory_layout.inc"
%include "image_layout.inc"
%include "initrd_payload.inc"

struc boot_text_console_info_struct
	.columns:		resw 1
	.rows:			resw 1
	.cursor_x:		resw 1
	.cursor_y:		resw 1
endstruc
struc boot_framebuffer_info_struct
	.physical_address:	resq 1
	.width:				resd 1
	.height:			resd 1
	.pitch_bytes:		resd 1
	.bits_per_pixel:	resw 1
	.pixel_format:		resw 1
endstruc
struc boot_memory_region_struct
	.physical_start:	resq 1
	.length:			resq 1
	.type:				resd 1
	.attributes:		resd 1
endstruc
struc boot_module_info_struct
	.physical_start:	resq 1
	.length:			resq 1
	.name:				resq 1
endstruc
struc boot_info_struct
	.magic:					resq 1
	.version:				resd 1
	.source:				resd 1
	.kernel_physical_start:	resq 1
	.kernel_physical_end:	resq 1
	.rsdp_physical:			resq 1
	.smbios_physical:		resq 1
	.command_line:			resq 1
	.bootloader_name:		resq 1
	.text_console:			resb boot_text_console_info_struct_size
	.framebuffer:			resb boot_framebuffer_info_struct_size
	.memory_map:			resq 1
	.memory_map_count:		resd 1
	.reserved0:				resd 1
	.modules:				resq 1
	.module_count:			resd 1
	.reserved1:				resd 1
endstruc

section .bss start=BOOT_INFO_ADDRESS
boot_info resb boot_info_struct_size

e_entry		resq	1
e_phoff		resq	1
e_shoff		resq	1
e_phentsize	resw	1
e_phnum		resw	1
e_shentsize	resw	1
e_shnum		resw	1

p_offset	resq	1
p_vaddr		resq	1
p_paddr		resq	1
p_filesz	resq	1
p_memsz		resq	1
kernel_load_start	resq	1
kernel_load_end		resq	1

section .bss.boot_memory_map start=BOOT_MEMORY_REGION_BUFFER_ADDRESS
boot_memory_map: times boot_memory_region_struct_size * (BOOT_MEMORY_REGION_CAPACITY + 1) db 0

section .bss.boot_modules start=BOOT_MODULE_INFO_BUFFER_ADDRESS
boot_modules: times boot_module_info_struct_size * 2 db 0

memory_pages	equ		EARLY_LONG_MODE_PAGE_TABLES_ADDRESS
kernel_image	equ 	KERNEL_IMAGE_LOAD_ADDRESS
initrd_image equ INITRD_LOAD_ADDRESS

section .text
[org LOADER16_LOAD_ADDRESS]
[bits 16]
	dw	0x7733
loader_main16:
	cli
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov sp, LOADER16_LOAD_ADDRESS
	mov [boot_device], dl

	; Stage 0 only fetched the first sector of kernel16.bin. Finish loading the
	; rest of this BIOS loader from a location outside the 0x7c00 boot page.
	mov ah, 0x08
	mov dl, [boot_device]
	int 0x13
	jc loader_bootstrap_stop
	and cx, 0x003F
	mov [loader_sectors_per_track], cx
	xor ax, ax
	mov al, dh
	inc ax
	mov [loader_head_count], ax

	mov word [loader_current_lba], LOADER16_IMAGE_START_LBA + 1
	mov bx, LOADER16_LOAD_ADDRESS + 512
	mov si, LOADER16_IMAGE_SECTOR_COUNT - 1
.bootstrap_load_next:
	or si, si
	jz .bootstrap_loaded

	mov ax, [loader_current_lba]
	xor dx, dx
	div word [loader_sectors_per_track]
	mov di, dx
	inc di

	xor dx, dx
	div word [loader_head_count]

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
	jc loader_bootstrap_stop

	add bx, 512
	inc word [loader_current_lba]
	dec si
	jmp .bootstrap_load_next

.bootstrap_loaded:
	mov dl, [boot_device]
	jmp loader_full_main16

loader_bootstrap_stop:
	hlt
	jmp loader_bootstrap_stop

boot_device				db 0x00
loader_sectors_per_track	dw 0
loader_head_count			dw 0
loader_current_lba			dw 0

loader_full_main16:
	mov [boot_device], dl
	mov si, str_loader16_hello
	call print16
	
	; enable A20
	call enable_a20_fast
	
	; check A20
	call check_a20
	or ax, ax
	jz .a20disabled
	mov si, str_a20
	call print16
	mov si, str_on
	call print16
	jmp .a20_next
.a20disabled:
	mov si, str_a20
	call print16
	mov si, str_off
	call print16
	jmp $ ; // can't work with this
.a20_next:
	mov si, crlf
	call print16

	; check long mode
	call check_long_mode
	jc no_long_mode

	; Load both fixed raw-image payloads through the CHS helper. Stage 0 already
	; proved that this BIOS can read the USB image through classic INT 13h CHS,
	; while some real machines fail the packet-read path used by the old helper.
	mov byte [disk_load_phase], 1
	mov edi, kernel_image
	mov ax, KERNEL_IMAGE_START_LBA
	mov dl, [boot_device]
	mov cx, KERNEL_IMAGE_SECTOR_COUNT
	call disk_read_chs_range
	jc .disk_error

	mov byte [disk_load_phase], 2
	mov edi, initrd_image
	mov ax, INITRD_IMAGE_START_LBA
	mov dl, [boot_device]
	mov cx, INITRD_PAYLOAD_SECTOR_COUNT
	call disk_read_chs_range
	jc .disk_error

	; success
	mov eax, kernel_image >> 4
	mov es, ax
	mov eax, [es:0]
	cmp eax, 0x464C457F
	jne .elf_error
	jmp .next
.disk_error:
	mov si, str_elf_fail_disk
	call print16
	call print_loader_disk_debug
	jmp .stop
.elf_error:
	mov si, str_elf_fail_check
	call print16
.stop:
	hlt
	jmp .stop
.next:	
	xor ax, ax
	mov es, ax

	; Zero the published handoff block first so BIOS-only boot leaves future
	; fields in a defined state until later boot paths populate them.
	mov di, boot_info
	mov cx, boot_info_struct_size / 2
	cld
	rep stosw
	mov dword [boot_info + boot_info_struct.version], BOOT_INFO_VERSION
	mov dword [boot_info + boot_info_struct.source], BOOT_SOURCE_BIOS_LEGACY
	mov word [boot_info + boot_info_struct.text_console + boot_text_console_info_struct.columns], BOOT_TEXT_COLUMNS
	mov word [boot_info + boot_info_struct.text_console + boot_text_console_info_struct.rows], BOOT_TEXT_ROWS

	call save_text_cursor16

	; detect memory
.l2:
	mov di, boot_memory_map
	call do_e820
	jnc .l3
	mov si, str_e820_failed
	call print16
	hlt
	jmp $			; can't detect memory. die here
.l3:
    cmp byte [do_e820.truncated], 0
    je .map_ready
    push bp
    mov si, str_e820_truncated
    call print16
    pop bp
    mov eax, KERNEL_RESERVED_PHYSICAL_START
    mov edx, KERNEL_RESERVED_PHYSICAL_END + PAGE_FRAME_BITMAP_SIZE_BYTES
    call e820_usable32
    jc .map_invalid
    mov eax, EARLY_LONG_MODE_PAGE_TABLES_ADDRESS
    mov edx, EARLY_LONG_MODE_PAGE_TABLES_ADDRESS + EARLY_PAGE_TABLE_BYTES
    call e820_usable32
    jnc .map_ready
.map_invalid:
    mov si, str_e820_destinations
    call print16
    cli
    hlt
    jmp $
.map_ready:
	; e820 success
	xor eax, eax
	mov ax, bp
	mov dword [boot_info + boot_info_struct.memory_map_count], eax
	mov eax, boot_memory_map
	mov dword [boot_info + boot_info_struct.memory_map], eax
	xor eax, eax
	mov dword [boot_info + boot_info_struct.memory_map + 4], eax

	; Save the previous boot's RAM log to disk and arm a fresh one. This must
	; run after E820 (it validates the RAM window) and while the BIOS is usable.
%if BOOT_DIAGNOSTICS_ENABLED
	call bootlog_prepare16
	call save_text_cursor16
%endif

	; TODO: detect video modes

	; switch to long mode
	mov edi, memory_pages
	jmp switch_to_long_mode
	
no_long_mode:
	mov si, str_no_long_mode
	call print16
	jmp $

; Publish the BIOS cursor position for the long-mode console and kernel.
save_text_cursor16:
	mov ah, 03h
	xor bh, bh
	int 10h
	xor ax, ax
	mov al, dl
	mov word [boot_info + boot_info_struct.text_console + boot_text_console_info_struct.cursor_x], ax
	xor ax, ax
	mov al, dh
	mov word [boot_info + boot_info_struct.text_console + boot_text_console_info_struct.cursor_y], ax
	ret

print_loader_disk_debug:
	cmp byte [disk_load_phase], 2
	je .phase_initrd
	mov si, str_disk_phase_kernel
	call print16
	jmp .phase_done
.phase_initrd:
	mov si, str_disk_phase_initrd
	call print16
.phase_done:

	mov si, str_disk_status
	call print16
	xor ax, ax
	mov al, [load_range_error_status]
	call print16_whex

	mov si, str_disk_retry
	call print16
	xor ax, ax
	mov al, [load_range_error_attempt]
	call print16_whex

	mov si, str_disk_lba
	call print16
	mov ax, [load_range_error_lba]
	call print16_whex

	mov si, str_disk_cylinder
	call print16
	mov ax, [load_range_error_cylinder]
	call print16_whex

	mov si, str_disk_head
	call print16
	xor ax, ax
	mov al, [load_range_error_head]
	call print16_whex

	mov si, str_disk_sector
	call print16
	xor ax, ax
	mov al, [load_range_error_sector]
	call print16_whex

	mov si, str_disk_drive
	call print16
	xor ax, ax
	mov al, [load_range_drive]
	call print16_whex

	mov si, str_disk_spt
	call print16
	mov ax, [loader_sectors_per_track]
	call print16_whex

	mov si, str_disk_heads
	call print16
	mov ax, [loader_head_count]
	call print16_whex

	mov si, crlf
	call print16
	ret

crlf		   	db 13, 10, 0
newline			db 10, 0
space		   	db " ", 0
str_loader16_hello   	db "[loader16] hello", 13, 10, 0
str_no_long_mode   		db "[loader16] 64bit mode not available", 13, 10, 0
str_e820_destinations   db "[loader16] Retained memory map does not cover bootstrap RAM", 13, 10, 0
str_e820_truncated      db "[loader16] Memory map capacity reached; extra ranges omitted", 13, 10, 0
str_e820_failed			db "[loader16] Memory detection failed", 13, 10, 0
str_elf_fail_disk    	db "[loader16] ELF load disk error", 13, 10, 0
str_elf_fail_check    	db "[loader16] ELF check failed", 13, 10, 0
str_disk_phase_kernel		db "[loader16] slot kernel", 13, 10, 0
str_disk_phase_initrd		db "[loader16] slot initrd", 13, 10, 0
str_disk_status			db "[loader16] bios ah=0x", 0
str_disk_retry			db " retry=0x", 0
str_disk_lba			db " lba=0x", 0
str_disk_cylinder		db " cyl=0x", 0
str_disk_head			db " head=0x", 0
str_disk_sector			db " sec=0x", 0
str_disk_drive			db " drive=0x", 0
str_disk_spt			db " spt=0x", 0
str_disk_heads			db " heads=0x", 0
str_a20					db "[loader16] A20 ", 0
str_on					db "on", 0
str_off					db "off", 0
str_bootloader_name		db "bios-loader", 0
str_initrd_name			db "initrd.cpio", 0
str_bootlog_name		db "bootlog", 0
%if BOOT_DIAGNOSTICS_ENABLED
str_boot_diagnostics_command_line db "os1.bootdiag", 0
%endif
disk_load_phase			db 0

%include "disk16_range.asm"
%include "bootlog16.asm"
%include "biosmemory.asm"
%include "console16.asm"
%include "long64.asm"
%include "console64.asm"
%include "bootlog64.asm"

[bits 64]
loader_main64:
	mov rsi, str_loader_hello
	call print64
	mov rsi, str_loader_stage_elf
	call print64

	;; expand the kernel ELF at [kernel_image] (quick and dirty = no checks)
	;; EH
	; e_entry
	mov rsi, 0x18
	mov rax, [kernel_image + rsi]
	mov [e_entry], rax
	; e_phoff
	mov rsi, 0x20
	mov rax, [kernel_image + rsi]
	mov [e_phoff], rax
	; e_shoff
	mov rsi, 0x28
	mov rax, [kernel_image + rsi]
	mov [e_shoff], rax
	; e_phentsize
	mov rsi, 0x36
	mov ax, [kernel_image + rsi]
	mov [e_phentsize], ax
	; e_phnum
	mov rsi, 0x38
	mov ax, [kernel_image + rsi]
	mov [e_phnum], ax
	; e_shentsize
	mov rsi, 0x3A
	mov ax, [kernel_image + rsi]
	mov [e_shentsize], ax
	; e_shnum
	mov rsi, 0x3C
	mov ax, [kernel_image + rsi]
	mov [e_shnum], ax

	;; PH	
	mov rax, -1
	mov [kernel_load_start], rax
	xor eax, eax
	mov [kernel_load_end], rax
	xor ebx, ebx

.load_next_segment:
	movzx eax, word [e_phnum]
	cmp ebx, eax
	jae .segments_loaded

	movzx rax, word [e_phentsize]
	imul rax, rbx
	add rax, [e_phoff]
	mov rdx, rax

	cmp dword [kernel_image + rdx], 1
	jne .next_segment

	mov rax, [kernel_image + rdx + 0x08]
	mov [p_offset], rax
	mov rax, [kernel_image + rdx + 0x10]
	mov [p_vaddr], rax
	mov rax, [kernel_image + rdx + 0x18]
	mov [p_paddr], rax
	mov rax, [kernel_image + rdx + 0x20]
	mov [p_filesz], rax
	mov rax, [kernel_image + rdx + 0x28]
	mov [p_memsz], rax

	mov rax, [p_memsz]
	cmp rax, [p_filesz]
	jb .elf_program_error

	mov rax, [p_offset]
	add rax, [p_filesz]
	cmp rax, (INITRD_LOAD_ADDRESS - KERNEL_IMAGE_LOAD_ADDRESS)
	ja .elf_program_error

	mov rax, [p_paddr]
	cmp rax, KERNEL_RESERVED_PHYSICAL_START
	jb .elf_program_error
	mov rcx, rax
	add rcx, [p_memsz]
	jc .elf_program_error
	cmp rcx, KERNEL_RESERVED_PHYSICAL_END - KERNEL_POST_IMAGE_RESERVE_BYTES
	ja .elf_program_error
	cmp rax, [kernel_load_start]
	jae .load_start_ok
	mov [kernel_load_start], rax
.load_start_ok:
	cmp rcx, [kernel_load_end]
	jbe .load_end_ok
	mov [kernel_load_end], rcx
.load_end_ok:

	mov rdi, [p_paddr]
	xor eax, eax
	mov rcx, [p_memsz]
	cld
	rep stosb

	mov rsi, [p_offset]
	add rsi, kernel_image
	mov rdi, [p_paddr]
	mov rcx, [p_filesz]
	cld
	rep movsb

.next_segment:
	inc ebx
	jmp .load_next_segment

.segments_loaded:
	mov rax, [kernel_load_start]
	cmp rax, -1
	je .elf_program_error
	mov rsi, str_loader_stage_segments
	call print64

	; Publish the immutable pieces of the BIOS boot contract right before the
	; kernel takes over. The kernel copies this block immediately on entry.
	mov rax, BOOT_INFO_MAGIC
	mov [boot_info + boot_info_struct.magic], rax
	mov rax, str_bootloader_name
	mov [boot_info + boot_info_struct.bootloader_name], rax
%if BOOT_DIAGNOSTICS_ENABLED
	mov rax, str_boot_diagnostics_command_line
	mov [boot_info + boot_info_struct.command_line], rax
%endif
	mov rsi, str_loader_stage_rsdp
	call print64
	call find_rsdp64
	mov [boot_info + boot_info_struct.rsdp_physical], rax
	mov rax, boot_memory_map
	mov [boot_info + boot_info_struct.memory_map], rax
	mov rax, [kernel_load_start]
	mov [boot_info + boot_info_struct.kernel_physical_start], rax
	mov rax, [kernel_load_end]
	mov [boot_info + boot_info_struct.kernel_physical_end], rax
	mov rax, boot_modules
	mov [boot_info + boot_info_struct.modules], rax
	mov dword [boot_info + boot_info_struct.module_count], 1
	mov qword [boot_modules + boot_module_info_struct.physical_start], initrd_image
	mov qword [boot_modules + boot_module_info_struct.length], INITRD_PAYLOAD_LENGTH_BYTES
	mov rax, str_initrd_name
	mov [boot_modules + boot_module_info_struct.name], rax
	cmp byte [bootlog_enabled], 0
	je .bootlog_module_done
	mov dword [boot_info + boot_info_struct.module_count], 2
	mov qword [boot_modules + boot_module_info_struct_size + boot_module_info_struct.physical_start], BOOTLOG_PHYSICAL_ADDRESS
	mov qword [boot_modules + boot_module_info_struct_size + boot_module_info_struct.length], BOOTLOG_REGION_BYTES
	mov rax, str_bootlog_name
	mov [boot_modules + boot_module_info_struct_size + boot_module_info_struct.name], rax
.bootlog_module_done:
	mov rsi, str_loader_stage_kernel
	call print64
    ; init program stack
	mov rbp, [kernel_load_end]
	add rbp, PAGE_SIZE
	shr rbp, 12
	shl rbp, 12
	mov rsi, rbp			; base of BSP cpu record: param to kernel main
	add rbp, PAGE_SIZE
	lea rsp, [rbp + PAGE_SIZE]	; give the kernel a dedicated 4k bootstrap stack

	; jump
	mov rax, [e_entry]
	mov rdi, boot_info
	call rax		; call kernel_main

	; kernel_main only returns when bring-up failed. With the boot log armed,
	; reset so the next loader run saves what the kernel logged.
	cmp byte [bootlog_enabled], 0
	je .l3
	call bootlog_reset64
.l3:		
	hlt
        jmp .l3 ; stop here

.elf_program_error:
	mov rsi, str_elf_fail_program
	call print64
	hlt
	jmp .elf_program_error

; Data
str_loader_hello	db "[loader64] hello", 10, 0
str_loader_stage_elf db "[loader64] load elf", 10, 0
str_loader_stage_segments db "[loader64] segments ready", 10, 0
str_loader_stage_rsdp db "[loader64] scan rsdp", 10, 0
str_loader_stage_kernel db "[loader64] jump kernel", 10, 0
str_elf_fail_program db "[loader64] ELF program header error", 10, 0
hexdigit			db "0123456789ABCDEF",0

find_rsdp64:
	push rbx
	push rcx
	push rdx
	push rsi
	push rdi

	xor eax, eax
	movzx ebx, word [0x40E]
	shl rbx, 4
	test rbx, rbx
	jz .search_base_memory

	mov rdi, rbx
	mov ecx, 1024
	call scan_rsdp_range64
	test rax, rax
	jnz .done

.search_base_memory:
	movzx ebx, word [0x413]
	shl rbx, 10
	cmp rbx, 1024
	jb .search_bios_rom
	sub rbx, 1024
	mov rdi, rbx
	mov ecx, 1024
	call scan_rsdp_range64
	test rax, rax
	jnz .done

.search_bios_rom:
	mov rdi, 0xE0000
	mov ecx, 0x20000
	call scan_rsdp_range64

.done:
	pop rdi
	pop rsi
	pop rdx
	pop rcx
	pop rbx
	ret

scan_rsdp_range64:
	lea rsi, [rdi + rcx]
.loop:
	cmp rdi, rsi
	jae .not_found

	cmp byte [rdi + 0], 'R'
	jne .next
	cmp byte [rdi + 1], 'S'
	jne .next
	cmp byte [rdi + 2], 'D'
	jne .next
	cmp byte [rdi + 3], ' '
	jne .next
	cmp byte [rdi + 4], 'P'
	jne .next
	cmp byte [rdi + 5], 'T'
	jne .next
	cmp byte [rdi + 6], 'R'
	jne .next
	cmp byte [rdi + 7], ' '
	jne .next

	push rsi
	call validate_rsdp64
	pop rsi
	test al, al
	jnz .found

.next:
	add rdi, 16
	jmp .loop

.found:
	mov rax, rdi
	ret

.not_found:
	xor eax, eax
	ret

validate_rsdp64:
	push rcx
	push rdx
	push rsi

	mov ecx, 20
	xor edx, edx
	xor esi, esi
.sum20:
	add dl, [rdi + rsi]
	inc rsi
	loop .sum20
	test dl, dl
	jne .invalid

	mov al, [rdi + 15]
	cmp al, 2
	jb .valid

	mov ecx, [rdi + 20]
	cmp ecx, 36
	jb .invalid
	xor edx, edx
	xor esi, esi
.sumext:
	add dl, [rdi + rsi]
	inc rsi
	loop .sumext
	test dl, dl
	jne .invalid

.valid:
	mov al, 1
	pop rsi
	pop rdx
	pop rcx
	ret

.invalid:
	xor eax, eax
	pop rsi
	pop rdx
	pop rcx
	ret

; Tail
; The loader text may grow up to the E820 buffer at BOOT_MEMORY_REGION_BUFFER_ADDRESS.
times (BOOT_MEMORY_REGION_BUFFER_ADDRESS - LOADER16_LOAD_ADDRESS)-($-$$) db 0xCC                ; Fill sectors
