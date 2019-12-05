/*
 *  linux/mm/memory.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * demand-loading started 01.12.91 - seems it is high on the list of
 * things wanted, and it should be easy to implement. - Linus
 */

/*
 * Ok, demand-loading was easy, shared pages a little bit tricker. Shared
 * pages started 02.12.91, seems to work. - Linus.
 *
 * Tested sharing by executing about 30 /bin/sh: under the old kernel it
 * would have taken more than the 6M I have free, but it worked well as
 * far as I could see.
 *
 * Also corrected some "invalidate()"s - I wasn't doing enough of them.
 */

/*
 * Real VM (paging to/from disk) started 18.12.91. Much more work and
 * thought has to go into this. Oh, well..
 * 19.12.91  -  works, somewhat. Sometimes I get faults, don't know why.
 *		Found it. Everything seems to work now.
 * 20.12.91  -  Ok, making the swap-device changeable like the root.
 */

#include <signal.h>

#include <asm/system.h>

#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>

#define CODE_SPACE(addr) ((((addr)+4095)&~4095) < \
current->start_code + current->end_code)

unsigned long HIGH_MEMORY = 0;

#define copy_page(from,to) \
__asm__("cld ; rep ; movsl"::"S" (from),"D" (to),"c" (1024):"cx","di","si")

// 一个数组元素对应一页内存
unsigned char mem_map [ PAGING_PAGES ] = {0,};

/*
 * Free a page of memory at physical address 'addr'. Used by
 * 'free_page_tables()'
 */
// addr -- 物理内存页的物理地址
void free_page(unsigned long addr)
{
	if (addr < LOW_MEM) return;
	if (addr >= HIGH_MEMORY)
		panic("trying to free nonexistent page");
	addr -= LOW_MEM;
	addr >>= 12;  // 除以4096
	if (mem_map[addr]--) return;
	mem_map[addr]=0;
	panic("trying to free free page");
}

/*
 * This function frees a continuos block of page tables, as needed
 * by 'exit()'. As does copy_page_tables(), this handles only 4Mb blocks.
 */
// from -- 线性地址
// size -- 释放的长度，单位是字节
int free_page_tables(unsigned long from,unsigned long size)
{
	unsigned long *pg_table;
	unsigned long * dir, nr;

	// from 该线性地址的后22个比特位必须都是0
	// 一个页表有1024项，每项对应一个物理页，一个物理页长度4KB，
	// 所以 from 要4MB对齐
	if (from & 0x3fffff)
		panic("free_page_tables called with wrong alignment");
	if (!from)
		panic("Trying to free up swapper memory space");
	// 该函数只处理4MB的内存块，所以要把待释放的字节数换算为待释放
	// 的页目录项，即要释放多少个页目录项。所以即使要释放的字节数仅有
	// 1个字节，换算后就要释放4MB的内存块
	size = (size + 0x3fffff) >> 22;
	// 页目录表的物理地址范围是 0-4095 之间，每个目录项长度为4字节，
	// 所以页目录项的合法地址是 0-4092(0xffc)，并且要4字节对齐
	dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	for ( ; size-->0 ; dir++) {
		if (!(1 & *dir))  // 如果该页目录项无效(P位=0)，表明没有对应的页表
			continue;
		pg_table = (unsigned long *) (0xfffff000 & *dir); // 取页表地址
		for (nr=0 ; nr<1024 ; nr++) {
			if (*pg_table) {
				if (1 & *pg_table)  // 页表项是否有效
					free_page(0xfffff000 & *pg_table);
				else
					swap_free(*pg_table >> 1);
				*pg_table = 0;
			}
			pg_table++;
		}
		free_page(0xfffff000 & *dir); // 页表物理地址在1MB以内，所以这句话没实际作用？
		*dir = 0; // 页目录项清零
	}
	invalidate();
	return 0;
}

