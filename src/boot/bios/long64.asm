[bits 16]
%define EARLY_PAGE_TABLE_BYTES (5 * PAGE_SIZE)
%if EARLY_LONG_MODE_PAGE_TABLES_ADDRESS < (LOADER16_LOAD_ADDRESS + LOADER16_IMAGE_SECTOR_COUNT * 512)
    %error "Early page tables overlap the BIOS loader"
%endif
%if KERNEL_IMAGE_LOAD_ADDRESS < (EARLY_LONG_MODE_PAGE_TABLES_ADDRESS + EARLY_PAGE_TABLE_BYTES)
    %error "BIOS kernel staging overlaps early page tables"
%endif
; Checks whether CPU supports long mode or not.
; Returns with carry set if CPU doesn't support long mode.
check_long_mode:
    ; Check whether CPUID is supported or not.
    pushfd                            ; Get flags in EAX register.
 
    pop eax
    mov ecx, eax  
    xor eax, 0x200000 
    push eax 
    popfd
 
    pushfd 
    pop eax
    xor eax, ecx
    shr eax, 21 
    and eax, 1                        ; Check whether bit 21 is set or not. If EAX now contains 0, CPUID isn't supported.
    push ecx
    popfd 
 
    test eax, eax
    jz .no_long_mode
 
    mov eax, 0x80000000   
    cpuid                 
 
    cmp eax, 0x80000001               ; Check whether extended function 0x80000001 is available or not.
    jb .no_long_mode                  ; If not, long mode not supported.
 
    mov eax, 0x80000001  
    cpuid                 
    test edx, 1 << 29                 ; Test if the LM-bit, is set or not.
    jz .no_long_mode                  ; If not Long mode not supported.
 	
 	clc
    ret
 
.no_long_mode:
    stc
    ret

%define PAGE_PRESENT    (1 << 0)
%define PAGE_WRITE      (1 << 1)
 
%define CODE_SEG     0x0008
%define DATA_SEG     0x0010
 
ALIGN 4
idt:
    .Length       dw 0
    .Base         dd 0
 
; Function to switch directly to long mode from real mode.
; Identity maps the first 4MiB and aliases that window at the higher-half
; kernel base so the shared kernel can enter at a high virtual address and
; still reach the early page-frame bitmap before it installs its own page
; tables.
; Uses Intel syntax.
 
; es:edi    Should point to a valid page-aligned 20KiB buffer, for the PML4,
;           PDPT, PD, and two PTs.
; ss:esp    Should point to memory that can be used as a small (1 uint32_t) stack
 
switch_to_long_mode:
    ; All BIOS disk and boot-log I/O is complete. Its scratch page can now be reused.
    ; Zero out the 20KiB buffer.
    ; Since we are doing a rep stosd, count should be bytes/4.   
    push di                           ; REP STOSD alters DI.
    mov ecx, EARLY_PAGE_TABLE_BYTES / 4
    xor eax, eax
    cld
    rep stosd
    pop di                            ; Get DI back.
 
 
    ; Build the Page Map Level 4.
    ; es:di points to the Page Map Level 4 table.
    lea eax, [es:di + 0x1000]         ; Put the address of the Page Directory Pointer Table in to EAX.
    or eax, PAGE_PRESENT | PAGE_WRITE ; Or EAX with the flags - present flag, writable flag.
    mov [es:di], eax                  ; Store the value of EAX as the first PML4E.
    mov [es:di + (KERNEL_PML4_INDEX * 8)], eax
 
 
    ; Build the Page Directory Pointer Table.
    lea eax, [es:di + 0x2000]         ; Put the address of the Page Directory in to EAX.
    or eax, PAGE_PRESENT | PAGE_WRITE ; Or EAX with the flags - present flag, writable flag.
    mov [es:di + 0x1000], eax         ; Store the value of EAX as the first PDPTE.
    mov [es:di + 0x1000 + (((KERNEL_VIRTUAL_OFFSET >> 30) & 0x1FF) * 8)], eax
 
 
    ; Build the Page Directory.
    lea eax, [es:di + 0x3000]         ; Put the address of the first Page Table in to EAX.
    or eax, PAGE_PRESENT | PAGE_WRITE ; Or EAX with the flags - present flag, writeable flag.
    mov [es:di + 0x2000], eax         ; Store to value of EAX as the first PDE.
    lea eax, [es:di + 0x4000]         ; Put the address of the second Page Table in to EAX.
    or eax, PAGE_PRESENT | PAGE_WRITE
    mov [es:di + 0x2008], eax
    ; Identity-map the RAM boot log window with one 2 MiB page so the loader
    ; and the early kernel can append to it before the kernel page tables exist.
