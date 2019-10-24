/*
 *  linux/kernel/keyboard.S
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *	Thanks to Alfred Leung for US keyboard patches
 *		Wolfgang Thiel for German keyboard patches
 *		Marc Corsini for the French keyboard
 */

/* KBD_FINNISH for Finnish keyboards
 * KBD_US for US-type
 * KBD_GR for German keyboards
 * KBD_FR for Frech keyboard
 */
#define KBD_FINNISH

.text
.globl _keyboard_interrupt

/*
 * these are for the keyboard read functions
 */
size	= 1024		/* must be a power of two ! And MUST be the same
			   as in tty_io.c !!!! */
			   
// struct tty_queue 中的字段
head = 4			// 缓冲区中头指针字段偏移
tail = 8			// 缓冲区中尾指针字段偏移
proc_list = 12		// 等待该缓冲队列的进程字段偏移
buf = 16 			// 缓冲区字段偏移


// mode 是键盘特殊键的按下状态标志。     
// 表示大小写转换键(caps)、交换键(alt)、控制键(ctrl)和换档键(shift)的状态。     
// 位 7 caps 键按下；     
// 位 6 caps 键的状态(应该与 leds 中的对应标志位一样)；     
// 位 5 右 alt 键按下；     
// 位 4 左 alt 键按下；     
// 位 3 右 ctrl 键按下；     
// 位 2 左 ctrl 键按下；     
// 位 1 右 shift 键按下；     
// 位 0 左 shift 键按下。
mode:	.byte 0		/* caps, alt, ctrl and shift mode */

// 位 7-3 全 0 不用
// 位 2 caps-lock
// 位 1 num-lock 初始置 1
// 位 0 scroll-lock
leds:	.byte 2		/* num-lock, caps, scroll-lock mode (num-lock on) */

// 当扫描码是 0xe0 或 0xe1 时，置该标志。表示其后还跟随着 1 个或 2 个字符扫描码
// 位 1=1 收到 0xe1 标志
// 位 0=1 收到 0xe0 标志
e0:	.byte 0

/*
 *  con_int is the real interrupt routine that reads the
 *  keyboard scan-code and converts it into the appropriate
 *  ascii character(s).
 */
_keyboard_interrupt:
	pushl %eax
	pushl %ebx
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	movl $0x10,%eax  // 将 ds、es 段寄存器置为内核数据段
	mov %ax,%ds
	mov %ax,%es
	movl _blankinterval,%eax
	movl %eax,_blankcount
	xorl %eax,%eax		/* %eax is scan code */
	inb $0x60,%al       // 读取扫描码 -> al
	cmpb $0xe0,%al
	je set_e0
	cmpb $0xe1,%al
	je set_e1
	call key_table(,%eax,4)  // 调用键处理程序 key_table + eax * 4
	movb $0,e0		// 复位 e0 标志
	
// 下面这段代码是针对使用 8255A 的 PC 标准键盘电路进行硬件复位处理。端口 0x61 是
// 	8255A 输出口 B 的地址，该输出端口的第 7 位(PB7)用于禁止和允许对键盘数据的处理。
// 这段程序用于对收到的扫描码做出应答。方法是首先禁止键盘，然后立刻重新允许键盘工作
e0_e1:	inb $0x61,%al
	jmp 1f
1:	jmp 1f
1:	orb $0x80,%al
	jmp 1f
1:	jmp 1f
1:	outb %al,$0x61  // 使 PPI PB7 位置位
	jmp 1f
1:	jmp 1f
1:	andb $0x7F,%al
	outb %al,$0x61  // 使 PPI PB7 复位（允许键盘工作）
	movb $0x20,%al
	outb %al,$0x20  // 向 8259 中断芯片发送 EOI（中断结束）信号
	pushl $0	    // 控制台 tty 号=0，作为参数入栈
	call _do_tty_interrupt  // 将收到数据复制成规范模式数据并存放在规范字符缓冲队列中
	addl $4,%esp	// 丢弃入栈的参数，弹出保留的寄存器，并中断返回
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %ebx
	popl %eax
	iret
set_e0:	movb $1,e0
	jmp e0_e1
set_e1:	movb $2,e0
	jmp e0_e1

/*
 * This routine fills the buffer with max 8 bytes, taken from
 * %ebx:%eax. (%edx is high). The bytes are written in the
 * order %al,%ah,%eal,%eah,%bl,%bh ... until %eax is zero.
 */
put_queue:
	pushl %ecx
	pushl %edx
	movl _table_list,%edx		# read-queue for console
	movl head(%edx),%ecx        // 取缓冲队列中头指针 -> ecx