/*
 *  Well, here is one of the most complicated functions in mm. It
 * copies a range of linerar addresses by copying only the pages.
 * Let's hope this is bug-free, 'cause this one I don't want to debug :-)
 *
 * Note! We don't copy just any chunks of memory - addresses have to
 * be divisible by 4Mb (one page-directory entry), as this makes the
 * function easier. It's used only by fork anyway.
 *
 * NOTE 2!! When from==0 we are copying kernel space for the first
 * fork(). Then we DONT want to copy a full page-directory entry, as
 * that would lead to some serious memory waste - we just copy the
 * first 160 pages - 640kB. Even that is more than we need, but it
 * doesn't take any more memory - we don't copy-on-write in the low
 * 1 Mb-range, so the pages can be shared with the kernel. Thus the
 * special case for nr=xxxx.
 */
// from to -- 线性地址
// size -- 要复制的长度，单位是字节 
int copy_page_tables(unsigned long from,unsigned long to,long size)
{
	unsigned long * from_page_table;
	unsigned long * to_page_table;
	unsigned long this_page;
	unsigned long * from_dir, * to_dir;
	unsigned long new_page;
	unsigned long nr;

	if ((from&0x3fffff) || (to&0x3fffff))
		panic("copy_page_tables called with wrong alignment");
	/*
	   对于32位的线性地址，高10位是页目录项的索引值，即from>>22，但因为页目录表
	   的物理基地址是0，并且每项页目录项的大小是4个字节，所以from>>2，再<<2(乘以4)
	   将得到该页目录项的地址，该地址是物理地址，因为内核态数据段的基地址是0
	*/
	from_dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	to_dir = (unsigned long *) ((to>>20) & 0xffc);
	// 将要复制的字节数转换为要多少个4MB块，比如一个进程的段限长是64MB，则需要16个页表
	size = ((unsigned) (size+0x3fffff)) >> 22;
	for( ; size-->0 ; from_dir++,to_dir++) {
		if (1 & *to_dir)
			panic("copy_page_tables: already exist");
		if (!(1 & *from_dir))
			continue;
		from_page_table = (unsigned long *) (0xfffff000 & *from_dir);
		// 为新进程申请1个页表，可存放4MB的数据(1024x4KB=4MB)
		if (!(to_page_table = (unsigned long *) get_free_page()))
			return -1;	/* Out of memory, see freeing */
		*to_dir = ((unsigned long) to_page_table) | 7; // 7是标志信息，(usr,RW,present)
		nr = (from==0)?0xA0:1024;  // 如果复制源是内核的代码段/数据段，则只复制160(0xA0)个内存页面
		for ( ; nr-- > 0 ; from_page_table++,to_page_table++) {
			this_page = *from_page_table;
			if (!this_page)
				continue;
			if (!(1 & this_page)) {
				if (!(new_page = get_free_page()))
					return -1;
				read_swap_page(this_page>>1, (char *) new_page);
				*to_page_table = this_page; // 当新进程需要用到该页面时，自己再去将数据从硬盘加载到内存？
				*from_page_table = new_page | (PAGE_DIRTY | 7);
				continue;
			}
			this_page &= ~2;  // 共享的内存页面被设为只读
			*to_page_table = this_page;
			if (this_page > LOW_MEM) {
				*from_page_table = this_page;
				this_page -= LOW_MEM;
				this_page >>= 12;
				mem_map[this_page]++;  // 被共享的内存页面，其引用计数+1
			}
		}
	}
	invalidate();
	return 0;
}

/*
 * This function puts a page in memory at the wanted address.
 * It returns the physical address of the page gotten, 0 if
 * out of memory (either when trying to access page-table or
 * page.)
 */
