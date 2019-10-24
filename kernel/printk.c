/*
 *  linux/kernel/printk.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * When in kernel-mode, we cannot use printf, as fs is liable to
 * point to 'interesting' things. Make a printf with fs-saving, and
 * all is well.
 */
#include <stdarg.h>
#include <stddef.h>

#include <linux/kernel.h>

static char buf[1024];

extern int vsprintf(char * buf, const char * fmt, va_list args);

int printk(const char *fmt, ...)
{
	va_list args;
	int i;

	// 在栈中为 fmt 分配了空间，用来存储它的值，所以 &fmt 返回的是
	// fmt 在栈中的地址。另外，参数是从右向左，依次将参数入栈的，并且
	// 栈是向下生长，所以 va_list 的第一个参数地址是 &fmt 加上 sizeof(char*)

	va_start(args, fmt);
	i=vsprintf(buf,fmt,args);
	va_end(args);
	console_print(buf);
	return i;
}