1:	movb %al,buf(%edx,%ecx)		// 将 al 中的字符放入缓冲队列头指针位置处
	incl %ecx					// 头指针前移 1 字节
	andl $size-1,%ecx			// 以缓冲区大小调整头指针（若超出则返回缓冲区开始）
	cmpl tail(%edx),%ecx		# buffer full - discard everything
								// 缓冲队列满？
	je 3f
	shrdl $8,%ebx,%eax  // 将 ebx 中 8 位比特位右移 8 位到 eax 中，但 ebx 不变
	je 2f               // 还有字符吗？即 eax 是否等于 0
	shrl $8,%ebx        // 将 ebx 中比特位右移 8 位，并跳转到标号 1
	jmp 1b
2:	movl %ecx,head(%edx)       // 若已将所有字符都放入了队列，则保存头指针 
	movl proc_list(%edx),%ecx  
	testl %ecx,%ecx			   // 有等待该队列的进程吗？
	je 3f
	movl $0,(%ecx)  		   // 有，则置该进程为就绪状态，struct task_struct 
							   // 的第一个域是 long state
3:	popl %edx
	popl %ecx
	ret

ctrl:	movb $0x04,%al  // 0x4 是模式标志 mode 中左 ctrl 键对应的比特位(位 2)
	jmp 1f
alt:	movb $0x10,%al  // 0x10是模式标志 mode 中左 alt 键对应的比特位(位 4)
1:	cmpb $0,e0			// e0 标志置位了吗(按下的是右边的 ctrl 或 alt 键吗)？
	je 2f				// 不是则跳转
	addb %al,%al		// 是，则改成置相应右键的标志位(位 3 或位5)
2:	orb %al,mode		// 设置模式标志 mode 中对应的比特位
	ret
	
// 这段代码处理 ctrl 或 alt 键松开的扫描码，对应复位模式标志 mode 中的比特位
// 在处理时要根据 e0 标志是否置位来判断是否是键盘右边的 ctrl 或 alt 键
unctrl:	movb $0x04,%al
	jmp 1f
unalt:	movb $0x10,%al
1:	cmpb $0,e0
	je 2f
	addb %al,%al
2:	notb %al
	andb %al,mode
	ret

lshift:
	orb $0x01,mode
	ret
unlshift:
	andb $0xfe,mode
	ret
rshift:
	orb $0x02,mode
	ret
unrshift:
	andb $0xfd,mode
	ret

caps:	testb $0x80,mode  // 测试 mode 的第 7 位是否已经置位（按下状态）
	jne 1f			 // 如果已处于按下状态，则返回
	xorb $4,leds     // 翻转 leds 标志中 caps-lock 位
	xorb $0x40,mode  // 翻转 mode 标志中 caps 键按下的位
	orb $0x80,mode   // 设置 mode 标志中 caps 键已按下标志位

// 根据 leds 标志，开启或关闭 LED 指示器
set_leds:
	call kb_wait		// 等待键盘控制器输入缓冲位为空
	movb $0xed,%al		/* set leds command */
	outb %al,$0x60      // 发送键盘命令 0xed 到端口 0x60
	call kb_wait
	movb leds,%al
	outb %al,$0x60
	ret
uncaps:	andb $0x7f,mode
	ret
scroll:
	testb $0x03,mode
	je 1f
	call _show_mem
	jmp 2f
1:	call _show_state
2:	xorb $1,leds
	jmp set_leds
num:	xorb $2,leds
	jmp set_leds

/*
 *  curosr-key/numeric keypad cursor keys are handled here.
 *  checking for numeric keypad etc.
 */
// je（同义名 jz）， 相等/零 则跳转
// jne（同义名 jnz），不相等/非零 则跳转 
// ja 超过则跳转
// cmpb S1,S2  则 S2- S1
// testb S1,S2 则 S1&S2
cursor:
	subb $0x47,%al
	jb 1f				// 扫描码 < 0x47 则不处理
	cmpb $12,%al		
	ja 1f				// 扫描码 > 0x53 则不处理
	jne cur2		/* check for ctrl-alt-del */
	                    // 如果等于 12，则说明 del 键已被按下，则
						// 继续判断 ctrl 和 alt 是否也被同时按下
	testb $0x0c,mode    // ctrl 键有被按下吗
	je cur2
	testb $0x30,mode    // alt 键有被按下吗
	jne reboot          // 有，则跳转到重启动处理