// 把一物理内存页映射到指定的线性地址，即修改线性地址换算后的
// 页目录和页表中的信息
// page -- 内存页的物理地址
// address -- 线性地址 
static unsigned long put_page(unsigned long page,unsigned long address)
{
	unsigned long tmp, *page_table;

/* NOTE !!! This uses the fact that _pg_dir=0 */

	if (page < LOW_MEM || page >= HIGH_MEMORY)
		printk("Trying to put page %p at %p\n",page,address);
	if (mem_map[(page-LOW_MEM)>>12] != 1)
		printk("mem_map disagrees with %p at %p\n",page,address);
	page_table = (unsigned long *) ((address>>20) & 0xffc);
	if ((*page_table)&1)
		page_table = (unsigned long *) (0xfffff000 & *page_table);
	else {
		if (!(tmp=get_free_page()))
			return 0;
		*page_table = tmp | 7;
		page_table = (unsigned long *) tmp;
	}
	page_table[(address>>12) & 0x3ff] = page | 7;
/* no need for invalidate */
	return page;
}

/*
 * The previous function doesn't work very well if you also want to mark
 * the page dirty: exec.c wants this, as it has earlier changed the page,
 * and we want the dirty-status to be correct (for VM). Thus the same
 * routine, but this time we mark it dirty too.
 */
unsigned long put_dirty_page(unsigned long page, unsigned long address)
{
	unsigned long tmp, *page_table;

/* NOTE !!! This uses the fact that _pg_dir=0 */

	if (page < LOW_MEM || page >= HIGH_MEMORY)
		printk("Trying to put page %p at %p\n",page,address);
	if (mem_map[(page-LOW_MEM)>>12] != 1)
		printk("mem_map disagrees with %p at %p\n",page,address);
	page_table = (unsigned long *) ((address>>20) & 0xffc);  /* 页目录项地址 */
	if ((*page_table)&1)
		page_table = (unsigned long *) (0xfffff000 & *page_table);  /* 页表地址 */
	else {
		if (!(tmp=get_free_page()))
			return 0;
		*page_table = tmp|7;  // 设置页目录项内容(页表地址+标志位)
		page_table = (unsigned long *) tmp;
	}
	page_table[(address>>12) & 0x3ff] = page | (PAGE_DIRTY | 7);
/* no need for invalidate */
	return page;
}

// 取消写保护页面，用于页异常中断过程中写保护异常的处理（写时复制）
// table_entry -- 页表项指针
void un_wp_page(unsigned long * table_entry)
{
	unsigned long old_page,new_page;

	old_page = 0xfffff000 & *table_entry;
	// 其在页面映射字节图数组中值为 1 表示没用被共享
	if (old_page >= LOW_MEM && mem_map[MAP_NR(old_page)]==1) {
		*table_entry |= 2;  // R/w => 1,表示可写
		invalidate();
		return;
	}
	if (!(new_page=get_free_page()))
		oom();
	if (old_page >= LOW_MEM)
		mem_map[MAP_NR(old_page)]--;
	copy_page(old_page,new_page);
	*table_entry = new_page | 7;
	invalidate();
}	

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 *
 * If it's in code space we exit with a segment error.
 */
// 页异常中断处理调用的 C 函数。写共享页面处理函数，在 page.s 中被调用。
// 写共享页面时，需要复制页面（写时复制）
// error_code -- 由 CPU 自动生成
// address -- 页面线性地址 
void do_wp_page(unsigned long error_code,unsigned long address)
{
	if (address < TASK_SIZE)
		printk("\n\rBAD! KERNEL MEMORY WP-ERR!\n\r");
	if (address - current->start_code > TASK_SIZE) {
		printk("Bad things happen: page error in do_wp_page\n\r");
		do_exit(SIGSEGV);
	}
#if 0
/* we cannot do this yet: the estdio library writes to code space */
/* stupid, stupid. I really want the libc.a from GNU */
	if (CODE_SPACE(address))
		do_exit(SIGSEGV);
#endif
	un_wp_page((unsigned long *)
		(((address>>10) & 0xffc) + (0xfffff000 &
		*((unsigned long *) ((address>>20) &0xffc)))));

}