%if BOOT_DIAGNOSTICS_ENABLED
    mov dword [es:di + 0x2000 + ((BOOTLOG_PHYSICAL_ADDRESS >> 21) * 8)], BOOTLOG_PHYSICAL_ADDRESS | PAGE_PRESENT | PAGE_WRITE | (1 << 7)
%endif
 
 
    push di                           ; Save DI for the time being.
    lea di, [di + 0x3000]             ; Point DI to the page table.
    mov eax, PAGE_PRESENT | PAGE_WRITE    ; Move the flags into EAX - and point it to 0x0000.
 
 
    ; Build the first Page Table.
.LoopPageTable:
    mov [es:di], eax
    add eax, 0x1000
    add di, 8
    cmp eax, 0x200000                 ; If we did the first 2MiB, end.
    jb .LoopPageTable

    ; Build the second Page Table.
.LoopSecondPageTable:
    mov [es:di], eax
    add eax, 0x1000
    add di, 8
    cmp eax, 0x400000                 ; If we did all 4MiB, end.
    jb .LoopSecondPageTable
 
    pop di                            ; Restore DI.
 
    ; Disable IRQs
    mov al, 0xFF                      ; Out 0xFF to 0xA1 and 0x21 to disable all IRQs.
    out 0xA1, al
    out 0x21, al
 
    nop
    nop
 
    lidt [idt]                        ; Load a zero length IDT so that any NMI causes a triple fault.
 
    ; Enter long mode.
    mov eax, 10100000b                ; Set the PAE and PGE bit.
    mov cr4, eax
 
    mov edx, edi                      ; Point CR3 at the PML4.
    mov cr3, edx
 
    mov ecx, 0xC0000080               ; Read from the EFER MSR. 
    rdmsr    
 
    ; Enable long mode and the NX bit so later kernel mappings can safely mark
    ; user stacks and data pages non-executable without triggering RSVD faults.
    or eax, 0x00000900                ; Set the LME and NXE bits.
    wrmsr
 
    mov ebx, cr0                      ; Activate long mode -
    or ebx,0x80000001                 ; - by enabling paging and protection simultaneously.
    mov cr0, ebx                    
 
    ; move GDT to address 0
GDTT equ 0x0
	mov edi, GDTT
    mov esi, gdt
	cld
	mov ecx, 3 * 8 + 8
	rep movsb
	mov eax, GDTT
	mov dword [GDTT + 3 * 8 + 4], eax

	; load GDT
	lgdt [GDTT + 3 * 8 + 2]
    jmp CODE_SEG:long_mode            ; Load CS with 64 bit segment and flush the instruction cache
 
 
    ; Global Descriptor Table
ALIGN 8
gdt:
.Null:
	dq 0x0000000000000000             ; Null Descriptor - should be present.
 
.Code:
	dq 0x00209A0000000000             ; 64-bit code descriptor (exec/read).
	dq 0x0000920000000000             ; 64-bit data descriptor (read/write).
 
ALIGN 4
	dw 0                              ; Padding to make the "address of the GDT" field aligned on a 4-byte boundary
 
.Pointer:
    dw $ - gdt - 1                    ; 16-bit Size (Limit) of GDT.
    dd gdt                            ; 32-bit Base Address of GDT. (CPU will zero extend to 64-bit)

 
[BITS 64]      
long_mode:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
 
	jmp loader_main64

    
