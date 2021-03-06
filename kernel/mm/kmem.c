#include <types.h>
#include <stddef.h>
#include "hal/hal.h"
#include "util.h"
#include "vector.h"
#include "mm/mm.h"
#include "mm/mlayout.h"
#include "mm/mmu.h"
#include "mm/kmem.h"
#include "matrix/matrix.h"
#include "debug.h"

/*
 * Size information for a block
 */
struct header {
	uint32_t magic;		// magic number, used for sanity check
	uint32_t size;
	uint8_t is_hole;
};

struct footer {
	uint32_t magic;		// magic number, same as in header
	struct header *hdr;
};

/* Structure describing a kernel memory pool */
struct kmem_pool {
	struct vector index;
	ptr_t start_addr;	// start of our allocated space
	ptr_t end_addr;		// end of our allocated space
	ptr_t max_addr;		// maximum address the pool can expand to
	uint8_t supervisor; 
	uint8_t readonly;
};

struct kmem_pool *_kpool = NULL;
struct mutex _kmem_lock;	// Lock for the kernel memory pool
boolean_t _kmem_init_done = FALSE;

static void expand(struct kmem_pool *pool, size_t new_size)
{
	struct page *p;
	size_t old_size, i;
	
	/* Sanity check */
	ASSERT(new_size > (pool->end_addr - pool->start_addr));

	/* Round up the new_size to PAGE_SIZE */
	new_size = ROUND_UP(new_size, PAGE_SIZE);
	
	/* Make sure we're not overreaching ourselves */
	ASSERT((pool->start_addr + new_size) < pool->max_addr);

	old_size = pool->end_addr - pool->start_addr;
	i = old_size;

	DEBUG(DL_DBG, ("pool(%p), new_size(%x).\n", pool, new_size));

	while (i < new_size) {
		p = mmu_get_page(&_kernel_mmu_ctx, pool->start_addr + i, TRUE, 0);
		page_alloc(p, 0);
		p->user = pool->supervisor ? TRUE : FALSE;
		p->rw = pool->readonly ? FALSE : TRUE;
		i += PAGE_SIZE;
	}
	
	pool->end_addr = pool->start_addr + new_size;
}

static uint32_t contract(struct kmem_pool *pool, size_t new_size)
{
	struct page *p;
	size_t old_size, i;

	/* Sanity check */
	ASSERT(new_size < (pool->end_addr - pool->start_addr));

	if (new_size & PAGE_SIZE) {
		new_size &= PAGE_SIZE;
		new_size += PAGE_SIZE;
	}

	/* Don't contract too much */
	if (new_size < POOL_MIN_SIZE) {
		new_size = POOL_MIN_SIZE;
	}

	old_size = pool->end_addr - pool->start_addr;
	i = old_size - PAGE_SIZE;

	DEBUG(DL_DBG, ("pool(%p), new_size(%x).\n", pool, new_size));

	while (new_size < i) {
		p = mmu_get_page(&_kernel_mmu_ctx, pool->start_addr + i, FALSE, 0);
		page_free(p);
		i -= PAGE_SIZE;
	}

	pool->end_addr = pool->start_addr + new_size;
	
	return new_size;
}

static int8_t header_compare(void *x, void *y)
{
	struct header *a = (struct header *)x;
	struct header *b = (struct header *)y;

	if (a->size == b->size)
		return 0;
	else
		return (a->size > b->size) ? 1 : 0;
}

/*
 * create the pool
 * start - the address to place the pool index at
 */
struct kmem_pool *create_pool(uint32_t start, uint32_t end, uint32_t max,
			      uint8_t supervisor, uint8_t readonly)
{
	phys_addr_t addr;
	struct kmem_pool *pool;
	struct header *hole;

	ASSERT(start % PAGE_SIZE == 0);
	ASSERT(end % PAGE_SIZE == 0);

	page_early_alloc(&addr, sizeof(struct kmem_pool), 0);
	ASSERT(addr != 0);
	
	pool = (struct kmem_pool *)addr;

	/* Initialize the index of the pool, size of index is fixed */
	place_vector(&pool->index, (void *)start, POOL_INDEX_SIZE, header_compare);

	/* Shift the start address forward to resemble where we can start putting data */
	start += (sizeof(type_t) * POOL_INDEX_SIZE);
	
	/* Make sure the start address is page-aligned */
	if ((start & 0xFFF) != 0) {
		start &= 0xFFFFF000;
		start += PAGE_SIZE;
	}
	
	pool->start_addr = start;
	pool->end_addr = end;
	pool->max_addr = max;
	pool->supervisor = supervisor;
	pool->readonly = readonly;

	/* Initialize header of the first hole */
	hole = (struct header *)start;
	hole->size = end - start;
	hole->magic = POOL_MAGIC;
	hole->is_hole = 1;

	insert_vector(&pool->index, (void *)hole);

	return pool;
}

