/*
 *  linux/kernel/fork.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also system_call.s), and some misc functions ('verify_area').
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/mm.c': 'copy_page_tables()'
 */
#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>

extern void write_verify(unsigned long address);

long last_pid=0;

void verify_area(void * addr,int size)
{
	unsigned long start;

	start = (unsigned long) addr;
	size += start & 0xfff; // start & 0xfff：获取指定 addr 在所在页面的偏移值
	start &= 0xfffff000;  // start 被调整为 addr 所在页面的页面边界值
	// start 加上进程数据段在线性地址空间中的起始基址，变成系统整个
	// 线性空间中的地址位置。 
	start += get_base(current->ldt[2]);
	while (size>0) {
		size -= 4096;
		write_verify(start);  // 写页面验证。若页面不可写，则复制页面
		start += 4096;
	}
}

// 设置新任务的代码和数据段基址、限长，并复制页表
// nr：新任务号
// p：新任务数据结构指针
/*
0x17 即二进制 0001 0111，用选择符格式来解析该选择符，其表示的是，
检索 LDT 表，特权级 3，表的索引下标值是 2（第 3 项），
而 LDT 表中的第 3 项表示“用户程序的数据段描述符项”。

0xf 即二进制 0000 1111，用选择符格式来解析该选择符，其表示的是，
检索 LDT 表，特权级 3，表的索引下标值是 1（第 2 项），
而 LDT 表中的第 2 项表示“用户程序的代码段描述符项”。
*/
int copy_mem(int nr,struct task_struct * p)
{
	unsigned long old_data_base,new_data_base,data_limit;
	unsigned long old_code_base,new_code_base,code_limit;

	code_limit=get_limit(0x0f); // 取局部描述符表中代码段描述符项中段限长
	data_limit=get_limit(0x17); // 取局部描述符表中数据段描述符项中段限长
	old_code_base = get_base(current->ldt[1]);  // 取原代码段基址
	old_data_base = get_base(current->ldt[2]);	// 取原数据段基址
	if (old_data_base != old_code_base)
		panic("We don't support separate I&D");
	if (data_limit < code_limit)
		panic("Bad data_limit");
	new_data_base = new_code_base = nr * TASK_SIZE;
	p->start_code = new_code_base;
	set_base(p->ldt[1],new_code_base);
	set_base(p->ldt[2],new_data_base);
	if (copy_page_tables(old_data_base,new_data_base,data_limit)) {
		free_page_tables(new_data_base,data_limit);  // 如果出错则释放申请的内存
		return -ENOMEM;
	}
	return 0;
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in it's entirety.
 */
int copy_process(int nr,long ebp,long edi,long esi,long gs,long none,
		long ebx,long ecx,long edx, long orig_eax, 
		long fs,long es,long ds,
		long eip,long cs,long eflags,long esp,long ss)
{
	struct task_struct *p;
	int i;
	struct file *f;

	p = (struct task_struct *) get_free_page();
	if (!p)
		return -EAGAIN;
	task[nr] = p;
	*p = *current;	/* NOTE! this doesn't copy the supervisor stack */
	p->state = TASK_UNINTERRUPTIBLE;
	p->pid = last_pid;  // 新进程号，由前面调用 find_empty_process() 得到
	p->counter = p->priority;
	p->signal = 0;
	p->alarm = 0;
	p->leader = 0;		/* process leadership doesn't inherit */
	p->utime = p->stime = 0;
	p->cutime = p->cstime = 0;  // 初始化子进程用户态和核心态时间
	p->start_time = jiffies;
	p->tss.back_link = 0;
	p->tss.esp0 = PAGE_SIZE + (long) p;  // 内核态堆栈指针
	p->tss.ss0 = 0x10;  // 堆栈段选择符（与内核数据段相同）
	p->tss.eip = eip;
	p->tss.eflags = eflags;
	p->tss.eax = 0;  // 这是当 fork() 返回时，新进程会返回 0 的原因所在
	p->tss.ecx = ecx;
	p->tss.edx = edx;
	p->tss.ebx = ebx;
	p->tss.esp = esp;  // 新进程完全复制了父进程的堆栈内容
	p->tss.ebp = ebp;
	p->tss.esi = esi;
	p->tss.edi = edi;
	p->tss.es = es & 0xffff;  // 段寄存器仅 16 位有效
	p->tss.cs = cs & 0xffff;
	p->tss.ss = ss & 0xffff;
	p->tss.ds = ds & 0xffff;
	p->tss.fs = fs & 0xffff;
	p->tss.gs = gs & 0xffff;
	p->tss.ldt = _LDT(nr);
	p->tss.trace_bitmap = 0x80000000;  // 高 16 位有效
	if (last_task_used_math == current)
		__asm__("clts ; fnsave %0 ; frstor %0"::"m" (p->tss.i387));
	if (copy_mem(nr,p)) {  // 设置新任务的代码和数据段基址、限长并复制页表
		task[nr] = NULL;
		free_page((long) p);
		return -EAGAIN;
	}
	for (i=0; i<NR_OPEN;i++)
		if (f=p->filp[i])
			f->f_count++;
	if (current->pwd)
		current->pwd->i_count++;
	if (current->root)
		current->root->i_count++;
	if (current->executable)
		current->executable->i_count++;
	if (current->library)
		current->library->i_count++;
	// 在 GDT 中设置新任务的 TSS 和 LDT 描述符，数据从 task 结构中取。
	// 在任务切换时，任务寄存器 tr 由 CPU 自动加载
	set_tss_desc(gdt+(nr<<1)+FIRST_TSS_ENTRY,&(p->tss));
	set_ldt_desc(gdt+(nr<<1)+FIRST_LDT_ENTRY,&(p->ldt));
	p->p_pptr = current;
	p->p_cptr = 0;
	p->p_ysptr = 0;
	p->p_osptr = current->p_cptr;
	if (p->p_osptr)
		p->p_osptr->p_ysptr = p;
	current->p_cptr = p;
	p->state = TASK_RUNNING;	/* do this last, just in case */
	return last_pid;	// 返回新进程号（与任务号是不同的）
}

// 为新进程取得不重复的进程号 last_pid
// 并返回任务数组中的任务号（即数组索引）
int find_empty_process(void)
{
	int i;

	repeat:
		if ((++last_pid)<0) last_pid=1;
		for(i=0 ; i<NR_TASKS ; i++)
			if (task[i] && ((task[i]->pid == last_pid) ||
				        (task[i]->pgrp == last_pid)))
				goto repeat;
	for(i=1 ; i<NR_TASKS ; i++)
		if (!task[i])
			return i;
	return -EAGAIN;
}
