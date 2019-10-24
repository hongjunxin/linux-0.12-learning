/*
 *  linux/kernel/hd.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * This is the low-level hd interrupt support. It traverses the
 * request-list, using interrupts to jump between functions. As
 * all the functions are called within interrupts, we may not
 * sleep. Special care is recommended.
 * 
 *  modified by Drew Eckhardt to check nr of hd's from the CMOS.
 */

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/hdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#define MAJOR_NR 3
#include "blk.h"

// 读 CMOS 参数宏函数
#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

/* Max read/write errors/sector */
#define MAX_ERRORS	7	// 读/写一个扇区时允许的最多出错次数
#define MAX_HD		2	// 系统支持的最多硬盘数

static void recal_intr(void);	// 硬盘中断程序在复位操作时会调用的重新校正函数
static void bad_rw_intr(void);

static int recalibrate = 0;  // 重新校正标志，将磁头移动到 0 柱面
static int reset = 0;  // 复位标志。当发生读写错误时会设置该标志，以复位硬盘和控制器

/*
 *  This struct defines the HD's and their types.
 */
// 各自段分别是磁头数、每磁道扇区数、柱面数、写前预补偿柱面号、
// 磁头着陆区柱面号、控制字节 
struct hd_i_struct {
	int head,sect,cyl,wpcom,lzone,ctl;
	};
#ifdef HD_TYPE
struct hd_i_struct hd_info[] = { HD_TYPE };
#define NR_HD ((sizeof (hd_info))/(sizeof (struct hd_i_struct)))
#else
struct hd_i_struct hd_info[] = { {0,0,0,0,0,0},{0,0,0,0,0,0} };
static int NR_HD = 0;
#endif

// 定义硬盘分区结构。给出每个分区的物理起始扇区号、分区扇区总数
static struct hd_struct {
	long start_sect;	// 起始扇区号
	long nr_sects;		// 扇区总数
} hd[5*MAX_HD]={{0,0},};

static int hd_sizes[5*MAX_HD] = {0, };

// 读端口 port，共读 nr 字，保存在 buf 中
#define port_read(port,buf,nr) \
__asm__("cld;rep;insw"::"d" (port),"D" (buf),"c" (nr):"cx","di")

// 写端口 port，共写 nr 字，从 buf 中取数据
#define port_write(port,buf,nr) \
__asm__("cld;rep;outsw"::"d" (port),"S" (buf),"c" (nr):"cx","si")

extern void hd_interrupt(void);  // 硬盘中断过程
extern void rd_load(void);	// 虚拟盘创建加载函数

