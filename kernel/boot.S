/*******************************************************************************

    Copyright (C) 2014  René 'Shirk' Köcher
 
    This file is part of Hotarubi.

    Hotarubi is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
 
    Hotarubi is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
 
    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*******************************************************************************/

#include <hotarubi/boot/bootmem.h>
#include <hotarubi/boot/multiboot.h>

.intel_syntax noprefix

/******************************************************************************/
.section .multiboot
/******************************************************************************/

#define MULTIBOOT_FLAGS  ( MULTIBOOT_PAGE_ALIGN | MULTIBOOT_MEMORY_INFO )
#define MULTIBOOT_CHKSUM -( MULTIBOOT_HEADER_MAGIC + MULTIBOOT_FLAGS )
.globl multiboot
.type multiboot, @object
.align 4
multiboot:
	.long MULTIBOOT_HEADER_MAGIC
	.long MULTIBOOT_FLAGS
	.long MULTIBOOT_CHKSUM

/******************************************************************************/
.section .text
/******************************************************************************/
.extern kernel_entry, kernel_ap_entry __bss, __ebss
.extern BOOT_PML4, BOOT_PDPT_1, BOOT_PDPT_2, BOOT_PDT

.code32
.globl kickstart
.type kickstart, @function
kickstart:
	cli
	lea edi, _MBD
	mov [edi + 0], eax
	mov [edi + 4], ebx

	/* quick plausibility check */
	mov eax, 0x80000000
	cpuid
	cmp eax, 0x80000000
	jbe L_failed_64bit_capable
	mov eax, 0x80000001
	cpuid
	bt  edx, 29
	jnc L_failed_64bit_capable

	/* good to go */
	lgdt [_GDTR]
	jmp 0x08:L_flush_gdt_32
	hlt

L_failed_64bit_capable:
	mov ebx, 0xb8000
	mov word ptr[ebx], 0x1f4c
	hlt
1:
	jmp 1b

L_flush_gdt_32:
	mov ax, 0x10
	mov ds, ax
	mov gs, ax
	mov ss, ax

L_enable_paging:
	lea eax, _BOOT_PML4
	mov cr3, eax

	mov eax, cr4
	or  eax, 1 << 5
	mov cr4, eax

	mov ecx, 0xc0000080
	rdmsr
	or  eax, 1 << 8
	wrmsr

	mov eax, cr0
	or  eax, 1 << 31 /* enable PE */
	or  eax, 1 << 16 /* enable WP and don't count on BIOS / EFI for that */
	mov cr0, eax
	jmp 0x08:L_paging_enabled
	hlt

L_paging_enabled:
	/* check if EFER.LMA is active too */
	mov ecx, 0xc0000080
	rdmsr
	bt  eax, 10
	jnc L_failed_64bit_enable

	lgdt [_GDTR]
	jmp 0x18:L_long_mode_enabled
	hlt

L_failed_64bit_enable:
	mov ebx, 0xb8000
	mov word ptr[ebx], 0x1fac
1:
	hlt
	jmp 1b

.code64
L_long_mode_enabled:
	mov ax, 0x20
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax

	cmp dword ptr[_BSP], 1
	jne L_jump_to_64

	/* memory at KERNEL_VMA is now accessible - clean out BSS */
L_clear_bss:
	lea rdi, __bss
	lea rcx, __ebss
	sub rcx, rdi
	shr rcx, 3
	xor rax, rax
	rep stosq

L_jump_to_64:
	/* FIXME: APs will use this stack too? */
	lea rsp, __boot_stack_top 
	mov edi, dword ptr[_MBD + 0]
	mov esi, dword ptr[_MBD + 4]

	cmp dword ptr[_BSP], 1
	jne L_use_ap_entry

	/* BSP passed through here - zero the flag */
	mov dword ptr[_BSP], 0

	lea rax, kernel_entry
	call rax

L_use_ap_entry:
	lea rax, kernel_ap_entry
	call rax

1:
	hlt
	jmp 1b
/******************************************************************************/
.section .data
/******************************************************************************/
.globl _BSP
.type _BSP, @object
_BSP:
	.long 1
_MBD:
	.quad 0x0000000000000000
_GDT:
	.quad 0x0000000000000000
	.quad 0x00cf9a000000ffff
	.quad 0x00cf92000000ffff
	.quad 0x00a09a0000000000
	.quad 0x00a0920000000000
_GDTR:
	.word (_GDTR - _GDT) - 1
	.quad _GDT

.align 4096
_BOOT_PDT:
	addr = 0x00000000
	.rept 512
		.quad ( addr | 0x83 )      /* addr | read,write,2MB */
		addr = ( addr + 0x200000 ) /* on to the next 2MB    */
	.endr

.align 4096
_BOOT_PML4:
	.quad (_BOOT_PDPT_1 + 3)       /* PDPT mapping 1GB 0...1GB */
	.rept 510
		.quad 0
	.endr
	.quad (_BOOT_PDPT_2 + 3)       /* PDPT mapping 1GB to KERNEL_VMA */

.align 4096
_BOOT_PDPT_1:
	.quad (_BOOT_PDT + 3)
	.rept 511
		.quad 0
	.endr

.align 4096
_BOOT_PDPT_2:
	.rept 510
		.quad 0
	.endr
	.quad (_BOOT_PDT + 3)
	.quad 0