// 验证页面是否可写，如果不能写则复制页面。
// address -- 线性地址
void write_verify(unsigned long address)
{
	unsigned long page;

	if (!( (page = *((unsigned long *) ((address>>20) & 0xffc)) )&1))
		return;
	page &= 0xfffff000;  // 获取页表地址
	page += ((address>>10) & 0xffc);  // 获取页表项地址
	if ((3 & *(unsigned long *) page) == 1)  /* non-writeable, present */
		un_wp_page((unsigned long *) page);
	return;
}

void get_empty_page(unsigned long address)
{
	unsigned long tmp;

	if (!(tmp=get_free_page()) || !put_page(tmp,address)) {
		free_page(tmp);		/* 0 is ok - ignored */
		oom();
	}
}

/*
 * try_to_share() checks the page at address "address" in the task "p",
 * to see if it exists, and if it is clean. If so, share it with the current
 * task.
 *
 * NOTE! This assumes we have checked that p != current, and that they
 * share the same executable or library.
 */
// address -- (页面线性地址 - current->start_code) 
static int try_to_share(unsigned long address, struct task_struct * p)
{
	unsigned long from;
	unsigned long to;
	unsigned long from_page;
	unsigned long to_page;
	unsigned long phys_addr;

	from_page = to_page = ((address>>20) & 0xffc);
	from_page += ((p->start_code>>20) & 0xffc);
	to_page += ((current->start_code>>20) & 0xffc);
/* is there a page-directory at from? */
	from = *(unsigned long *) from_page;
	if (!(from & 1))
		return 0;
	from &= 0xfffff000;
	from_page = from + ((address>>10) & 0xffc);
	phys_addr = *(unsigned long *) from_page;
/* is the page clean and present? */
	if ((phys_addr & 0x41) != 0x01) // 0x41 对应Dirty和Present标志
		return 0;
	phys_addr &= 0xfffff000;
	if (phys_addr >= HIGH_MEMORY || phys_addr < LOW_MEM)
		return 0;
	to = *(unsigned long *) to_page;
	if (!(to & 1))
		if (to = get_free_page())
			*(unsigned long *) to_page = to | 7;
		else
			oom();
	to &= 0xfffff000;
	to_page = to + ((address>>10) & 0xffc);
	if (1 & *(unsigned long *) to_page) // 对应的页面已经存在
		panic("try_to_share: to_page already exists");
/* share them: write-protect */
	*(unsigned long *) from_page &= ~2;
	*(unsigned long *) to_page = *(unsigned long *) from_page;
	invalidate();
	phys_addr -= LOW_MEM;
	phys_addr >>= 12;
	mem_map[phys_addr]++;
	return 1;
}

/*
 * share_page() tries to find a process that could share a page with
 * the current one. Address is the address of the wanted page relative
 * to the current data space.
 *
 * We first check if it is at all feasible by checking executable->i_count.
 * It should be >1 if there are other tasks sharing this inode.
 */
static int share_page(struct m_inode * inode, unsigned long address)
{
	struct task_struct ** p;

	if (inode->i_count < 2 || !inode)
		return 0;
	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		if (!*p)
			continue;
		if (current == *p)
			continue;
		if (address < LIBRARY_OFFSET) {
			if (inode != (*p)->executable)
				continue;
		} else {
			if (inode != (*p)->library)
				continue;
		}
		if (try_to_share(address,*p))
			return 1;
	}
	return 0;
}