static uint32_t find_smallest_hole(struct kmem_pool *pool, size_t size, boolean_t page_align)
{
	uint32_t iterator;
	
	/* Find the smallest hole that will fit */
	iterator = 0;

	while (iterator < pool->index.size) {
		struct header *header;
		header = (struct header *)lookup_vector(&pool->index, iterator);

		/* If the user has requested the memory be page-aligned */
		if (page_align) {
			/* Page-align the starting point of this header */
			uint32_t location;
			int32_t offset = 0;
			int32_t hole_size;

			location = (uint32_t)header;
			if (((location + sizeof(struct header)) & 0xFFF) != 0) {
				offset = PAGE_SIZE - ((location + sizeof(struct header)) % PAGE_SIZE);
			}
			hole_size = (int32_t)header->size - offset;
			
			/* Can we fit now ? */
			if (hole_size >= (int32_t)size) {
				break;
			}
		} else if (header->size >= size) {
			break;
		}
		
		iterator++;
	}

	if (iterator == pool->index.size) {
		return -1;	// No hole left for us
	} else {
		return iterator;
	}
}

void *alloc(struct kmem_pool *pool, size_t size, boolean_t page_align)
{
	size_t new_size;
	int32_t iterator;
	struct header *orig_hole_hdr, *block_hdr;
	struct footer *block_ftr;
	uint32_t orig_hole_pos, orig_hole_size;

	/* Make sure we take the size of header/footer into account */
	new_size = sizeof(struct header) + sizeof(struct footer) + size;

	/* Find the smallest hole that will fit */
	iterator = find_smallest_hole(pool, new_size, page_align);
	if (iterator == -1) {
		size_t old_length = pool->end_addr - pool->start_addr;
		size_t old_end_addr = pool->end_addr;
		size_t new_length, idx, value;
		struct header *header;
		struct footer *footer;

		/* We need to allocate more space */
		expand(pool, old_length + new_size);
		new_length = pool->end_addr - pool->start_addr;

		/* Find the last header in location */
		iterator = 0;
		idx = -1;
		value = 0;

		while (iterator < pool->index.size) {
			uint32_t tmp = (uint32_t)lookup_vector(&pool->index, iterator);
			if (tmp > value) {
				value = tmp;
				idx = iterator;
			}
			iterator++;
		}

		/* If we didn't find any headers we need to add one */
		if (idx == -1) {
			header = (struct header *)old_end_addr;
			header->magic = POOL_MAGIC;
			header->size = new_length - old_length;
			header->is_hole = 1;
			footer = (struct footer *)(old_end_addr + header->size - sizeof(struct footer));
			footer->magic = POOL_MAGIC;
			footer->hdr = header;
			insert_vector(&pool->index, (void *)header);
		} else {
			/* The last header need adjusting */
			header = lookup_vector(&pool->index, idx);
			header->size += (new_length - old_length);
			/* Rewrite the footer */
			footer = (struct footer *)((uint32_t)header + header->size - sizeof(struct footer));
			footer->hdr = header;
			footer->magic = POOL_MAGIC;
		}

		/* We have enough space now. Call alloc again. */
		return alloc(pool, size, page_align);
	}

	orig_hole_hdr = (struct header *)lookup_vector(&pool->index, iterator);
	orig_hole_pos = (uint32_t)orig_hole_hdr;
	orig_hole_size = orig_hole_hdr->size;

	/* Here we work out if we should split the hole we found into parts
	 * Is the original hole size - requested hole size less than the overhead
	 * for adding a new hole?
	 */
	if ((orig_hole_size - new_size) < (sizeof(struct header) + sizeof(struct footer))) {
		/* Then just increase the requested size to the hole we found */
		size += (orig_hole_size - new_size);
		new_size = orig_hole_size;
	}

	/* If we need to page-align the data, do it now and make a new hole
	 * in front of our block
	 */
	if (page_align && ((orig_hole_pos + sizeof(struct header)) & 0xFFF)) {
		uint32_t new_location;
		struct header *hole_header;
		struct footer *hole_footer;
		/* The memory address returned to caller should be page-aligned */
		new_location = orig_hole_pos + PAGE_SIZE - (orig_hole_pos & 0xFFF) - sizeof(struct header);
		hole_header = (struct header *)orig_hole_pos;
		hole_header->size = PAGE_SIZE - (orig_hole_pos & 0xFFF) - sizeof(struct header);
		hole_header->magic = POOL_MAGIC;
		hole_header->is_hole = 1;
		hole_footer = (struct footer *)(new_location - sizeof(struct footer));
		hole_footer->magic = POOL_MAGIC;
		hole_footer->hdr = hole_header;
		/* We have made a new hole so don't need to remove it from vector */
		orig_hole_pos = new_location;
		orig_hole_size = orig_hole_size - hole_header->size;
	} else {
		/* This hole was allocated, so just remove it */
		remove_vector(&pool->index, iterator);
	}

	/* Overwrite the original header ... */
	block_hdr = (struct header *)orig_hole_pos;
	block_hdr->magic = POOL_MAGIC;
	block_hdr->is_hole = 0;
	block_hdr->size = new_size;

	/* And the footer ... */
	block_ftr = (struct footer *)(orig_hole_pos + sizeof(struct header) + size);
	block_ftr->magic = POOL_MAGIC;
	block_ftr->hdr = block_hdr;

	/* We may need to write a new hole after the allocated block. */
	if (orig_hole_size  > new_size) {
		struct header *hole_hdr;
		struct footer *hole_ftr;
		hole_hdr = (struct header *)
			(orig_hole_pos + sizeof(struct header) + size + sizeof(struct footer));
		hole_hdr->magic = POOL_MAGIC;
		hole_hdr->is_hole = 1;
		hole_hdr->size = orig_hole_size - new_size;
		hole_ftr = (struct footer *)
			((uint32_t)hole_hdr + orig_hole_size - new_size - sizeof(struct footer));
		if ((uint32_t)hole_ftr < pool->end_addr) {
			hole_ftr->magic = POOL_MAGIC;
			hole_ftr->hdr = hole_hdr;
		}

		/* Insert the new hole into the index */
		insert_vector(&pool->index, (void *)hole_hdr);
	}

	/* Done! */
	return (void *)((uint32_t)block_hdr + sizeof(struct header));
}

