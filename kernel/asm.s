/*
 *  linux/kernel/asm.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * asm.s contains the low-level code for most hardware faults.
 * page_exception is handled by the mm, so that isn't here. This
 * file also handles (hopefully) fpu-exceptions due to TS-bit, as
 * the fpu must be properly saved/resored. This hasn't been tested.
 */

/* 本代码文件主要涉及对Intel 保留的中断 int0--int16 的处理（int17-int31 留作今后使用）。
   下是一些全局函数名的声明，其原形在traps.c 中说明。*/

.globl _divide_error,_debug,_nmi,_int3,_overflow,_bounds,_invalid_op
.globl _double_fault,_coprocessor_segment_overrun
.globl _invalid_TSS,_segment_not_present,_stack_segment
.globl _general_protection,_coprocessor_error,_irq13,_reserved
.globl _alignment_check

/* divide_error() 函数原型在 traps.c 中
   do_divide_error() 函数实现在 traps.c 中 */
_divide_error:
	pushl $_do_divide_error  # 首先将要调用的函数地址入栈
no_error_code:
	xchgl %eax,(%esp)  # _do_divide_error 的地址被交换到 eax 中，eax 中原来的值被交换到堆栈中
	pushl %ebx
	pushl %ecx
	pushl %edx
	pushl %edi
	pushl %esi
	pushl %ebp
	push %ds
	push %es
	push %fs
	pushl $0		# "error code"  这段程序的出错号是 0
	lea 44(%esp),%edx
	pushl %edx
	movl $0x10,%edx  # 内核代码数据段选择符
	mov %dx,%ds
	mov %dx,%es
	mov %dx,%fs
	call *%eax  # *号表示是绝对调用操作数，与程序指针 PC 无关。
				# 调用 C 函数 do_divide_error()
	addl $8,%esp  # 堆栈指针重新指向寄存器 fs 入栈处
	pop %fs
	pop %es
	pop %ds
	popl %ebp
	popl %esi
	popl %edi
	popl %edx
	popl %ecx
	popl %ebx
	popl %eax  # 弹出原来 eax 中的内容
	iret

_debug:
	pushl $_do_int3		# _do_debug
	jmp no_error_code

_nmi:
	pushl $_do_nmi
	jmp no_error_code

_int3:
	pushl $_do_int3
	jmp no_error_code

_overflow:
	pushl $_do_overflow
	jmp no_error_code

_bounds:
	pushl $_do_bounds
	jmp no_error_code

_invalid_op:
	pushl $_do_invalid_op
	jmp no_error_code

_coprocessor_segment_overrun:
	pushl $_do_coprocessor_segment_overrun
	jmp no_error_code

_reserved:
	pushl $_do_reserved
	jmp no_error_code

# int45 -- ( = 0x20 + 13 ) 数学协处理器（Coprocessor）发出的中断。
# 当协处理器执行完一个操作时就会发出IRQ13 中断信号，以通知 CPU 操作完成

_irq13:
	pushl %eax	
	xorb %al,%al		# 80387 在执行计算时，CPU 会等待其操作的完成。 
	outb %al,$0xF0		# 通过写 0xF0 端口，本中断将消除 CPU 的 BUSY 延续信号，并重新
						# 激活 80387 的处理器扩展请求引脚 PEREQ。该操作主要是为了确保
						# 继续执行 80387 的任何指令之前，响应本中断。
	movb $0x20,%al		
	outb %al,$0x20		# 向 8259 主中断控制芯片发送 EOI（中断结束）信号
	jmp 1f				# 两个跳转指令起延时作用
1:	jmp 1f
1:	outb %al,$0xA0		# 再向 8259 从中断控制芯片发送 EOI（中断结束）信号
	popl %eax
	jmp _coprocessor_error

_double_fault:
	pushl $_do_double_fault
error_code:
	xchgl %eax,4(%esp)		# error code <-> %eax
	xchgl %ebx,(%esp)		# &function <-> %ebx
	pushl %ecx
	pushl %edx
	pushl %edi
	pushl %esi
	pushl %ebp
	push %ds
	push %es
	push %fs
	pushl %eax			# error code
	lea 44(%esp),%eax		# offset
	pushl %eax
	movl $0x10,%eax		# 内核数据段选择符
	mov %ax,%ds
	mov %ax,%es
	mov %ax,%fs
	call *%ebx
	addl $8,%esp
	pop %fs
	pop %es
	pop %ds
	popl %ebp
	popl %esi
	popl %edi
	popl %edx
	popl %ecx
	popl %ebx
	popl %eax
	iret

_invalid_TSS:
	pushl $_do_invalid_TSS
	jmp error_code

_segment_not_present:
	pushl $_do_segment_not_present
	jmp error_code

_stack_segment:
	pushl $_do_stack_segment
	jmp error_code

_general_protection:
	pushl $_do_general_protection
	jmp error_code

_alignment_check:
	pushl $_do_alignment_check
	jmp error_code

