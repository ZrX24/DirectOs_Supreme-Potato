BITS 32
section .text.entry
global _start
extern kernel_main
extern __bss_start
extern __bss_end

_start:
	cli
	cld
	mov esp, 0x90000
	mov edi, __bss_start
	mov ecx, __bss_end
	sub ecx, edi
	xor eax, eax
	rep stosb
	call kernel_main
.halt:
	cli
	hlt
	jmp .halt