/* This may be used only once, enforced by 'static int callable' */
/* 下面该函数只在初始化时被调用一次。用静态变量 callable 作为可调用标志。*/
// 该函数的参数由初始化程序init/main.c 的 init子程序设置为指向 0x90080 处，此处存放着 setup.s
// 程序从 BIOS 取得的 2 个硬盘的基本参数表(32 字节)。
// 本函数主要功能是读取 CMOS 和硬盘参数表信息，用于设置硬盘分区结构 hd，并加载 RAM 虚拟盘和
// 根文件系统。
int sys_setup(void * BIOS)
{
	static int callable = 1;
	int i,drive;
	unsigned char cmos_disks;
	struct partition *p;
	struct buffer_head * bh;

	if (!callable)
		return -1;
	callable = 0;
// 如果没有在 config.h 中定义硬盘参数，就从 0x90080 处读入。
#ifndef HD_TYPE
	for (drive=0 ; drive<2 ; drive++) {
		hd_info[drive].cyl = *(unsigned short *) BIOS;
		hd_info[drive].head = *(unsigned char *) (2+BIOS);
		hd_info[drive].wpcom = *(unsigned short *) (5+BIOS);
		hd_info[drive].ctl = *(unsigned char *) (8+BIOS);
		hd_info[drive].lzone = *(unsigned short *) (12+BIOS);
		hd_info[drive].sect = *(unsigned char *) (14+BIOS);
		BIOS += 16;
	}
	if (hd_info[1].cyl)
		NR_HD=2;
	else
		NR_HD=1;
#endif
	for (i=0 ; i<NR_HD ; i++) {
		hd[i*5].start_sect = 0;
		hd[i*5].nr_sects = hd_info[i].head*
				hd_info[i].sect*hd_info[i].cyl;
	}

	/*
		We querry CMOS about hard disks : it could be that 
		we have a SCSI/ESDI/etc controller that is BIOS
		compatable with ST-506, and thus showing up in our
		BIOS table, but not register compatable, and therefore
		not present in CMOS.

		Furthurmore, we will assume that our ST-506 drives
		<if any> are the primary drives in the system, and 
		the ones reflected as drive 1 or 2.

		The first drive is stored in the high nibble of CMOS
		byte 0x12, the second in the low nibble.  This will be
		either a 4 bit drive type or 0xf indicating use byte 0x19 
		for an 8 bit type, drive 1, 0x1a for drive 2 in CMOS.

		Needless to say, a non-zero value means we have 
		an AT controller hard disk for that drive.

		
	*/

    /*      
	 * 我们对 CMOS 有关硬盘的信息有些怀疑：可能会出现这样的情况，我们有一块 SCSI/ESDI/等的      
	 * 控制器，它是以 ST-506 方式与 BIOS兼容的，因而会出现在我们的 BIOS 参数表中，但却又不      
	 * 是寄存器兼容的，因此这些参数在 CMOS 中又不存在。      
	 * 另外，我们假设 ST-506驱动器（如果有的话）是系统中的基本驱动器，也即以驱动器 1 或 2      
	 * 出现的驱动器。      
	 * 第 1 个驱动器参数存放在 CMOS 字节 0x12 的高半字节中，第 2 个存放在低半字节中。该 4 位字节      
	 * 信息可以是驱动器类型，也可能仅是 0xf。0xf 表示使用 CMOS 中 0x19 字节作为驱动器 1 的 8 位      
	 * 类型字节，使用 CMOS 中 0x1A 字节作为驱动器 2 的类型字节。      
	 * 总之，一个非零值意味着我们有一个 AT 控制器硬盘兼容的驱动器。      
	 */

	if ((cmos_disks = CMOS_READ(0x12)) & 0xf0)
		if (cmos_disks & 0x0f)
			NR_HD = 2;
		else
			NR_HD = 1;
	else
		NR_HD = 0;
	for (i = NR_HD ; i < 2 ; i++) {
		hd[i*5].start_sect = 0;
		hd[i*5].nr_sects = 0;
	}
	
	// 读取每一个硬盘上第 1 块数据（第 1 个扇区有用），获取其中的分区表信息。  
	// 首先利用函数bread()读硬盘第 1 块数据(fs/buffer.c)，参数中的 0x300 是硬盘的主设备号
	// 然后根据硬盘头 1 个扇区位置 0x1fe 处的两个字节是否为'55AA'来判断
	// 该扇区中位于 0x1BE 开始的分区表是否有效。最后将分区表信息放入硬盘分区数据结构 hd 中。
	for (drive=0 ; drive<NR_HD ; drive++) {
		if (!(bh = bread(0x300 + drive*5,0))) {
			printk("Unable to read partition table of drive %d\n\r",
				drive);
			panic("");
		}
		if (bh->b_data[510] != 0x55 || (unsigned char)
		    bh->b_data[511] != 0xAA) {
			printk("Bad partition table on drive %d\n\r",drive);
			panic("");
		}
		p = 0x1BE + (void *)bh->b_data; // 分区表位于硬盘第 1 扇区的 0x1BE 处。
		for (i=1;i<5;i++,p++) {		// 一个磁盘只支持四个逻辑分区
			hd[i+5*drive].start_sect = p->start_sect;
			hd[i+5*drive].nr_sects = p->nr_sects;
		}
		brelse(bh);  // 释放为存放硬盘块而申请的内存缓冲区页
	}
	for (i=0 ; i<5*MAX_HD ; i++)
		hd_sizes[i] = hd[i].nr_sects>>1 ;
	blk_size[MAJOR_NR] = hd_sizes;
	if (NR_HD)
		printk("Partition table%s ok.\n\r",(NR_HD>1)?"s":"");
	rd_load();  // 加载（创建）RAMDISK(kernel/blk_drv/ramdisk.c)
	init_swapping();
	mount_root();  // 安装根文件系统(fs/super.c)
	return (0);
}

