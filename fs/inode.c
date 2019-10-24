/*
 *  linux/fs/inode.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/system.h>

extern int *blk_size[];

struct m_inode inode_table[NR_INODE]={{0,},};

static void read_inode(struct m_inode * inode);
static void write_inode(struct m_inode * inode);

static inline void wait_on_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	sti();
}

static inline void lock_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	inode->i_lock=1;
	sti();
}

static inline void unlock_inode(struct m_inode * inode)
{
	inode->i_lock=0;
	wake_up(&inode->i_wait);
}

void invalidate_inodes(int dev)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);  // 等待该 i 节点可用（解锁）
		if (inode->i_dev == dev) {
			if (inode->i_count)
				printk("inode in use on removed disk\n\r");
			inode->i_dev = inode->i_dirt = 0;
		}
	}
}

// 同步内存与设备上的所有 i 节点信息
void sync_inodes(void)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		// 如果该 i 节点已修改且不是管道节点
		if (inode->i_dirt && !inode->i_pipe)
			write_inode(inode);  // 写盘
	}
}

// block -- 文件数据块，取值 0-(7+512+512*512-1)
// 返回文件数据块号 block 在设备上对应的逻辑块号（盘块号）
static int _bmap(struct m_inode * inode,int block,int create)
{
	struct buffer_head * bh;
	int i;

	if (block<0)
		panic("_bmap: block<0");
	if (block >= 7+512+512*512)
		panic("_bmap: block>big");
	if (block<7) {
		if (create && !inode->i_zone[block])
			// 为文件申请空余盘块，该盘块必须要有对应的缓冲头结构，
			// 所以如果没有空闲的缓冲块，则陷入等待。
			if (inode->i_zone[block]=new_block(inode->i_dev)) {
				inode->i_ctime=CURRENT_TIME;
				inode->i_dirt=1;
			}
		return inode->i_zone[block];
	}
	block -= 7;
	if (block<512) {
		if (create && !inode->i_zone[7])
			if (inode->i_zone[7]=new_block(inode->i_dev)) {
				inode->i_dirt=1;
				inode->i_ctime=CURRENT_TIME;
			}
		if (!inode->i_zone[7])
			return 0;
		if (!(bh = bread(inode->i_dev,inode->i_zone[7])))
			return 0;
		i = ((unsigned short *) (bh->b_data))[block]; // 盘号用 unsigned short 来描述
		if (create && !i)
			if (i=new_block(inode->i_dev)) {
				((unsigned short *) (bh->b_data))[block]=i;
				bh->b_dirt=1;
			}
		brelse(bh);
		return i;
	}
	block -= 512; // 到这里，block 取值 0-(512*512-1)
	if (create && !inode->i_zone[8])
		if (inode->i_zone[8]=new_block(inode->i_dev)) {
			inode->i_dirt=1;
			inode->i_ctime=CURRENT_TIME;
		}
	if (!inode->i_zone[8])
		return 0;
	if (!(bh=bread(inode->i_dev,inode->i_zone[8])))
		return 0;
	i = ((unsigned short *)bh->b_data)[block>>9]; // >>9 即除以 512，使得 block 在 0-511
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block>>9]=i;  
			bh->b_dirt=1;
		}
	brelse(bh);
	if (!i)
		return 0;
	if (!(bh=bread(inode->i_dev,i)))
		return 0;
	i = ((unsigned short *)bh->b_data)[block&511]; // &511 使得 block 在 0-511
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block&511]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	return i;
}

// 根据 i 节点信息取文件数据块 block 在设备上对应的逻辑块号
int bmap(struct m_inode * inode,int block)
{
	return _bmap(inode,block,0);
}

// 创建文件数据块 block 在设备上对应的逻辑块
// 并返回设备上对应的逻辑块号
int create_block(struct m_inode * inode, int block)
{
	return _bmap(inode,block,1);
}

// 释放一个 i 节点（回写入设备）
void iput(struct m_inode * inode)
{
	if (!inode)
		return;
	wait_on_inode(inode);
	if (!inode->i_count)
		panic("iput: trying to free free inode");
	// 如果是管道 i 节点，则唤醒等待该管道的进程，引用计数减 1，
	// 如果还有引用则返回
	if (inode->i_pipe) {
		wake_up(&inode->i_wait);
		wake_up(&inode->i_wait2);
		if (--inode->i_count)
			return;
		// 对于 pipe 节点，inode->i_size 存放着物理内存页地址
		free_page(inode->i_size);
		inode->i_count=0;
		inode->i_dirt=0;
		inode->i_pipe=0;
		return;
	}
	if (!inode->i_dev) {
		inode->i_count--;
		return;
	}
	// 如果是块设备文件的 i 节点，此时文件数据块 0 中是设备号，
	// 则刷新该设备，并等待 i 节点解锁
	if (S_ISBLK(inode->i_mode)) {
		sync_dev(inode->i_zone[0]);
		wait_on_inode(inode);
	}
repeat:
	if (inode->i_count>1) {
		inode->i_count--;
		return;
	}
	// 如果 i 节点的链接数为 0，则释放该 i 节点
	// 的所有逻辑块，并释放该 i 节点。
	if (!inode->i_nlinks) {
		truncate(inode);
		free_inode(inode);
		return;
	}
	if (inode->i_dirt) {
		write_inode(inode);	/* we can sleep - so do again */
		wait_on_inode(inode);
		goto repeat;
	}
	inode->i_count--;
	return;
}

