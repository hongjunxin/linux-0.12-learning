// 读取fs段中指定地址处的字节
// addr - 指定的内存地址
// %0 - (返回的字节_v)；%1 - (内存地址addr)
// 返回：返回内存fs:[addr]处的字节
/*
put/get_fs_byte系列的函数，用来当处于内核态时访问用户态数据。
在用户态运行时，fs存放的是当前运行进程的数据段基地址，当该进程执行
系统调用时，传递给系统调用的参数地址是该进程对应的可执行程序中编译出来
的相对地址。当在用户态的情况下访问该参数时，CPU会自动加上fs中的基地址，
组合成线性地址再经过分页换算，得到物理地址。但在内核态运行时，CPU选择
的数据段基地址是内核态的（内核态的代码段/数据段的基地址都为0），而不是
执行系统调用进程的，所以在内核态时，需要手动加上fs中的当前运行进程的
数据段基地址，才能访问到传进来的参数。

PS：fs寄存器用来存放进程在用户态时数据段的基地址
*/
extern inline unsigned char get_fs_byte(const char * addr)
{
	unsigned register char _v;

	__asm__ ("movb %%fs:%1,%0":"=r" (_v):"m" (*addr));
	return _v;
}

extern inline unsigned short get_fs_word(const unsigned short *addr)
{
	unsigned short _v;

	__asm__ ("movw %%fs:%1,%0":"=r" (_v):"m" (*addr));
	return _v;
}

extern inline unsigned long get_fs_long(const unsigned long *addr)
{
	unsigned long _v;

	__asm__ ("movl %%fs:%1,%0":"=r" (_v):"m" (*addr)); \
	return _v;
}

extern inline void put_fs_byte(char val,char *addr)
{
__asm__ ("movb %0,%%fs:%1"::"r" (val),"m" (*addr));
}

extern inline void put_fs_word(short val,short * addr)
{
__asm__ ("movw %0,%%fs:%1"::"r" (val),"m" (*addr));
}

extern inline void put_fs_long(unsigned long val,unsigned long * addr)
{
__asm__ ("movl %0,%%fs:%1"::"r" (val),"m" (*addr));
}

/*
 * Someone who knows GNU asm better than I should double check the followig.
 * It seems to work, but I don't know if I'm doing something subtly wrong.
 * --- TYT, 11/24/91
 * [ nothing wrong here, Linus ]
 */

extern inline unsigned long get_fs() 
{
	unsigned short _v;
	__asm__("mov %%fs,%%ax":"=a" (_v):);
	return _v;
}

extern inline unsigned long get_ds() 
{
	unsigned short _v;
	__asm__("mov %%ds,%%ax":"=a" (_v):);
	return _v;
}

extern inline void set_fs(unsigned long val)
{
	__asm__("mov %0,%%fs"::"a" ((unsigned short) val));
}

