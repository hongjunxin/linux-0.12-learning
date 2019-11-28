/*
0x17 即二进制 0001 0111，用选择符格式来解析该选择符，其表示的是，
检索 LDT 表，特权级 3，表的索引下标值是 2（第 3 项），
而 LDT 表中的第 3 项表示“用户程序的数据段描述符项”。

0xf 即二进制 0000 1111，用选择符格式来解析该选择符，其表示的是，
检索 LDT 表，特权级 3，表的索引下标值是 1（第 2 项），
而 LDT 表中的第 2 项表示“用户程序的代码段描述符项”。
*/

/* 
执行iret指令后，出栈情况：
lf => %eip
0x0f => %cs
fl => %eflags
move_to_user_mode()执行之前的栈指针 => %esp
0x17 => %ss, 堆栈段选择符不变，即任务0的用户态堆栈就是内核程序使用的堆栈
*/

/* 
其中 0x0f => %cs，CPU判断出目的代码段的特权级是3，与当前内核态的0级
不同，由于特权级发生变化，所以段寄存器DS/ES/FS/GS的值变得无效，CPU会
将它们清零，所以在执行iret指令之后，我们需要手动用值0x17对它们进行装载。
不过好像对于段选择符，我们也就使用了%cs和%fs？
*/

#define move_to_user_mode() \
__asm__ ("movl %%esp,%%eax\n\t" \
	"pushl $0x17\n\t" 		/* 将堆栈段选择符(SS)入栈 */ \
	"pushl %%eax\n\t" 		/* 将堆栈指针值(esp)入栈 */ \
	"pushfl\n\t" 			/* 将标志寄存器(eflags)内容入栈 */ \
	"pushl $0x0f\n\t"       /* 将 Task0 代码段选择符(cs)入栈 */ \
	"pushl $1f\n\t" 		/* 将下面标号 1 的偏移地址(eip)入栈 */ \
	"iret\n" 				/* 执行中断返回指令，则会跳转到下面的标号 1 处 */ \
	"1:\tmovl $0x17,%%eax\n\t"   /* 此时开始执行任务 0 */ \
	"mov %%ax,%%ds\n\t"     /* 段选择符0x17 => ds */ \
	"mov %%ax,%%es\n\t" \
	"mov %%ax,%%fs\n\t" \
	"mov %%ax,%%gs" \
	:::"ax")

#define sti() __asm__ ("sti"::)  // 开中断
#define cli() __asm__ ("cli"::)  // 关中断
#define nop() __asm__ ("nop"::)  // 空操作

#define iret() __asm__ ("iret"::)  // 中断返回

// (0x8000+(dpl<<13)+(type<<8)) => desc_struct.b
// addr(处理函数地址) => desc_struct.a
#define _set_gate(gate_addr,type,dpl,addr) \
__asm__ ("movw %%dx,%%ax\n\t" \
	"movw %0,%%dx\n\t" \
	"movl %%eax,%1\n\t" \
	"movl %%edx,%2" \
	: \
	: "i" ((short) (0x8000+(dpl<<13)+(type<<8))), \
	"o" (*((char *) (gate_addr))), \
	"o" (*(4+(char *) (gate_addr))), \
	"d" ((char *) (addr)),"a" (0x00080000))

#define set_intr_gate(n,addr) \
	_set_gate(&idt[n],14,0,addr)

#define set_trap_gate(n,addr) \
	_set_gate(&idt[n],15,0,addr) 

#define set_system_gate(n,addr) \
	_set_gate(&idt[n],15,3,addr)

// 设置段描述符函数
// 参考书附录<代码段/数据段描述符的介绍>
#define _set_seg_desc(gate_addr,type,dpl,base,limit) {\
	*(gate_addr) = ((base) & 0xff000000) | \
		(((base) & 0x00ff0000)>>16) | \
		((limit) & 0xf0000) | \
		((dpl)<<13) | \
		(0x00408000) | \
		((type)<<8); \
	*((gate_addr)+1) = (((base) & 0x0000ffff)<<16) | \
		((limit) & 0x0ffff); }

/* -- 关于描述符 --
描述符是由8字节构成，用来记录一个地址，可以是代码段/数据段、任务结构体中
tss ldt 成员的基地址，地址的长度只要4字节，所以描述符剩余的4字节可以用来
记录其它信息，比如类型、权限标志等。但是描述符8字节中并不是以连续的比特位
来记录地址的，而是第3-4字节(记录地址的0-15位)、第5字节(记录16-23位)，
第8字节（记录24-31位）。
*/

// n -- GDT 数组中(全局描述符表)指定项的地址，并不是第n项的地址，
// 因为 GDT 中第0不用，第1项是内核代码段描述符，第2项是内核数据段
// 描述符，第3项是系统段（linux不用系统段）
// addr -- 状态段(struct tss_struct)或局部表(struct desc_struct)所在内存的基地址，
// 其实就是任务结构体 task_struct 中的成员 tss 和 ldt 的基地址
// TSS/LDT 描述符的组成和代码段/数据段的描述符组成是一样的
#define _set_tssldt_desc(n,addr,type) \
__asm__ ("movw $104,%1\n\t" \
	"movw %%ax,%2\n\t" \
	"rorl $16,%%eax\n\t" \
	"movb %%al,%3\n\t" \
	"movb $" type ",%4\n\t" \
	"movb $0x00,%5\n\t" \
	"movb %%ah,%6\n\t" \
	"rorl $16,%%eax" \
	::"a" (addr), "m" (*(n)), "m" (*(n+2)), "m" (*(n+4)), \
	 "m" (*(n+5)), "m" (*(n+6)), "m" (*(n+7)) \
	)

#define set_tss_desc(n,addr) _set_tssldt_desc(((char *) (n)),addr,"0x89")
#define set_ldt_desc(n,addr) _set_tssldt_desc(((char *) (n)),addr,"0x82")
