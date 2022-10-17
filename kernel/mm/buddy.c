#include <common/util.h>
#include <common/macro.h>
#include <common/kprint.h>

#include "buddy.h"

//zfb
void page_append(struct phys_mem_pool *pool, struct page *page) {
	struct free_list *free_list = &pool->free_lists[page->order];
	list_add(&page->node, &free_list->free_list);
	free_list->nr_free++;
}

void page_del(struct phys_mem_pool *pool, struct page *page) {
	struct free_list *free_list = &pool->free_lists[page->order];
	list_del(&page->node);
	free_list->nr_free--;
}
//zfb

/*
 * The layout of a phys_mem_pool:
 * | page_metadata are (an array of struct page) | alignment pad | usable memory |
 *
 * The usable memory: [pool_start_addr, pool_start_addr + pool_mem_size).
 */
void init_buddy(struct phys_mem_pool *pool, struct page *start_page,
		vaddr_t start_addr, u64 page_num)
{
	int order;
	int page_idx;
	struct page *page;

	/* Init the physical memory pool. */
	pool->pool_start_addr = start_addr;
	pool->page_metadata = start_page;
	pool->pool_mem_size = page_num * BUDDY_PAGE_SIZE;
	/* This field is for unit test only. */
	pool->pool_phys_page_num = page_num;

	/* Init the free lists */
	for (order = 0; order < BUDDY_MAX_ORDER; ++order) {
		pool->free_lists[order].nr_free = 0;
		init_list_head(&(pool->free_lists[order].free_list));
	}

	/* Clear the page_metadata area. */
	memset((char *)start_page, 0, page_num * sizeof(struct page));

	/* Init the page_metadata area. */
	for (page_idx = 0; page_idx < page_num; ++page_idx) {
		page = start_page + page_idx;
		page->allocated = 1;
		page->order = 0;
	}

	/* Put each physical memory page into the free lists. */
	for (page_idx = 0; page_idx < page_num; ++page_idx) {
		page = start_page + page_idx;
		buddy_free_pages(pool, page);
	}
}

static struct page *get_buddy_chunk(struct phys_mem_pool *pool,
				    struct page *chunk)
{
	u64 chunk_addr;
	u64 buddy_chunk_addr;
	int order;

	/* Get the address of the chunk. */
	chunk_addr = (u64) page_to_virt(pool, chunk);
	order = chunk->order;
	/*
	 * Calculate the address of the buddy chunk according to the address
	 * relationship between buddies.
	 */
#define BUDDY_PAGE_SIZE_ORDER (12)
	buddy_chunk_addr = chunk_addr ^
	    (1UL << (order + BUDDY_PAGE_SIZE_ORDER));

	/* Check whether the buddy_chunk_addr belongs to pool. */
	if ((buddy_chunk_addr < pool->pool_start_addr) ||
	    (buddy_chunk_addr >= (pool->pool_start_addr +
				  pool->pool_mem_size))) {
		return NULL;
	}

	return virt_to_page(pool, (void *)buddy_chunk_addr);
}

/*
 * split_page: split the memory block into two smaller sub-block, whose order
 * is half of the origin page.
 * pool @ physical memory structure reserved in the kernel
 * order @ order for origin page block
 * page @ splitted page
 * 
 * Hints: don't forget to substract the free page number for the corresponding free_list.
 * you can invoke split_page recursively until the given page can not be splitted into two
 * smaller sub-pages.
 */
//zfb
// 递归的分割块，直到分割至指定大小
static struct page *split_page(struct phys_mem_pool *pool, u64 order,
			       struct page *page)
{
	// <lab2>
    // 禁止分割已分配的块
	if (page->allocated) {
		kwarn("Try to split an allocated page\n");
		return 0;
	}

    // 标记块为未分配，并从 free_list 中删除
	page->allocated = 0;
	page_del(pool, page);

    // 递归的分割块，直到 order 变成指定大小
	while (page->order > order) {
		// 先缩小 order，再找到对应的伙伴块
        // 操作后原先的 page 就变成了当前 page 和 buddy_page
        page->order--;
		struct page *buddy_page = get_buddy_chunk(pool, page);

        // 把 buddy_page 插入 free_list 中
        if (buddy_page != NULL) {
			buddy_page->allocated = 0;
			buddy_page->order = page->order;
			page_append(pool, buddy_page);
		}
	}

	return page;
	// </lab2>
}
//zfb

/*
 * buddy_get_pages: get free page from buddy system.
 * pool @ physical memory structure reserved in the kernel
 * order @ get the (1<<order) continous pages from the buddy system
 * 
 * Hints: Find the corresonding free_list which can allocate 1<<order
 * continuous pages and don't forget to split the list node after allocation   
 */