// 判断并循环等待驱动器就绪。     
// 读硬盘控制器状态寄存器端口 HD_STATUS(0x1f7)，
// 并循环检测驱动器就绪比特位和控制器忙位。
static int controller_ready(void)
{
	int retries = 100000;

	while (--retries && (inb_p(HD_STATUS)&0xc0)!=0x40);
	return (retries);
}

static int win_result(void)
{
	int i=inb_p(HD_STATUS);  // 读取状态信息

	if ((i & (BUSY_STAT | READY_STAT | WRERR_STAT | SEEK_STAT | ERR_STAT))
		== (READY_STAT | SEEK_STAT))
		return(0); /* ok */
	if (i&1) i=inb(HD_ERROR);  // 倘若 ERR_STAT 置位，则读取错误寄存器
	return (1);
}

// driver 硬盘号; nsect 读写扇区数; sect 起始扇区;
// head 磁头号; cyl 柱面号; cmd 控制器命令码
// *intr_addr 硬盘中断发生时处理程序中将调用的 c 处理函数
static void hd_out(unsigned int drive,unsigned int nsect,unsigned int sect,
		unsigned int head,unsigned int cyl,unsigned int cmd,
		void (*intr_addr)(void))
{
	register int port asm("dx");  // port 变量对应寄存器 dx

	if (drive>1 || head>15)
		panic("Trying to write bad sector");
	if (!controller_ready())
		panic("HD controller not ready");
	SET_INTR(intr_addr);
	outb_p(hd_info[drive].ctl,HD_CMD); // 向控制寄存器(0x3f6)输出控制字节
	port=HD_DATA; // 置 dx为数据寄存器端口(0x1f0)
	outb_p(hd_info[drive].wpcom>>2,++port); // 参数：写预补偿柱面号(需除 4)
	outb_p(nsect,++port); // 参数：读/写扇区总数
	outb_p(sect,++port);  // 参数：起始扇区
	outb_p(cyl,++port);   // 参数：柱面号低 8 位
	outb_p(cyl>>8,++port); // 参数：柱面号高 8 位
	outb_p(0xA0|(drive<<4)|head,++port);  // 参数：驱动器号+磁头号
	outb(cmd,++port); // 命令：硬盘控制命令
}

// 等待硬盘就绪
static int drive_busy(void)
{
	unsigned int i;
	unsigned char c;

	for (i = 0; i < 50000; i++) {
		c = inb_p(HD_STATUS);
		c &= (BUSY_STAT | READY_STAT | SEEK_STAT);
		if (c == (READY_STAT | SEEK_STAT))
			return 0;
	}
	printk("HD controller times out\n\r");
	return(1);
}

static void reset_controller(void)
{
	int	i;

	outb(4,HD_CMD); // 向控制寄存器端口发送控制字节(4-复位）
	for(i = 0; i < 1000; i++) nop();
	outb(hd_info[0].ctl & 0x0f ,HD_CMD); // 再发送正常的控制字节(不禁止重试、重读)
	if (drive_busy())
		printk("HD-controller still busy\n\r");
	if ((i = inb(HD_ERROR)) != 1) // 取错误寄存器，若不等于 1（无错误）则出错
		printk("HD-controller reset failed: %02x\n\r",i);
}

static void reset_hd(void)
{
	static int i;

repeat:
	if (reset) {
		reset = 0;
		i = -1;
		reset_controller();
	} else if (win_result()) {
		bad_rw_intr();
		if (reset)
			goto repeat;
	}
	i++;
	if (i < NR_HD) {
		hd_out(i,hd_info[i].sect,hd_info[i].sect,hd_info[i].head-1,
			hd_info[i].cyl,WIN_SPECIFY,&reset_hd);
	} else
		do_hd_request();
}

void unexpected_hd_interrupt(void)
{
	printk("Unexpected HD interrupt\n\r");
	reset = 1;
	do_hd_request();
}

static void bad_rw_intr(void)
{
	if (++CURRENT->errors >= MAX_ERRORS)
		end_request(0);
	if (CURRENT->errors > MAX_ERRORS/2)
		reset = 1;
}

