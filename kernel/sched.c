/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/sys.h>
#include <linux/fdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <signal.h>

#define _S(nr) (1<<((nr)-1))
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))

void show_task(int nr,struct task_struct * p)
{
	int i,j = 4096-sizeof(struct task_struct);

	printk("%d: pid=%d, state=%d, father=%d, child=%d, ",nr,p->pid,
		p->state, p->p_pptr->pid, p->p_cptr ? p->p_cptr->pid : -1);
	i=0;
	while (i<j && !((char *)(p+1))[i])
		i++;
	printk("%d/%d chars free in kstack\n\r",i,j);
	printk("   PC=%08X.", *(1019 + (unsigned long *) p));
	if (p->p_ysptr || p->p_osptr) 
		printk("   Younger sib=%d, older sib=%d\n\r", 
			p->p_ysptr ? p->p_ysptr->pid : -1,
			p->p_osptr ? p->p_osptr->pid : -1);
	else
		printk("\n\r");
}

void show_state(void)
{
	int i;

	printk("\rTask-info:\n\r");
	for (i=0;i<NR_TASKS;i++)
		if (task[i])
			show_task(i,task[i]);
}

#define LATCH (1193180/HZ)		// 定义每个时间片的滴答数

extern void mem_use(void);

extern int timer_interrupt(void);		// kernel/system_call.s
extern int system_call(void);		// kernel/system_call.s

union task_union {				// 定义任务联合（任务结构成员和 stack 字符数组程序成员）
	struct task_struct task;	// 因为一个任务的数据结构和它的内核态堆栈放在同一内存页
	char stack[PAGE_SIZE];		// 中，所以从堆栈段寄存器 ss 可以获得它的数据段选择符。
};

static union task_union init_task = {INIT_TASK,};  // INIT_TASK 在 sched.h

unsigned long volatile jiffies=0;	// 从开机开始算起的滴答数（10ms/滴答）
unsigned long startup_time=0;		// 开机时间。从 1970:0:0:0 开始计时的秒数
int jiffies_offset = 0;		/* # clock ticks to add to get "true
				   time".  Should always be less than
				   1 second's worth.  For time fanatics
				   who like to syncronize their machines
				   to WWV :-) */

struct task_struct *current = &(init_task.task);
struct task_struct *last_task_used_math = NULL;

struct task_struct * task[NR_TASKS] = {&(init_task.task), };

long user_stack [ PAGE_SIZE>>2 ] ;

// 该结构体用于设置堆栈 ss:esp（数据段选择符，指针），见 head.s，第23行
struct {
	long * a;
	short b;
	} stack_start = { & user_stack [PAGE_SIZE>>2] , 0x10 };
/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 */
 // 当任务被调度交换过以后，该函数用以保存原任务的协处理器状态（上下文）
 // 并恢复新调度进来的当前任务的协处理器执行状态。
void math_state_restore()
{
	if (last_task_used_math == current)
		return;
	__asm__("fwait");		// 在发送协处理器命令之前要先发送 WAIT 指令
	if (last_task_used_math) {		// 如果上个任务使用了协处理器，则保存其状态
		__asm__("fnsave %0"::"m" (last_task_used_math->tss.i387));
	}
	last_task_used_math=current;
	if (current->used_math) {	// 如果当前任务用过协处理器，则恢复其状态
		__asm__("frstor %0"::"m" (current->tss.i387));
	} else {					// 否则的话说明是第一次使用
		__asm__("fninit"::);	// 则向协处理器发送初始化指令
		current->used_math=1;	// 并且置位使用了协处理器标志位
	}
}

/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 */
void schedule(void)
{
	int i,next,c;
	struct task_struct ** p;

/* check alarm, wake up any interruptible tasks that have got a signal */

	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
		if (*p) {
			if ((*p)->timeout && (*p)->timeout < jiffies) {
				(*p)->timeout = 0;
				if ((*p)->state == TASK_INTERRUPTIBLE)
					(*p)->state = TASK_RUNNING;
			}
			if ((*p)->alarm && (*p)->alarm < jiffies) {
				(*p)->signal |= (1<<(SIGALRM-1));
				(*p)->alarm = 0;
			}
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
			(*p)->state==TASK_INTERRUPTIBLE)
				(*p)->state=TASK_RUNNING;
		}

