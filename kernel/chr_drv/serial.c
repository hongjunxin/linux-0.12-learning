/*
 *  linux/kernel/serial.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *	serial.c
 *
 * This module implements the rs232 io functions
 *	void rs_write(struct tty_struct * queue);
 *	void rs_init(void);
 * and all interrupts pertaining to serial IO.
 */

#include <linux/tty.h>
#include <linux/sched.h>
#include <asm/system.h>
#include <asm/io.h>

// 当写队列中含有 WAKEUP_CHARS 个字符时，就开始发送
#define WAKEUP_CHARS (TTY_BUF_SIZE/4)

extern void rs1_interrupt(void);  // 串行口 1 的中断处理程序 rs_io.s
extern void rs2_interrupt(void);  // 串行口 2 的中断处理程序 rs_io.s

static void init(int port)
{
	outb_p(0x80,port+3);	/* set DLAB of line control reg */
	outb_p(0x30,port);	/* LS of divisor (48 -> 2400 bps */
	outb_p(0x00,port+1);	/* MS of divisor */
	outb_p(0x03,port+3);	/* reset DLAB */
	outb_p(0x0b,port+4);	/* set DTR,RTS, OUT_2 */
	outb_p(0x0d,port+1);	/* enable all intrs but writes */
	(void)inb(port);	/* read data port to reset things (?) */
}

void rs_init(void)
{
	set_intr_gate(0x24,rs1_interrupt); // 设置串行口 1 的中断门向量(硬件 IRQ4 信号)
	set_intr_gate(0x23,rs2_interrupt); // 设置串行口 2 的中断门向量(硬件 IRQ3 信号)
	init(tty_table[64].read_q->data); // 初始化串行口 1(.data 是端口号)
	init(tty_table[65].read_q->data); // 初始化串行口2
	outb(inb_p(0x21)&0xE7,0x21);  // 允许主 8259A 芯片的 IRQ3，IRQ4 中断信号请求
}

/*
 * This routine gets called when tty_write has put something into
 * the write_queue. It must check wheter the queue is empty, and
 * set the interrupt register accordingly
 *
 *	void _rs_write(struct tty_struct * tty);
 */
// 串行数据发送输出
// 实际上只是开启串行发送保持寄存器已空中断标志
// 在 UART 将数据发送出去后允许发中断信号
void rs_write(struct tty_struct * tty)
{
	cli();
	if (!EMPTY(tty->write_q))
		outb(inb_p(tty->write_q->data+1)|0x02,tty->write_q->data+1);
	sti();
}