static void read_intr(void)
{
	if (win_result()) { // 若控制器忙、读写错或命令执行错
		bad_rw_intr();
		do_hd_request(); // 执行硬盘请求（复位处理）
		return;
	}
	port_read(HD_DATA,CURRENT->buffer,256);  // 将数据从数据寄存器口读到请求结构缓冲区
	CURRENT->errors = 0;
	CURRENT->buffer += 512;
	CURRENT->sector++;
	if (--CURRENT->nr_sectors) {
		SET_INTR(&read_intr);  // 再次置硬盘调用 C 函数指针为 read_intr()
		return;
	}
	end_request(1);
	do_hd_request();  // 执行其它硬盘请求操作
}

static void write_intr(void)
{
	if (win_result()) {
		bad_rw_intr();
		do_hd_request();
		return;
	}
	if (--CURRENT->nr_sectors) {
		CURRENT->sector++;
		CURRENT->buffer += 512;
		SET_INTR(&write_intr);
		port_write(HD_DATA,CURRENT->buffer,256);
		return;
	}
	end_request(1);
	do_hd_request();
}

static void recal_intr(void)
{
	if (win_result())
		bad_rw_intr();
	do_hd_request();
}

void hd_times_out(void)
{
	if (!CURRENT)
		return;
	printk("HD timeout");
	if (++CURRENT->errors >= MAX_ERRORS)
		end_request(0);
	SET_INTR(NULL);
	reset = 1;
	do_hd_request();
}

void do_hd_request(void)
{
	int i,r;
	unsigned int block,dev;
	unsigned int sec,head,cyl;
	unsigned int nsect;

	INIT_REQUEST;
	dev = MINOR(CURRENT->dev); // 子设备号即是硬盘上的分区号
	block = CURRENT->sector;
	// 起始扇区不能大于该分区扇区数-2，因为一次要求读写 2 个扇区
	if (dev >= 5*NR_HD || block+2 > hd[dev].nr_sects) {
		end_request(0);
		goto repeat;  // 该标号在 blk.h 
	}
	block += hd[dev].start_sect;
	dev /= 5; // 结果是 0 或 1，所以代表的是硬盘号
	__asm__("divl %4":"=a" (block),"=d" (sec):"0" (block),"1" (0),
		"r" (hd_info[dev].sect));
	__asm__("divl %4":"=a" (cyl),"=d" (head):"0" (block),"1" (0),
		"r" (hd_info[dev].head));
	sec++;
	nsect = CURRENT->nr_sectors;
	if (reset) {
		recalibrate = 1;
		reset_hd();
		return;
	}
	if (recalibrate) {
		recalibrate = 0;
		hd_out(dev,hd_info[CURRENT_DEV].sect,0,0,0,
			WIN_RESTORE,&recal_intr);
		return;
	}	
	if (CURRENT->cmd == WRITE) {
		hd_out(dev,nsect,sec,head,cyl,WIN_WRITE,&write_intr);
		// 如果当前请求是写扇区操作，则发送写命令，
		// 循环读取状态寄存器信息并判断请求服务标志DRQ_STAT 是否置位。
		// DRQ_STAT 是硬盘状态寄存器的请求服务位，表示驱动器已经准备好在主机和
		// 数据端口之间传输一个字或一个字节的数据
		for(i=0 ; i<10000 && !(r=inb_p(HD_STATUS)&DRQ_STAT) ; i++)
			/* nothing */ ;
		if (!r) {
			bad_rw_intr();
			goto repeat;
		}
		port_write(HD_DATA,CURRENT->buffer,256);
	} else if (CURRENT->cmd == READ) {
		hd_out(dev,nsect,sec,head,cyl,WIN_READ,&read_intr);
	} else
		panic("unknown hd-command");
}

void hd_init(void)
{
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
	set_intr_gate(0x2E,&hd_interrupt); // 设置硬盘中断门向量 int 0x2E(46)
	outb_p(inb_p(0x21)&0xfb,0x21); // 复位接联的主8259A int2的屏蔽位，允许从片发出中断请求信号
	outb(inb_p(0xA1)&0xbf,0xA1); // 复位硬盘的中断请求屏蔽位（在从片上），允许硬盘控制器发送中断请求信号
} 
