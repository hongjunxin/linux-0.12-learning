#define move_to_user_mode() \
__asm__ ("movl %%esp,%%eax\n\t" \
	"pushl $0x17\n\t" \		// 将堆栈段选择符(SS)入栈
	"pushl %%eax\n\t" \		// 将堆栈指针值(esp)入栈
	"pushfl\n\t" \			// 将标志寄存器(eflags)内容入栈
	"pushl $0x0f\n\t" \     // 将 Task0 代码段选择符(cs)入栈
	"pushl $1f\n\t" \		// 将下面标号 1 的偏移地址(eip)入栈
	"iret\n" \				// 执行中断返回指令，则会跳转到下面的标号 1 处
	"1:\tmovl $0x17,%%eax\n\t" \  // 此时开始执行任务 0
	"mov %%ax,%%ds\n\t" \   // 初始化段寄存器指向本局部表的数据段
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

#define _set_seg_desc(gate_addr,type,dpl,base,limit) {\
	*(gate_addr) = ((base) & 0xff000000) | \
		(((base) & 0x00ff0000)>>16) | \
		((limit) & 0xf0000) | \
		((dpl)<<13) | \
		(0x00408000) | \
		((type)<<8); \
	*((gate_addr)+1) = (((base) & 0x0000ffff)<<16) | \
		((limit) & 0x0ffff); }

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
