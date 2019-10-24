/*
 *  linux/kernel/system_call.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  system_call.s  contains the system-call low-level handling routines.
 * This also contains the timer-interrupt handler, as some of the code is
 * the same. The hd- and flopppy-interrupts are also here.
 *
 * NOTE: This code handles signal-recognition, which happens every time
 * after a timer-interrupt and after each system call. Ordinary interrupts
 * don't handle signal-recognition, as that would clutter them up totally
 * unnecessarily.
 *
 * Stack layout in 'ret_from_system_call':
 *
 *	 0(%esp) - %eax
 *	 4(%esp) - %ebx
 *	 8(%esp) - %ecx
 *	 C(%esp) - %edx
 *	10(%esp) - original %eax	(-1 if not system call)
 *	14(%esp) - %fs
 *	18(%esp) - %es
 *	1C(%esp) - %ds
 *	20(%esp) - %eip
 *	24(%esp) - %cs
 *	28(%esp) - %eflags
 *	2C(%esp) - %oldesp
 *	30(%esp) - %oldss
 */

SIG_CHLD	= 17

EAX		= 0x00		# 堆栈中各个寄存器的偏移位置
EBX		= 0x04
ECX		= 0x08
EDX		= 0x0C
ORIG_EAX	= 0x10
FS		= 0x14
ES		= 0x18
DS		= 0x1C
EIP		= 0x20
CS		= 0x24
EFLAGS		= 0x28
OLDESP		= 0x2C		# 当有特权级变化时
OLDSS		= 0x30

# 以下这些是任务结构(task_struct)中变量的偏移值，参见include/linux/sched.h，77 行开始
state	= 0		# these are offsets into the task-struct. # 进程状态码
counter	= 4		# 运行时间片
priority = 8
signal	= 12	# 信号位图，每个比特位代表一种信号，信号值=位偏移值+1
sigaction = 16		# MUST be 16 (=len of sigaction) 
blocked = (33*16)	# 受阻塞信号位图的偏移量

# 下定义在 sigaction 结构中的偏移量，参见 include/signal.h，第 48 行开始
# offsets within sigaction
sa_handler = 0
sa_mask = 4		# 信号量屏蔽码
sa_flags = 8	# 信号集
sa_restorer = 12	# 恢复函数指针

nr_system_calls = 82	# 内核中的系统调用总数

ENOSYS = 38

/*
 * Ok, I get parallel printer interrupts while using the floppy for some
 * strange reason. Urgel. Now I just ignore them.
 */
.globl _system_call,_sys_fork,_timer_interrupt,_sys_execve
.globl _hd_interrupt,_floppy_interrupt,_parallel_interrupt
.globl _device_not_available, _coprocessor_error

.align 2		# 内存 4 字节对齐
bad_sys_call:
	pushl $-ENOSYS
	jmp ret_from_sys_call
.align 2
reschedule:
	pushl $ret_from_sys_call
	jmp _schedule
.align 2
_system_call:
	push %ds		# 保存原段寄存器的值
	push %es
	push %fs
	pushl %eax		# save the orig_eax
	pushl %edx		# ebx ecx edx 中放着系统调用相应的 C 语言函数的调用参数
	pushl %ecx		# push %ebx,%ecx,%edx as parameters
	pushl %ebx		# to the system call
	movl $0x10,%edx		# set up ds,es to kernel space
	mov %dx,%ds
	mov %dx,%es
	movl $0x17,%edx		# fs points to local data space
	mov %dx,%fs
	cmpl _NR_syscalls,%eax		# 发生系统调用时，系统调用号被存在 eax 中
	jae bad_sys_call
	call _sys_call_table(,%eax,4) 	# 调用地址 = _sys_call_table + %eax * 4
	pushl %eax		# 把系统调用返回值入栈
2:
	movl _current,%eax		# 取当前任务（进程）数据结构地址 -> eax
	cmpl $0,state(%eax)		# state
	jne reschedule			# 如果当前任务不是就绪态（state不等于0），则执行调度程序
	cmpl $0,counter(%eax)		# counter
	je reschedule			# 如果任务的时间片已用完，则执行调度程序
	
# 下这段代码执行从系统调用 C 函数返回后，对信号量进行识别处理。

ret_from_sys_call:

# 先判别当前任务是否是初始任务 task0，如果是则不必对其进行信号量方面的处理，直接返回。
# _task 对应 C 程序中的task[]数组，直接引用 task 相当于引用task[0]	
	movl _current,%eax
	cmpl _task,%eax			# task[0] cannot have signals
	je 3f
	
# 通过对原调用程序代码选择符的检查来判断调用程序是否是内核任务（例如任务 1）。如果是则直接
# 退出中断，否则对于普通进程则需进行信号量的处理。
# 这里比较选择符是否为普通用户代码段的选择符 0x000f（RPL=3，局部表，第1个段（代码段））
	cmpw $0x0f,CS(%esp)		# was old code segment supervisor ?
	jne 3f
	
# 如果原堆栈段选择符不是 0x17（即原堆栈不在用户数据段中），则直接退出
	cmpw $0x17,OLDSS(%esp)		# was stack segment = 0x17 ?
	jne 3f
	