//zfb
// 申请指定 order 的块
struct page *buddy_get_pages(struct phys_mem_pool *pool, u64 order)
{
	// <lab2>
    // 找到一个非空的，足够大的 free_list
	int current_order = order;
	while (current_order < BUDDY_MAX_ORDER && pool->free_lists[current_order].nr_free <= 0)
		current_order++;
	
    // 申请的 order 太大或者没有足够大的块能分配
	if (current_order >= BUDDY_MAX_ORDER) {
		kwarn("Try to allocate an buddy chunk greater than BUDDY_MAX_ORDER");
		return NULL;
	}

    // 得到指定 free_list 的表头块
	struct page *page = list_entry(pool->free_lists[current_order].free_list.next, struct page, node);
	if (page == NULL){
		kdebug("buddy get a NULL page\n");
		return NULL;
	}

    // 分割块
	split_page(pool, order, page);
	
	// 将返回的块标记为已分配
	page->allocated = 1;
	return page;
	// </lab2>
}
//zfb

/*
 * merge_page: merge the given page with the buddy page
 * pool @ physical memory structure reserved in the kernel
 * page @ merged page (attempted)
 * 
 * Hints: you can invoke the merge_page recursively until
 * there is not corresponding buddy page. get_buddy_chunk
 * is helpful in this function.
 */
//zfb
// 对指定块向上递归的做合并操作
static struct page *merge_page(struct phys_mem_pool *pool, struct page *page)
{
	// <lab2>
    // 只能对空闲块做合并操作，禁止对已分配的块做合并操作
	if (page->allocated) {
		kwarn("Try to merge an allocated page\n", page);
		return NULL;
	}

    // 合并前先将空闲块从它所属的 free_list 中拆出来
	page_del(pool, page);

	// 递归地向上合并空闲块，循环停止地条件:
    // 1) 当前块的 order 已达到允许的最大值
    // 2) 找不到伙伴块或者无法与伙伴块合并
	while (page->order < BUDDY_MAX_ORDER - 1) {
		struct page* buddy_page = get_buddy_chunk(pool, page);
        // 只能与等大的、空闲的伙伴块合并
		if (buddy_page == NULL ||
			buddy_page->allocated ||
			buddy_page->order != page->order) {
			break;
		}

        // 调整下位置，保证 page 为左伙伴， buddy_page 为右伙伴
		if(page > buddy_page) {
			struct page *tmp = buddy_page;
			buddy_page = page;
			page = tmp;
		}

        // 做合并，将 buddy_page 标记为已分配并从 free_list 中删除
        // 将 page 的 order ++
		buddy_page->allocated = 1;
		page_del(pool, page);
		page->order++;
	}
	
    // 将合并后的块插入对应的 free_list 中
	page_append(pool, page);
	return page;
	// </lab2>
}
//zfb

/*
 * buddy_free_pages: give back the pages to buddy system
 * pool @ physical memory structure reserved in the kernel
 * page @ free page structure
 * 
 * Hints: you can invoke merge_page.
 */
//zfb
// 回收指定块
void buddy_free_pages(struct phys_mem_pool *pool, struct page *page)
{
	// <lab2>
    // 空闲的块无法被回收
	if (!page->allocated) {
		kwarn("Try to free a free page\n");
		return;
	}

    // 将块标记为空闲，并插入到 free_list 中
	page->allocated = 0;
	page_append(pool, page);

    // 递归的向上合并块
	merge_page(pool, page);
	// </lab2>
}
//zfb

void *page_to_virt(struct phys_mem_pool *pool, struct page *page)
{
	u64 addr;

	/* page_idx * BUDDY_PAGE_SIZE + start_addr */
	addr = (page - pool->page_metadata) * BUDDY_PAGE_SIZE +
	    pool->pool_start_addr;
	return (void *)addr;
}

struct page *virt_to_page(struct phys_mem_pool *pool, void *addr)
{
	struct page *page;

	page = pool->page_metadata +
	    (((u64) addr - pool->pool_start_addr) / BUDDY_PAGE_SIZE);
	return page;
}

u64 get_free_mem_size_from_buddy(struct phys_mem_pool * pool)
{
	int order;
	struct free_list *list;
	u64 current_order_size;
	u64 total_size = 0;

	for (order = 0; order < BUDDY_MAX_ORDER; order++) {
		/* 2^order * 4K */
		current_order_size = BUDDY_PAGE_SIZE * (1 << order);
		list = pool->free_lists + order;
		total_size += list->nr_free * current_order_size;

		/* debug : print info about current order */
		kdebug("buddy memory chunk order: %d, size: 0x%lx, num: %d\n",
		       order, current_order_size, list->nr_free);
	}
	return total_size;
}
