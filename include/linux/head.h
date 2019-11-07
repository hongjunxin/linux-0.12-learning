#ifndef _HEAD_H
#define _HEAD_H

typedef struct desc_struct {
	unsigned long a,b;
} desc_table[256];

extern unsigned long pg_dir[1024];
extern desc_table idt,gdt;  // 中断描述符表，全局描述符表

#define GDT_NUL 0		// 全局描述符表的第 0 项，不使用
#define GDT_CODE 1		// 第 1 项 -- 内核的代码段描述符项
#define GDT_DATA 2		// 第 2 项 -- 内核的数据段描述符项
#define GDT_TMP 3		// 第 3 项 -- 系统描述符项，linux 没有使用

#define LDT_NUL 0		// 每个局部描述符表的第 0 项，不使用
#define LDT_CODE 1		// 第 1 项 -- 用户程序的代码段描述符项
#define LDT_DATA 2		// 第 2 项 -- 用户程序的数据段描述符项

#endif