cur2:	cmpb $0x01,e0		/* e0 forces cursor movement */
	je cur				// e0 被置位了则跳转
	testb $0x02,leds	/* not num-lock forces cursor */
	je cur
	testb $0x03,mode	/* shift forces cursor */
	jne cur
	xorl %ebx,%ebx      // 否则查询扫数字表(199 行)，取对应键的数字 ASCII码
	movb num_table(%eax),%al  // 以 eax 为索引值，取对应数字字符 -> al
	jmp put_queue
1:	ret

cur:	movb cur_table(%eax),%al  // 取光标字符表中相应键的代表字符 -> al
	cmpb $'9,%al  // 若该字符 <= '9'，说明是上一页、下一页、插入或删除键
	ja ok_cur	
	movb $'~,%ah
ok_cur:	shll $16,%eax	// 将 ax 中内容移到 eax 高字节
	movw $0x5b1b,%ax	// 在 ax 中放入字符 'esc [' ，与 eax 高字节中字符组成移动序列
	xorl %ebx,%ebx
	jmp put_queue

#if defined(KBD_FR)
num_table:
	.ascii "789 456 1230."  // 数字小键盘上键对应的数字 ASCII 码表
#else
num_table:
	.ascii "789 456 1230,"
#endif
cur_table:
	.ascii "HA5 DGC YB623"  // 数字小键盘上方向键或插入删除键对应的移动表示字符表

/*
 * this routine handles function keys
 */
func:
	subb $0x3B,%al
	jb end_func
	cmpb $9,%al
	jbe ok_func
	subb $18,%al
	cmpb $10,%al
	jb end_func
	cmpb $11,%al
	ja end_func
ok_func:
	testb $0x10,mode
	jne alt_func
	cmpl $4,%ecx		/* check that there is enough room */
	jl end_func
	movl func_table(,%eax,4),%eax
	xorl %ebx,%ebx
	jmp put_queue
alt_func:
	pushl %eax
	call _change_console
	popl %eax
end_func:
	ret

/*
 * function keys send F1:'esc [ [ A' F2:'esc [ [ B' etc.
 */
func_table:
	.long 0x415b5b1b,0x425b5b1b,0x435b5b1b,0x445b5b1b
	.long 0x455b5b1b,0x465b5b1b,0x475b5b1b,0x485b5b1b
	.long 0x495b5b1b,0x4a5b5b1b,0x4b5b5b1b,0x4c5b5b1b

#if	defined(KBD_FINNISH)
key_map:
	.byte 0,27
	.ascii "1234567890+'"
	.byte 127,9
	.ascii "qwertyuiop}"
	.byte 0,13,0
	.ascii "asdfghjkl|{"
	.byte 0,0
	.ascii "'zxcvbnm,.-"
	.byte 0,'*,0,32		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte '-,0,0,0,'+	/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '<
	.fill 10,1,0

shift_map:
	.byte 0,27
	.ascii "!\"#$%&/()=?`"
	.byte 127,9
	.ascii "QWERTYUIOP]^"
	.byte 13,0
	.ascii "ASDFGHJKL\\["
	.byte 0,0
	.ascii "*ZXCVBNM;:_"
	.byte 0,'*,0,32		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte '-,0,0,0,'+	/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '>
	.fill 10,1,0

alt_map:
	.byte 0,0
	.ascii "\0@\0$\0\0{[]}\\\0"
	.byte 0,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte '~,13,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte 0,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte 0,0,0,0		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte 0,0,0,0,0		/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '|
	.fill 10,1,0

#elif defined(KBD_US)

key_map:
	.byte 0,27
	.ascii "1234567890-="
	.byte 127,9
	.ascii "qwertyuiop[]"
	.byte 13,0
	.ascii "asdfghjkl;'"
	.byte '`,0
	.ascii "\\zxcvbnm,./"
	.byte 0,'*,0,32		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte '-,0,0,0,'+	/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '<
	.fill 10,1,0


shift_map:
	.byte 0,27
	.ascii "!@#$%^&*()_+"
	.byte 127,9
	.ascii "QWERTYUIOP{}"
	.byte 13,0
	.ascii "ASDFGHJKL:\""
	.byte '~,0
	.ascii "|ZXCVBNM<>?"
	.byte 0,'*,0,32		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte '-,0,0,0,'+	/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '>
	.fill 10,1,0

alt_map:
	.byte 0,0
	.ascii "\0@\0$\0\0{[]}\\\0"
	.byte 0,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte '~,13,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte 0,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte 0,0,0,0		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte 0,0,0,0,0		/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '|
	.fill 10,1,0

#elif defined(KBD_GR)