/* this is the scheduler proper: */

	while (1) {
		c = -1;
		next = 0;
		i = NR_TASKS;
		p = &task[NR_TASKS];
		while (--i) {  /* 任务0不参与剩余时间片的比较 */
			if (!*--p)
				continue;
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c)
				c = (*p)->counter, next = i;
		}
		if (c) break;  
		/* 处于running状态的任务，它们剩余的时间片都用完了，
		   则全部重新分配时间片，包括不处于running状态的任务，但不包括任务0 */
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
			if (*p)
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority;
	}
	switch_to(next);
}

int sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}

static inline void __sleep_on(struct task_struct **p, int state)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp = *p;  // 保存睡眠队列当前的首位任务指针
	*p = current;
	current->state = state;
repeat:	schedule(); // schedule()中只唤醒可中断的任务，其实就是让任务的state等于0
	if (*p && *p != current) { // 从被唤醒到执行这条语句之间，有其它任务T被加入到等待队列
		(**p).state = 0;	   // 首位，所以把这次的被唤醒机会让给了任务T，自己再次进入不可
		current->state = TASK_UNINTERRUPTIBLE;  // 中断状态，等待下次wake_up()的唤醒？
		goto repeat;
	}
	if (!*p)
		printk("Warning: *P = NULL\n\r");
	if (*p = tmp)      
		tmp->state=0;  // 为什么要唤醒tmp？
}

/* 将当前任务置为可中断的等待状态，并放入 *p 指定的等待队列中 */
void interruptible_sleep_on(struct task_struct **p)
{
	__sleep_on(p,TASK_INTERRUPTIBLE);
}

/*
 * 当前任务置为不可中断的等待状态，并让睡眠队列头的指针指向当前任务。
 * 只有明确地唤醒时才会返回。该函数提供了进程与中断处理程序之间的同步机制。
 * 函数参数 *p 是放置等待任务的队列头指针
 */
void sleep_on(struct task_struct **p)
{
	__sleep_on(p,TASK_UNINTERRUPTIBLE);
}

void wake_up(struct task_struct **p)
{
	if (p && *p) {
		if ((**p).state == TASK_STOPPED)
			printk("wake_up: TASK_STOPPED");
		if ((**p).state == TASK_ZOMBIE)
			printk("wake_up: TASK_ZOMBIE");
		(**p).state=0;
	}
}

/*
 * OK, here are some floppy things that shouldn't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 */
static struct task_struct * wait_motor[4] = {NULL,NULL,NULL,NULL};
static int  mon_timer[4]={0,0,0,0};
static int moff_timer[4]={0,0,0,0};
unsigned char current_DOR = 0x0C;

int ticks_to_floppy_on(unsigned int nr)
{
	extern unsigned char selected;
	unsigned char mask = 0x10 << nr;

	if (nr>3)
		panic("floppy_on: nr>3");
	moff_timer[nr]=10000;		/* 100 s = very big :-) */
	cli();				/* use floppy_off to turn it off */
	mask |= current_DOR;
	if (!selected) {
		mask &= 0xFC;
		mask |= nr;
	}
	if (mask != current_DOR) {
		outb(mask,FD_DOR);
		if ((mask ^ current_DOR) & 0xf0)
			mon_timer[nr] = HZ/2;
		else if (mon_timer[nr] < 2)
			mon_timer[nr] = 2;
		current_DOR = mask;
	}
	sti();
	return mon_timer[nr];
}

void floppy_on(unsigned int nr)
{
	cli();
	while (ticks_to_floppy_on(nr))
		sleep_on(nr+wait_motor);
	sti();
}

void floppy_off(unsigned int nr)
{
	moff_timer[nr]=3*HZ;
}

void do_floppy_timer(void)
{
	int i;
	unsigned char mask = 0x10;

	for (i=0 ; i<4 ; i++,mask <<= 1) {
		if (!(mask & current_DOR))
			continue;
		if (mon_timer[i]) {
			if (!--mon_timer[i])
				wake_up(i+wait_motor);
		} else if (!moff_timer[i]) {
			current_DOR &= ~mask;
			outb(current_DOR,FD_DOR);
		} else
			moff_timer[i]--;
	}
}

#define TIME_REQUESTS 64	// 最多可以有64个定时器链表

static struct timer_list {
	long jiffies;
	void (*fn)();
	struct timer_list * next;
} timer_list[TIME_REQUESTS], * next_timer = NULL;