void free(struct kmem_pool *pool, void *p)
{
	boolean_t do_add, do_contract;
	struct header *header, *test_hdr;
	struct footer *footer, *test_ftr;
	uint32_t iterator;
	
	if (!p) {
		return;
	}

	header = (struct header *)((uint32_t)p - sizeof(struct header));
	footer = (struct footer *)((uint32_t)header + header->size - sizeof(struct footer));

	/* Sanity check */
	ASSERT(header->magic == POOL_MAGIC);
	ASSERT(footer->magic == POOL_MAGIC);

	/* Make us a hole */
	header->is_hole = 1;

	/* Do we want to add this header into the 'free holes' index? */
	do_add = TRUE;

	test_ftr = (struct footer *)((uint32_t)header - sizeof(struct footer));
	if ((test_ftr->magic == POOL_MAGIC) &&
	    (test_ftr->hdr->is_hole)) {
		uint32_t cache_size = header->size;	// Cache our current size
		header = test_ftr->hdr;
		footer->hdr = header;
		header->size += cache_size;
		do_add = FALSE;
	}

	test_hdr = (struct header *)((uint32_t)footer + sizeof(struct footer));
	if ((test_hdr->magic == POOL_MAGIC) &&
	    (test_hdr->is_hole)) {
		
		header->size += test_hdr->size;		// Increase our size
		test_ftr = (struct footer *)
			((uint32_t)test_hdr + test_hdr->size - sizeof(struct footer));
		footer = test_ftr;
		/* Find and remove this header from the index */
		iterator = 0;
		while ((iterator < pool->index.size) &&
		       (lookup_vector(&pool->index, iterator) != (void *)test_hdr)) {
			iterator++;
		}

		/* Make sure we actually find the item */
		ASSERT(iterator < pool->index.size);
		/* Remove it */
		remove_vector(&pool->index, iterator);
	}

	/* If the footer location is end address, try to contract */
	do_contract = pool->end_addr > (KERNEL_KMEM_START + KERNEL_KMEM_SIZE);
	if (((uint32_t)footer + sizeof(struct footer) == pool->end_addr) &&
	    do_contract) {
		size_t old_length = pool->end_addr - pool->start_addr;
		size_t new_length = contract(pool, (uint32_t)header - pool->start_addr);

		if (header->size - (old_length - new_length) > 0) {
			header->size -= (old_length - new_length);
			footer = (struct footer *)((uint32_t)header + header->size -
						   sizeof(struct footer));
			footer->magic = POOL_MAGIC;
			footer->hdr = header;
		} else {

			iterator = 0;
			while ((iterator < pool->index.size) &&
			       (lookup_vector(&pool->index, iterator) != (void *)test_hdr)) {
				iterator++;
			}
			if (iterator < pool->index.size) {
				remove_vector(&pool->index, iterator);
			}
		}
	}

	if (do_add) {
		insert_vector(&pool->index, (void *)header);
	}
}