// 从 i 节点表中获取一个空闲 i 节点项
// 寻找引用计数 count 为 0 的 i 节点，并将其写盘后清零，返回其指针
struct m_inode * get_empty_inode(void)
{
	struct m_inode * inode;
	static struct m_inode * last_inode = inode_table;
	int i;

	do {
		inode = NULL;
		for (i = NR_INODE; i ; i--) {
			if (++last_inode >= inode_table + NR_INODE)
				last_inode = inode_table;
			if (!last_inode->i_count) {
				inode = last_inode;
				if (!inode->i_dirt && !inode->i_lock)
					break;
			}
		}
		if (!inode) {
			for (i=0 ; i<NR_INODE ; i++)
				printk("%04x: %6d\t",inode_table[i].i_dev,
					inode_table[i].i_num);
			panic("No free inodes in mem");
		}
		wait_on_inode(inode);
		while (inode->i_dirt) {
			write_inode(inode);
			wait_on_inode(inode);
		}
	} while (inode->i_count);
	memset(inode,0,sizeof(*inode));
	inode->i_count = 1;
	return inode;
}

struct m_inode * get_pipe_inode(void)
{
	struct m_inode * inode;

	if (!(inode = get_empty_inode()))
		return NULL;
	// 节点的 i_size 字段指向缓冲块
	if (!(inode->i_size=get_free_page())) { 
		inode->i_count = 0;  // 如果已经没有空闲缓冲块，则释放该 i 节点
		return NULL;
	}
	inode->i_count = 2;	/* sum of readers/writers */
	PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;
	inode->i_pipe = 1;
	return inode;
}

// 从设备上读取指定节点号的 i 节点
// nr -- i 节点号
struct m_inode * iget(int dev,int nr)
{
	struct m_inode * inode, * empty;

	if (!dev)
		panic("iget with dev==0");
	empty = get_empty_inode();
	inode = inode_table;
	while (inode < NR_INODE+inode_table) {
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode++;
			continue;
		}
		wait_on_inode(inode);
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode = inode_table;
			continue;
		}
		inode->i_count++;
		// 如果该 i 节点是某个文件系统的挂载点
		// 则将 nr 设置为 ROOT_INO，即变成了寻找并返回该文件系统的根节点
		if (inode->i_mount) {
			int i;

			for (i = 0 ; i<NR_SUPER ; i++)
				if (super_block[i].s_imount==inode)
					break;
			if (i >= NR_SUPER) {
				printk("Mounted inode hasn't got sb\n");
				if (empty)
					iput(empty);
				return inode;
			}
			iput(inode);  // 将该挂载点的 i 节点信息写盘，并释放内存中对应的 i 节点，即 inode
			dev = super_block[i].s_dev;
			nr = ROOT_INO; // 寻找该文件系统的根节点
						   // 对于根文件系统，其挂载点的 i 节点也是它的根 i 节点
			inode = inode_table;
			continue;
		}
		if (empty)
			iput(empty);
		return inode;
	}
	if (!empty)
		return (NULL);
	inode=empty;
	inode->i_dev = dev;
	inode->i_num = nr;
	read_inode(inode);
	return inode;
}

// 从指定设备上读取指定 i 节点的信息到内存中（缓冲区中）
// 即初始化 m_inode 的 dev 和 i_num 成员，然后传入去获取其他成员信息
// m_inode -- memory inode; d_inode -- disk inode
static void read_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to read inode without dev");
	// minix 硬盘格式：引导块+超级块+i节点位图块+盘块位图块+i节点块+盘块
	// 这些块的大小都是 1024 byte，有的块不只一块
	// 计算该 i 节点所在的逻辑块号 block
	// 在一个硬盘中，引导块 1 块，超级块 1 块，引导块是第 0 块
	// INODES_PER_BLOCK 是一个i节点块中（1024byte）能存放的 struct d_inode 个数
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	*(struct d_inode *)inode =
		((struct d_inode *)bh->b_data)
			[(inode->i_num-1)%INODES_PER_BLOCK];
	brelse(bh);
	if (S_ISBLK(inode->i_mode)) {
		int i = inode->i_zone[0];
		if (blk_size[MAJOR(i)])
			inode->i_size = 1024*blk_size[MAJOR(i)][MINOR(i)];
		else
			inode->i_size = 0x7fffffff;
	}
	unlock_inode(inode);
}

// 将指定 i 节点信息写入设备
// (写入缓冲区相应的缓冲块中，待缓冲区刷新时会写入盘中)
// 该缓冲块对应硬盘中的 i 节点块
static void write_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	if (!inode->i_dirt || !inode->i_dev) {
		unlock_inode(inode);
		return;
	}
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to write inode without device");
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	((struct d_inode *)bh->b_data)
		[(inode->i_num-1)%INODES_PER_BLOCK] =
			*(struct d_inode *)inode;  // inode => bh->d_data
	bh->b_dirt=1;
	inode->i_dirt=0;
	brelse(bh);
	unlock_inode(inode);
}