key_map:
	.byte 0,27
	.ascii "1234567890\\'"
	.byte 127,9
	.ascii "qwertzuiop@+"
	.byte 13,0
	.ascii "asdfghjkl[]^"
	.byte 0,'#
	.ascii "yxcvbnm,.-"
	.byte 0,'*,0,32		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte '-,0,0,0,'+	/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '<
	.fill 10,1,0


shift_map:
	.byte 0,27
	.ascii "!\"#$%&/()=?`"
	.byte 127,9
	.ascii "QWERTZUIOP\\*"
	.byte 13,0
	.ascii "ASDFGHJKL{}~"
	.byte 0,''
	.ascii "YXCVBNM;:_"
	.byte 0,'*,0,32		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte '-,0,0,0,'+	/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '>
	.fill 10,1,0

alt_map:
	.byte 0,0
	.ascii "\0@\0$\0\0{[]}\\\0"
	.byte 0,0
	.byte '@,0,0,0,0,0,0,0,0,0,0
	.byte '~,13,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte 0,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte 0,0,0,0		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte 0,0,0,0,0		/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '|
	.fill 10,1,0


#elif defined(KBD_FR)

key_map:
	.byte 0,27
	.ascii "&{\"'(-}_/@)="
	.byte 127,9
	.ascii "azertyuiop^$"
	.byte 13,0
	.ascii "qsdfghjklm|"
	.byte '`,0,42		/* coin sup gauche, don't know, [*|mu] */
	.ascii "wxcvbn,;:!"
	.byte 0,'*,0,32		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte '-,0,0,0,'+	/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '<
	.fill 10,1,0

shift_map:
	.byte 0,27
	.ascii "1234567890]+"
	.byte 127,9
	.ascii "AZERTYUIOP<>"
	.byte 13,0
	.ascii "QSDFGHJKLM%"
	.byte '~,0,'#
	.ascii "WXCVBN?./\\"
	.byte 0,'*,0,32		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte '-,0,0,0,'+	/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '>
	.fill 10,1,0

alt_map:
	.byte 0,0
	.ascii "\0~#{[|`\\^@]}"
	.byte 0,0
	.byte '@,0,0,0,0,0,0,0,0,0,0
	.byte '~,13,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte 0,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte 0,0,0,0		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte 0,0,0,0,0		/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '|
	.fill 10,1,0

#else
#error "KBD-type not defined"
#endif
/*
 * do_self handles "normal" keys, ie keys that don't change meaning
 * and which have just one character returns.
 */
 // 处理“普通”键，也即含义没有变化并且只有一个字符返回的键
do_self:
	lea alt_map,%ebx  // 将映射表基址 alt_map -> ebx
	testb $0x20,mode		/* alt-gr */
	jne 1f
	lea shift_map,%ebx
	testb $0x03,mode
	jne 1f
	lea key_map,%ebx
1:	movb (%ebx,%eax),%al
	orb %al,%al
	je none
	testb $0x4c,mode		/* ctrl or caps */
	je 2f
	cmpb $'a,%al
	jb 2f
	cmpb $'},%al
	ja 2f
	subb $32,%al
2:	testb $0x0c,mode		/* ctrl */
	je 3f
	cmpb $64,%al
	jb 3f
	cmpb $64+32,%al
	jae 3f
	subb $64,%al
3:	testb $0x10,mode		/* left alt */
	je 4f
	orb $0x80,%al
4:	andl $0xff,%eax
	xorl %ebx,%ebx
	call put_queue
none:	ret

/*
 * minus has a routine of it's own, as a 'E0h' before
 * the scan code for minus means that the numeric keypad
 * slash was pushed.
 */
minus:	cmpb $1,e0
	jne do_self
	movl $'/,%eax
	xorl %ebx,%ebx
	jmp put_queue

/*
 * This table decides which routine to call when a scan-code has been
 * gotten. Most routines just call do_self, or none, depending if
 * they are make or break.
 */