void *kmem_alloc(size_t size, int mmflag)
{
	void *ret = NULL;
	boolean_t align;

	if (!_kpool) {
		goto out;
	}
	
	align = FLAG_ON(mmflag, MM_ALIGN) ? TRUE : FALSE;

	/* Allocate virtual address from the kernel memory pool */
	ret = alloc(_kpool, size, align);
	if (!ret) {
		goto out;
	}
	
	if (align) {
		ASSERT(((uint32_t)ret % PAGE_SIZE) == 0);
	}

 out:
	return ret;
}

void kmem_free(void *p)
{
	free(_kpool, p);
}

void *kmem_map(phys_addr_t base, size_t size, int mmflag)
{
	int rc;
	size_t i;
	ptr_t virt;

	ASSERT(((base % PAGE_SIZE) == 0) && ((size % PAGE_SIZE) == 0));
	
	rc = 0;
	virt = base;	// FixMe: For now we just do the identical map (virt = base)
	
	for (i = 0; i < size; i += PAGE_SIZE) {
		rc = mmu_map(&_kernel_mmu_ctx, virt + i, base + i,
			     MMU_MAP_WRITE | MMU_MAP_EXEC);
		if (rc != 0) {
			virt = (ptr_t)NULL;
			break;
		}
	}

	if (rc != 0) {
		/* Rollback the changes */
		for (; i != 0; i -= PAGE_SIZE) {
			mmu_unmap(&_kernel_mmu_ctx, virt + (i - PAGE_SIZE),
				  TRUE, NULL);
		}
	}

	DEBUG(DL_DBG, ("virt(%x) map range[%p, %p) rc(%x)\n",
		       virt, base, base + size, rc));

	return (void *)virt;
}

void kmem_unmap(void *addr, size_t size, boolean_t shared)
{
	int rc;
	size_t i;
	ptr_t virt;
	phys_addr_t phys;

	ASSERT((((ptr_t)addr % PAGE_SIZE) == 0) && ((size % PAGE_SIZE) == 0));
	
	rc = 0;
	virt = (ptr_t)addr;
	
	for (i = 0; i < size; i += PAGE_SIZE) {
		rc = mmu_unmap(&_kernel_mmu_ctx, virt + i, shared, &phys);
		if (rc != 0) {
			PANIC("Unmapping page failed");
		}
	}

	DEBUG(DL_DBG, ("unmap range[%p, %p)\n", virt, virt + size));
}

void init_kmem()
{
	/* Initialize the kernel memory pool lock */
	mutex_init(&_kmem_lock, "kmem-mutex", 0);
	
	/* Create kernel memory pool */
	_kpool = create_pool(KERNEL_KMEM_START, KERNEL_KMEM_START + KERNEL_KMEM_SIZE,
			     0xCFFFF000, FALSE, FALSE);
	ASSERT(_kpool != NULL);
	
	_kmem_init_done = TRUE;
}