# 取当前任务结构中的信号位图	
	movl signal(%eax),%ebx		# 取信号位图 -> ebx，每 1 位代表 1 种信号，共 32 个信号。
	movl blocked(%eax),%ecx		# 取阻塞（屏蔽）信号位图 -> ecx。
	notl %ecx					# 每位取反
	andl %ebx,%ecx				# 获得许可的信号位图
	bsfl %ecx,%ecx				# 从低位（位 0）开始扫描位图，看是否有置 1 的位，
								# 若有，则 ecx 保留该位的偏移值，所以是取得数值最小的
								# 并且被置 1 的信号量
	je 3f						# 如果没有信号则向前跳转退出
	btrl %ecx,%ebx				# btrl: bit clear. 复位该信号
	movl %ebx,signal(%eax)
	incl %ecx		# 将信号调整为从 1 开始的数（1-32）
	pushl %ecx		# 将信号值入栈，作为调用 do_signal 的参数之一
	call _do_signal		# do_signal() 在 kernel/signal.c 中
	popl %ecx
	testl %eax, %eax
	jne 2b		# see if we need to switch tasks, or do more signals
3:	popl %eax
	popl %ebx
	popl %ecx
	popl %edx
	addl $4, %esp	# skip orig_eax
	pop %fs
	pop %es
	pop %ds
	iret

.align 2
_coprocessor_error:
	push %ds
	push %es
	push %fs
	pushl $-1		# fill in -1 for orig_eax
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	pushl $ret_from_sys_call
	jmp _math_error

.align 2
_device_not_available:
	push %ds
	push %es
	push %fs
	pushl $-1		# fill in -1 for orig_eax
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	pushl $ret_from_sys_call
	clts				# clear TS so that we can use math
	movl %cr0,%eax		# 控制寄存器 cr0
	testl $0x4,%eax			# EM (math emulation bit)
	je _math_state_restore	# kernel/sched.c
	pushl %ebp
	pushl %esi
	pushl %edi
	pushl $0		# temporary storage for ORIG_EIP
	call _math_emulate		# kernel/math/math_emulate.c
	addl $4,%esp
	popl %edi
	popl %esi
	popl %ebp
	ret			# 跳转到 ret_from_sys_call

#### int32 -- (int 0x20) 时钟中断处理程序。中断频率被设置为 
# 100Hz(include/linux/sched.h,5), 定时芯片 8253/8254 
# 是在(kernel/sched.c,406)处初始化的。因此这里 jiffies 
# 每 10 毫秒加1。这段代码将 jiffies 增 1，发送结束中断指令给 
# 8259 控制器，然后用当前特权级作为参数调用 C 函数 do_timer(long CPL)。
# 当调用返回时转去检测并处理信号，即跳转到 ret_from_sys_call

.align 2
_timer_interrupt:
	push %ds		# save ds,es and put kernel data space
	push %es		# into them. %fs is used by _system_call
	push %fs
	pushl $-1		# fill in -1 for orig_eax
	pushl %edx		# we save %eax,%ecx,%edx as gcc doesn't
	pushl %ecx		# save those across function calls. %ebx
	pushl %ebx		# is saved as we use that in ret_sys_call
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	incl _jiffies
	
# 由于初始化中断控制芯片时没有采用自动 EOI，所以这里需要发指令结束该硬件中断
	movb $0x20,%al		# EOI to interrupt controller #1
	outb %al,$0x20
	
	movl CS(%esp),%eax
	andl $3,%eax		# %eax is CPL (0 or 3, 0=supervisor)
	pushl %eax			# 将当前特权级入栈，作为 do_timer() 的参数
	call _do_timer		# 'do_timer(long CPL)' does everything from
						# do_timer 在 kernel/sched.c 中
	addl $4,%esp		# task switching to accounting ...
	jmp ret_from_sys_call

.align 2
_sys_execve:
	lea EIP(%esp),%eax
	pushl %eax
	call _do_execve		# fs/exec.c 中
	addl $4,%esp		# 丢弃调用时压入栈的 EIP 值
	ret

.align 2
_sys_fork:
	call _find_empty_process	# kernel/fork.c
	testl %eax,%eax
	js 1f
	push %gs
	pushl %esi
	pushl %edi
	pushl %ebp
	pushl %eax
	call _copy_process		# kernel/fork.c
	addl $20,%esp			# 丢弃这里所有的压栈内容
1:	ret

#### int 46 -- (int 0x2E) 硬盘中断处理程序，响应硬件中断请求IRQ14
# 当硬盘操作完成或出错就会发出此中断信号。(参见 kernel/blk_drv/hd.c)
_hd_interrupt:
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	movb $0x20,%al
	outb %al,$0xA0		# EOI to interrupt controller #1
	jmp 1f			# give port chance to breathe
1:	jmp 1f
1:	xorl %edx,%edx
	movl %edx,_hd_timeout
	xchgl _do_hd,%edx
	testl %edx,%edx		# do_hd 定义为一个函数指针，将被赋值 read_intr()或 
						# write_intr()函数地址，(kernel/blk_drv/hd.c)
						# 放到 edx 寄存器后就将 do_hd 指针变量置为 NULL。 
	jne 1f
	movl $_unexpected_hd_interrupt,%edx		# 若_do_hd 为空
1:	outb %al,$0x20
	call *%edx		# "interesting" way of handling intr.
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret

### int38 -- (int 0x26) 软盘驱动器中断处理程序，响应硬件中断请求 IRQ6
# 其处理过程与上面对硬盘的处理基本一样。(kernel/blk_drv/floppy.c)
_floppy_interrupt:
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	movb $0x20,%al
	outb %al,$0x20		# EOI to interrupt controller #1
	xorl %eax,%eax
	xchgl _do_floppy,%eax
	testl %eax,%eax
	jne 1f
	movl $_unexpected_floppy_interrupt,%eax
1:	call *%eax		# "interesting" way of handling intr.
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret

### int 39 -- (int 0x27) 并行口中断处理程序，对应硬件中断请求信号 IRQ7
# 本版本内核还未实现。这里只是发送 EOI 指令
_parallel_interrupt:
	pushl %eax
	movb $0x20,%al
	outb %al,$0x20
	popl %eax
	iret
