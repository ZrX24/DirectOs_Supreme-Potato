BITS 16
ORG 0x7C00

start:
	cli
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov sp, 0x7C00
	sti
	mov [boot_drive], dl
	xor eax, eax
	mov [0x500], eax
	mov [0x504], eax
	xor ebx, ebx
.memory_map:
	mov eax, 0xE820
	mov edx, 0x534D4150
	mov ecx, 24
	mov di, 0x0510
	int 0x15
	jc .memory_done
	cmp eax, 0x534D4150
	jne .memory_done
	cmp dword [0x520], 1
	jne .memory_next
	mov eax, [0x518]
	add [0x500], eax
	mov eax, [0x51C]
	adc [0x504], eax
.memory_next:
	test ebx, ebx
	jnz .memory_map
.memory_done:

	mov si, message
	call print_string
	mov si, disk_packet
	mov dl, [boot_drive]
	mov ah, 0x42
	int 0x13
	jnc load_kernel
	call read_chs
	jc disk_error

load_kernel:
	cli
	lgdt [gdt_descriptor]
	mov eax, cr0
	or eax, 1
	mov cr0, eax
	jmp 0x08:protected_mode

print_string:
	lodsb
	test al, al
	jz .done
	mov ah, 0x0E
	int 0x10
	jmp print_string
.done:
	ret

read_chs:
	mov dl, [boot_drive]
	mov ah, 0x08
	int 0x13
	jc .failed
	and cl, 0x3F
	jz .failed
	mov [sectors_per_track], cl
	inc dh
	mov [head_count], dh
	mov byte [chs_cylinder], 0
	mov byte [chs_head], 0
	mov byte [chs_sector], 2
	mov word [sectors_left], 127
	mov ax, 0x1000
	mov es, ax
	xor bx, bx
.read:
	mov ah, 0x02
	mov al, 1
	mov ch, [chs_cylinder]
	mov cl, [chs_sector]
	mov dh, [chs_head]
	mov dl, [boot_drive]
	int 0x13
	jc .failed
	add bx, 512
	inc byte [chs_sector]
	mov al, [sectors_per_track]
	cmp [chs_sector], al
	jbe .next
	mov byte [chs_sector], 1
	inc byte [chs_head]
	mov al, [head_count]
	cmp [chs_head], al
	jb .next
	mov byte [chs_head], 0
	inc byte [chs_cylinder]
.next:
	dec word [sectors_left]
	jnz .read
	clc
	ret
.failed:
	stc
	ret
	mov dword [0x500], 0
	mov dword [0x504], 0
	xor ebx, ebx
.memory_map:
	mov eax, 0xE820
	mov edx, 0x534D4150
	mov ecx, 24
	mov di, 0x0510
	int 0x15
	jc .memory_done
	cmp eax, 0x534D4150
	jne .memory_done
	cmp ecx, 20
	jb .memory_done
	cmp dword [0x520], 1
	jne .memory_next
	mov eax, [0x518]
	add [0x500], eax
	mov eax, [0x51C]
	adc [0x504], eax
.memory_next:
	test ebx, ebx
	jnz .memory_map
.memory_done:

disk_error:
	mov si, error_message
	call print_string
.halt:
	cli
	hlt
	jmp .halt

BITS 32
protected_mode:
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax
	mov esp, 0x90000
	jmp 0x08:0x10000

BITS 16
boot_drive: db 0
sectors_per_track: db 0
head_count: db 0
chs_cylinder: db 0
chs_head: db 0
chs_sector: db 0
sectors_left: dw 0
message: db 'DirectOS loading...', 13, 10, 0
error_message: db 'Disk read error', 13, 10, 0

align 4
disk_packet:
	db 0x10, 0
	dw 127
	dw 0x0000, 0x1000
	dq 1

gdt_start:
	dq 0
	dw 0xFFFF, 0x0000
	db 0x00, 0x9A, 0xCF, 0x00
	dw 0xFFFF, 0x0000
	db 0x00, 0x92, 0xCF, 0x00
gdt_end:
gdt_descriptor:
	dw gdt_end - gdt_start - 1
	dd gdt_start

times 510 - ($ - $$) db 0
dw 0xAA55