// 添加定时器，输入参数为指定的定时值（滴答数）和相应的处理程序指针
void add_timer(long jiffies, void (*fn)(void))
{
	struct timer_list * p;

	if (!fn)
		return;
	cli();
	if (jiffies <= 0)
		(fn)();
	else {
		for (p = timer_list ; p < timer_list + TIME_REQUESTS ; p++)
			if (!p->fn)
				break;
		if (p >= timer_list + TIME_REQUESTS)
			panic("No more time requests free");
		p->fn = fn;
		p->jiffies = jiffies;
		p->next = next_timer;
		next_timer = p;
		while (p->next && p->next->jiffies < p->jiffies) {
			p->jiffies -= p->next->jiffies;
			fn = p->fn;
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			p->jiffies = p->next->jiffies;
			p->next->jiffies = jiffies;
			p = p->next;
		}
	}
	sti();
}

// 时钟中断 C 函数处理程序，在 kernel/system_call.s 中的 _timer_interrupt 被
// 调用。参数 cpl 是当前特权级 0 或 3，0 表示在内核代码在执行
// 对于一个进程由于执行时间片用完时，则进行任务切换，并执行一个计时更新工作
void do_timer(long cpl)
{
	static int blanked = 0;

	if (blankcount || !blankinterval) {
		if (blanked)
			unblank_screen();
		if (blankcount)
			blankcount--;
		blanked = 0;
	} else if (!blanked) {
		blank_screen();
		blanked = 1;
	}
	if (hd_timeout)
		if (!--hd_timeout)
			hd_times_out();

	if (beepcount)
		if (!--beepcount)
			sysbeepstop();

	if (cpl)
		current->utime++;
	else
		current->stime++;

	if (next_timer) {
		next_timer->jiffies--;
		while (next_timer && next_timer->jiffies <= 0) {
			void (*fn)(void);
			
			fn = next_timer->fn;
			next_timer->fn = NULL;
			next_timer = next_timer->next;
			(fn)();
		}
	}
	if (current_DOR & 0xf0)
		do_floppy_timer();
	if ((--current->counter)>0) return;  // 如果进程运行时间还没完，则退出
	current->counter=0;
	if (!cpl) return;  // 对于内核态程序，不依赖 counter 值进行调度
	schedule();
}

// 设置报警定时时间值（秒）
int sys_alarm(long seconds)
{
	int old = current->alarm;

	if (old)
		old = (old - jiffies) / HZ;
	current->alarm = (seconds>0)?(jiffies+HZ*seconds):0;
	return (old);
}

int sys_getpid(void)
{
	return current->pid;
}

int sys_getppid(void)
{
	return current->p_pptr->pid;
}

int sys_getuid(void)
{
	return current->uid;
}

int sys_geteuid(void)
{
	return current->euid;
}

int sys_getgid(void)
{
	return current->gid;
}

int sys_getegid(void)
{
	return current->egid;
}

int sys_nice(long increment)
{
	if (current->priority-increment>0)
		current->priority -= increment;
	return 0;
}

void sched_init(void)
{
	int i;
	struct desc_struct * p;  // 描述符表结构指针

	if (sizeof(struct sigaction) != 16)
		panic("Struct sigaction MUST be 16 bytes");
	// 设置初始任务（任务 0）的任务状态段描述符和局部数据表
	// 描述符(include/asm/system.h,65
	set_tss_desc(gdt+FIRST_TSS_ENTRY,&(init_task.task.tss));
	set_ldt_desc(gdt+FIRST_LDT_ENTRY,&(init_task.task.ldt));
	p = gdt+2+FIRST_TSS_ENTRY;
	for(i=1;i<NR_TASKS;i++) {
		task[i] = NULL;
		p->a=p->b=0;
		p++;
		p->a=p->b=0;
		p++;
	}
/* Clear NT, so that we won't have troubles with that later on */
	__asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl");
	ltr(0);		// 将任务 0 的 TSS 段描述符地址加载到任务寄存器 tr
	lldt(0);	// 将局部描述符表加载到局部描述符表寄存器
	
	// 初始化 8253 定时器
	outb_p(0x36,0x43);		/* binary, mode 3, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff , 0x40);	/* LSB */
	outb(LATCH >> 8 , 0x40);	/* MSB */
	
	set_intr_gate(0x20,&timer_interrupt);  // 设置时钟中断处理程序句柄
	outb(inb_p(0x21)&~0x01,0x21);  // 修改中断控制器屏蔽码，允许时钟中断
	set_system_gate(0x80,&system_call);  // 设置系统调用中断
}
