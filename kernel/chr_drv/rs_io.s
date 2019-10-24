/*
 *  linux/kernel/rs_io.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *	rs_io.s
 *
 * This module implements the rs232 io interrupts.
 */

.text
.globl _rs1_interrupt,_rs2_interrupt

size	= 1024				/* must be power of two !
					   and must match the value
					   in tty_io.c!!! */

/* these are the offsets into the read/write buffer structures */
rs_addr = 0   // 串行端口号字段偏移（端口号是 0x3f8或 0x2f8）
head = 4
tail = 8
proc_list = 12
buf = 16

startup	= 256		/* chars left in write queue when we restart it */
					// 当写队列里还剩256个字符空间时，我们就可以写

/*
 * These are the actual interrupt routines. They look where
 * the interrupt is coming from, and take appropriate action.
 */
// 先检查中断的来源，然后执行相应的处理 
.align 2
_rs1_interrupt:
	pushl $_table_list+8  // tty 表中对应串口 1的读写缓冲指针的地址入栈 tty_io.c
	jmp rs_int
.align 2
_rs2_interrupt:
	pushl $_table_list+16  // tty 表中对应串口 2 的读写缓冲队列指针的地址入栈
rs_int:
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	push %es
	push %ds		/* as this is an interrupt, we cannot */
	pushl $0x10		/* know that bs is ok. Load it */
	pop %ds
	pushl $0x10
	pop %es
	movl 24(%esp),%edx  // 将缓冲队列指针地址存入 edx 寄存器
	movl (%edx),%edx    // 取读缓冲队列结构指针(地址) -> edx
						// 对于串行终端，data 字段存放着串行端口地址（端口号）
	movl rs_addr(%edx),%edx
	addl $2,%edx		/* interrupt ident. reg */
	                    // edx 指向中断标志寄存器
rep_int:				// 中断标识寄存器端口是 0x3fa 或 0x2fa
	xorl %eax,%eax		// eax 清零
	inb %dx,%al			// 取中断标识字节，用来判断中断来源
	testb $1,%al		// 首先判断有无待处理的中断（位0=1 无中断；=0 有中断）
	jne end
	cmpb $6,%al		/* this shouldn't happen, but ... */
	ja end			// al 值 >6？是则跳转到 end
	movl 24(%esp),%ecx	// 再次取缓冲队列指针地址 -> ecx
	pushl %edx			// 将中断标识寄存器端口号 0x3fa(0x2fa) 入栈
	subl $2,%edx		// 0x3f8(0x2f8)
	call jmp_table(,%eax,2)		/* NOTE! not *4, bit0 is 0 already */
	popl %edx
	jmp rep_int
end:	movb $0x20,%al  // 向中断控制器发送结束中断指令 EOI
	outb %al,$0x20		/* EOI */
	pop %ds
	pop %es
	popl %eax
	popl %ebx
	popl %ecx
	popl %edx
	addl $4,%esp		# jump over _table_list entry
	iret

jmp_table:
	.long modem_status,write_char,read_char,line_status

.align 2
modem_status:
	addl $6,%edx		/* clear intr by reading modem status reg */
	inb %dx,%al			// 通过读 modem 状态寄存器进行复位 0x3fe
	ret  // 返回到 call jmp_table 的下一条指令

.align 2
line_status:
	addl $5,%edx		/* clear intr by reading line status reg. */
	inb %dx,%al			// 通过读取线路状态寄存器进行复位 0x3fd
	ret

.align 2
read_char:
	inb %dx,%al				// 读取字符 -> al
	movl %ecx,%edx			// 当前串口缓冲队列指针地址->edx
	subl $_table_list,%edx  // $_table_list - %edx -> edx
	shrl $3,%edx			// 对于串口1是1，对于串口2是2
	movl (%ecx),%ecx		# read-queue
	movl head(%ecx),%ebx
	movb %al,buf(%ecx,%ebx) // al -> 缓冲区中头指针所指的位置
	incl %ebx		// 头指针前移一字节
	andl $size-1,%ebx  // 用缓冲区大小对头指针进行取模，使头指针不超过缓冲区大小
	cmpl tail(%ecx),%ebx
	je 1f			// 缓冲区已满
	movl %ebx,head(%ecx)  // 保存修改过的头指针
1:	addl $63,%edx
	pushl %edx  // 将串口号压入堆栈(1- 串口 1，2 - 串口 2)，作为参数
	call _do_tty_interrupt  // tty_io.c
	addl $4,%esp
	ret

.align 2
write_char:
	movl 4(%ecx),%ecx		# write-queue // 取写缓冲队列结构地址 -> ecx
	movl head(%ecx),%ebx    // 取写队列头指针 -> ebx
	subl tail(%ecx),%ebx    // 队列中的字符数
	andl $size-1,%ebx		# nr chars in queue
	je write_buffer_empty
	cmpl $startup,%ebx  // 队列中字符数超过 256 个？
	ja 1f  // 超过则跳转
	movl proc_list(%ecx),%ebx	# wake up sleeping process
	testl %ebx,%ebx			# is there any?
	je 1f  // 空（0）则跳转
	movl $0,(%ebx)  // 否则将进程置为可运行状态
1:	movl tail(%ecx),%ebx
	movb buf(%ecx,%ebx),%al  // 从缓冲中尾指针处取一字符 -> al
	outb %al,%dx  // 向端口 0x3f8(0x2f8) 送出到保持寄存器中
	incl %ebx  // 尾指针前移
	andl $size-1,%ebx
	movl %ebx,tail(%ecx)  // 保存修改过的尾指针
	cmpl head(%ecx),%ebx  // 比较头指针和尾指针
	je write_buffer_empty  // 若相等则表示队列已空，则跳转
	ret
.align 2
// 处理写缓冲队列write_q已空的情况。
// 若有等待写该串行终端的进程则唤醒之，然后屏蔽发送
// 保持寄存器空中断，不让发送保持寄存器空时产生中断
write_buffer_empty:
	movl proc_list(%ecx),%ebx	# wake up sleeping process
	testl %ebx,%ebx			# is there any?
	je 1f
	movl $0,(%ebx)
1:	incl %edx  // 指向端口 0x3f9(0x2f9)
	inb %dx,%al  // 读取中断允许寄存器 -> al
	jmp 1f
1:	jmp 1f
1:	andb $0xd,%al		/* disable transmit interrupt */
						// 屏蔽发送保持寄存器空中断
	outb %al,%dx
	ret