// 页异常中断处理调用的函数，处理缺页异常情况。
// error_code -- 由 CPU 自动生成
// address -- 页面的线性地址
void do_no_page(unsigned long error_code,unsigned long address)
{
	int nr[4];
	unsigned long tmp;
	unsigned long page;
	int block,i;
	struct m_inode * inode;

	if (address < TASK_SIZE)
		printk("\n\rBAD!! KERNEL PAGE MISSING\n\r");
	if (address - current->start_code > TASK_SIZE) {
		printk("Bad things happen: nonexistent page error in do_no_page\n\r");
		do_exit(SIGSEGV);
	}
	page = *(unsigned long *) ((address >> 20) & 0xffc);
	if (page & 1) {
		page &= 0xfffff000;
		page += (address >> 10) & 0xffc;
		tmp = *(unsigned long *) page;
		// 页表项有效但页面被交换出，则把页面换进内存
		if (tmp && !(1 & tmp)) {
			swap_in((unsigned long *) page);
			return;
		}
	}
	address &= 0xfffff000;
	// 计算指定的线性地址在进程空间中相对于进程基址的偏移长度值
	// current->end_data 是代码段加数据段的长度
	tmp = address - current->start_code; 
	if (tmp >= LIBRARY_OFFSET ) {
		inode = current->library;
		// 库的位置在elf文件中怎么布局的？
		block = 1 + (tmp-LIBRARY_OFFSET) / BLOCK_SIZE;
	} else if (tmp < current->end_data) {
		inode = current->executable;
		// i 节点中，文件数据块的头块即 i_zone[0] 指向的是存放程序头的逻辑块号
		// i_zone[1] 才开始指向存放程序代码和数据的逻辑块号
		block = 1 + tmp / BLOCK_SIZE;
	} else {
		// 什么情况下 tmp >= current->end_data ?
		inode = NULL;
		block = 0;
	}
	if (!inode) {
		get_empty_page(address);
		return;
	}
	if (share_page(inode,tmp))
		return;
	if (!(page = get_free_page()))
		oom();
/* remember that 1 block is used for header */
	for (i=0 ; i<4 ; block++,i++)
		nr[i] = bmap(inode,block);
	bread_page(page,inode->i_dev,nr);
	// 超过 current->end_data 的部分要清零
	i = tmp + 4096 - current->end_data;
	if (i>4095)
		i = 0;
	tmp = page + 4096;
	while (i-- > 0) {
		tmp--;
		*(char *)tmp = 0;
	}
	if (put_page(page,address))
		return;
	free_page(page);
	oom();
}

void mem_init(long start_mem, long end_mem)
{
	int i;

	HIGH_MEMORY = end_mem;
	for (i=0 ; i<PAGING_PAGES ; i++) // PAGING_PAGES size is 15M, i<15M/4096
		mem_map[i] = USED;
	i = MAP_NR(start_mem); // don't use some pages since 0 ? 
	end_mem -= start_mem;
	end_mem >>= 12;  // number of main memory pages
	while (end_mem-->0)
		mem_map[i++]=0;  // mark main memory pages by 0
}

void show_mem(void)
{
	int i,j,k,free=0,total=0;
	int shared=0;
	unsigned long * pg_tbl;

	printk("Mem-info:\n\r");
	for(i=0 ; i<PAGING_PAGES ; i++) {
		if (mem_map[i] == USED)
			continue;
		total++;
		if (!mem_map[i])
			free++;
		else
			shared += mem_map[i]-1;
	}
	printk("%d free pages of %d\n\r",free,total);
	printk("%d pages shared\n\r",shared);
	k = 0;
	for(i=4 ; i<1024 ;) {
		if (1&pg_dir[i]) {
			if (pg_dir[i]>HIGH_MEMORY) {
				printk("page directory[%d]: %08X\n\r",
					i,pg_dir[i]);
				continue;
			}
			if (pg_dir[i]>LOW_MEM)
				free++,k++;
			pg_tbl=(unsigned long *) (0xfffff000 & pg_dir[i]);
			for(j=0 ; j<1024 ; j++)
				if ((pg_tbl[j]&1) && pg_tbl[j]>LOW_MEM)
					if (pg_tbl[j]>HIGH_MEMORY)
						printk("page_dir[%d][%d]: %08X\n\r",
							i,j, pg_tbl[j]);
					else
						k++,free++;
		}
		i++;
		if (!(i&15) && k) {
			k++,free++;	/* one page/process for task_struct */
			printk("Process %d: %d pages\n\r",(i>>4)-1,k);
			k = 0;
		}
	}
	printk("Memory found: %d (%d)\n\r",free-shared,total);
}