key_table:
	.long none,do_self,do_self,do_self	/* 00-03 s0 esc 1 2 */
	.long do_self,do_self,do_self,do_self	/* 04-07 3 4 5 6 */
	.long do_self,do_self,do_self,do_self	/* 08-0B 7 8 9 0 */
	.long do_self,do_self,do_self,do_self	/* 0C-0F + ' bs tab */
	.long do_self,do_self,do_self,do_self	/* 10-13 q w e r */
	.long do_self,do_self,do_self,do_self	/* 14-17 t y u i */
	.long do_self,do_self,do_self,do_self	/* 18-1B o p } ^ */
	.long do_self,ctrl,do_self,do_self	/* 1C-1F enter ctrl a s */
	.long do_self,do_self,do_self,do_self	/* 20-23 d f g h */
	.long do_self,do_self,do_self,do_self	/* 24-27 j k l | */
	.long do_self,do_self,lshift,do_self	/* 28-2B { para lshift , */
	.long do_self,do_self,do_self,do_self	/* 2C-2F z x c v */
	.long do_self,do_self,do_self,do_self	/* 30-33 b n m , */
	.long do_self,minus,rshift,do_self	/* 34-37 . - rshift * */
	.long alt,do_self,caps,func		/* 38-3B alt sp caps f1 */
	.long func,func,func,func		/* 3C-3F f2 f3 f4 f5 */
	.long func,func,func,func		/* 40-43 f6 f7 f8 f9 */
	.long func,num,scroll,cursor		/* 44-47 f10 num scr home */
	.long cursor,cursor,do_self,cursor	/* 48-4B up pgup - left */
	.long cursor,cursor,do_self,cursor	/* 4C-4F n5 right + end */
	.long cursor,cursor,cursor,cursor	/* 50-53 dn pgdn ins del */
	.long none,none,do_self,func		/* 54-57 sysreq ? < f11 */
	.long func,none,none,none		/* 58-5B f12 ? ? ? */
	.long none,none,none,none		/* 5C-5F ? ? ? ? */
	.long none,none,none,none		/* 60-63 ? ? ? ? */
	.long none,none,none,none		/* 64-67 ? ? ? ? */
	.long none,none,none,none		/* 68-6B ? ? ? ? */
	.long none,none,none,none		/* 6C-6F ? ? ? ? */
	.long none,none,none,none		/* 70-73 ? ? ? ? */
	.long none,none,none,none		/* 74-77 ? ? ? ? */
	.long none,none,none,none		/* 78-7B ? ? ? ? */
	.long none,none,none,none		/* 7C-7F ? ? ? ? */
	.long none,none,none,none		/* 80-83 ? br br br */
	.long none,none,none,none		/* 84-87 br br br br */
	.long none,none,none,none		/* 88-8B br br br br */
	.long none,none,none,none		/* 8C-8F br br br br */
	.long none,none,none,none		/* 90-93 br br br br */
	.long none,none,none,none		/* 94-97 br br br br */
	.long none,none,none,none		/* 98-9B br br br br */
	.long none,unctrl,none,none		/* 9C-9F br unctrl br br */
	.long none,none,none,none		/* A0-A3 br br br br */
	.long none,none,none,none		/* A4-A7 br br br br */
	.long none,none,unlshift,none		/* A8-AB br br unlshift br */
	.long none,none,none,none		/* AC-AF br br br br */
	.long none,none,none,none		/* B0-B3 br br br br */
	.long none,none,unrshift,none		/* B4-B7 br br unrshift br */
	.long unalt,none,uncaps,none		/* B8-BB unalt br uncaps br */
	.long none,none,none,none		/* BC-BF br br br br */
	.long none,none,none,none		/* C0-C3 br br br br */
	.long none,none,none,none		/* C4-C7 br br br br */
	.long none,none,none,none		/* C8-CB br br br br */
	.long none,none,none,none		/* CC-CF br br br br */
	.long none,none,none,none		/* D0-D3 br br br br */
	.long none,none,none,none		/* D4-D7 br br br br */
	.long none,none,none,none		/* D8-DB br ? ? ? */
	.long none,none,none,none		/* DC-DF ? ? ? ? */
	.long none,none,none,none		/* E0-E3 e0 e1 ? ? */
	.long none,none,none,none		/* E4-E7 ? ? ? ? */
	.long none,none,none,none		/* E8-EB ? ? ? ? */
	.long none,none,none,none		/* EC-EF ? ? ? ? */
	.long none,none,none,none		/* F0-F3 ? ? ? ? */
	.long none,none,none,none		/* F4-F7 ? ? ? ? */
	.long none,none,none,none		/* F8-FB ? ? ? ? */
	.long none,none,none,none		/* FC-FF ? ? ? ? */

/*
 * kb_wait waits for the keyboard controller buffer to empty.
 * there is no timeout - if the buffer doesn't empty, we hang.
 */
kb_wait:
	pushl %eax
1:	inb $0x64,%al    // 读取键盘控制器状态
	testb $0x02,%al  // 测试输入缓冲器是否为空
	jne 1b
	popl %eax
	ret
/*
 * This routine reboots the machine by asking the keyboard
 * controller to pulse the reset-line low.
 */
reboot:
	call kb_wait
	movw $0x1234,0x472	/* don't do memory check */
	movb $0xfc,%al		/* pulse reset and A20 low */
	outb %al,$0x64
die:	jmp die
