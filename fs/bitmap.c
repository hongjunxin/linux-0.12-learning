/*
 *  linux/fs/bitmap.c
 *
 *  (C) 1991  Linus Torvalds
 */

/* bitmap.c contains the code that handles the inode and block bitmaps */
#include <string.h>

#include <linux/sched.h>
#include <linux/kernel.h>

// 输入：eax=0, ecx=(BLOCK_SIZE/4), edi=addr
#define clear_block(addr) \
__asm__("cld\n\t" \  // DF=0, 地址指针增加（清除方向）
	"rep\n\t" \  // 用在MOVS、STOS、LODS指令前，每次执行一次指令，CX减1；直到CX=0,重复执行结束
	"stosl" \    // 字串存储：eax -> edi, edi+4 -> edi
	::"a" (0),"c" (BLOCK_SIZE/4),"D" ((long) (addr)):"cx","di")

// 置位指定地址开始的第 nr 个位偏移处的比特位（nr 可以大于 32）
// 返回原比特位（0 或 1）
// 输入：%0-eax(返回值)
// %1-eax(0)
// %2-nr, 位偏移值
// %3-(addr), addr 的内容
// BTS -- Bit Test and Set
// btsl offset origin
// 先将指定位的值存储到 CF 标志中，然后将 offset 对应的位置 1
// setb D -- D <- CF
#define set_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btsl %2,%3\n\tsetb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

// 复位指定地址开始的第 nr 位偏移处的比特位
// 返回原比特位的反码（0 或 1）
// BTR -- Bit Test and Reset
// 先将指定位的值存储到 CF 标志中，然后将 offset 对应的位置 0
// setnb D -- D <- ~CF
#define clear_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btrl %2,%3\n\tsetnb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

// 从 addr 开始寻找第 1 个 0 值比特位
// 并将其距离 addr 的比特位偏移值返回
// %0 -- ecx(返回值)
// %1 -- ecx(0)
// %2 -- esi(addr) 
// LODS -- 串读取指令
#define find_first_zero(addr) ({ \
int __res; \
__asm__("cld\n" \				// DF=0  地址递增
	"1:\tlodsl\n\t" \			// eax <- esi, esi <- esi+4
	"notl %%eax\n\t" \          // eax 中每位取反
	"bsfl %%eax,%%edx\n\t" \    // 从位 0 开始扫描 eax，找到第一个是 1 的位，其偏移值 -> edx
	"je 2f\n\t" \				// 如果 eax 中全是 0 则跳转到标号 2
	"addl %%edx,%%ecx\n\t" \	// edx + ecx -> ecx
	"jmp 3f\n" \				// 结束
	"2:\taddl $32,%%ecx\n\t" \  // 没有找到 0 比特位, 则 ecx 需加上 32
	"cmpl $8192,%%ecx\n\t" \	// 已经扫描了 8192 位(1024 字节)了吗？
	"jl 1b\n" \					// 若还没扫描完 1 块数据，则继续
	"3:" \
	:"=c" (__res):"c" (0),"S" (addr):"ax","dx","si"); \
__res;})

// 释放设备 dev 上数据区中的逻辑块 block
// 复位指定逻辑块 block 的逻辑块位图比特位
// block -- 逻辑块号（盘块号）
int free_block(int dev, int block)
{
	struct super_block * sb;
	struct buffer_head * bh;

	if (!(sb = get_super(dev)))
		panic("trying to free block on nonexistent device");
	if (block < sb->s_firstdatazone || block >= sb->s_nzones)
		panic("trying to free block not in datazone");
	bh = get_hash_table(dev,block);
	if (bh) {
		if (bh->b_count > 1) {
			brelse(bh);
			return 0;
		}
		bh->b_dirt=0;
		bh->b_uptodate=0;
		if (bh->b_count)
			brelse(bh);
	}
	// 计算 block 在数据区开始算起的数据逻辑块号（从 1 开始计数）
	// 一个缓冲块有 1024 KB, 即 8192 个比特位，一个比特位可代表一个缓冲块
	// 所以可支持的最大块设备容量（长度）是 64MB
	// block&8191 得到的结果在 0-8191 之间
	block -= sb->s_firstdatazone - 1 ;
	if (clear_bit(block&8191,sb->s_zmap[block/8192]->b_data)) {
		printk("block (%04x:%d) ",dev,block+sb->s_firstdatazone-1);
		printk("free_block: bit already cleared\n");
	}
	sb->s_zmap[block/8192]->b_dirt = 1;
	return 1;
}

int new_block(int dev)
{
	struct buffer_head * bh;
	struct super_block * sb;
	int i,j;

	if (!(sb = get_super(dev)))
		panic("trying to get new block from nonexistant device");
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_zmap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (i>=8 || !bh || j>=8192)
		return 0;
	if (set_bit(j,bh->b_data))
		panic("new_block: bit already set");
	bh->b_dirt = 1;
	j += i*8192 + sb->s_firstdatazone-1;
	if (j >= sb->s_nzones)
		return 0;
	if (!(bh=getblk(dev,j)))
		panic("new_block: cannot get block");
	if (bh->b_count != 1)
		panic("new block: count is != 1");
	clear_block(bh->b_data);
	bh->b_uptodate = 1;
	bh->b_dirt = 1;
	brelse(bh);
	return j;
}

void free_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;

	if (!inode)
		return;
	if (!inode->i_dev) {
		memset(inode,0,sizeof(*inode));
		return;
	}
	if (inode->i_count>1) {
		printk("trying to free inode with count=%d\n",inode->i_count);
		panic("free_inode");
	}
	if (inode->i_nlinks)
		panic("trying to free inode with links");
	if (!(sb = get_super(inode->i_dev)))
		panic("trying to free inode on nonexistent device");
	if (inode->i_num < 1 || inode->i_num > sb->s_ninodes)
		panic("trying to free inode 0 or nonexistant inode");
	if (!(bh=sb->s_imap[inode->i_num>>13]))  // >>13 即除以 65536
		panic("nonexistent imap in superblock");
	if (clear_bit(inode->i_num&8191,bh->b_data))
		printk("free_inode: bit already cleared.\n\r");
	bh->b_dirt = 1;
	memset(inode,0,sizeof(*inode));
}

struct m_inode * new_inode(int dev)
{
	struct m_inode * inode;
	struct super_block * sb;
	struct buffer_head * bh;
	int i,j;

	if (!(inode=get_empty_inode()))
		return NULL;
	if (!(sb = get_super(dev)))
		panic("new_inode with unknown device");
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_imap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (!bh || j >= 8192 || j+i*8192 > sb->s_ninodes) {
		iput(inode);
		return NULL;
	}
	if (set_bit(j,bh->b_data))
		panic("new_inode: bit already set");
	bh->b_dirt = 1;
	inode->i_count=1;
	inode->i_nlinks=1;
	inode->i_dev=dev;
	inode->i_uid=current->euid;
	inode->i_gid=current->egid;
	inode->i_dirt=1;
	inode->i_num = j + i*8192;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	return inode;
}
