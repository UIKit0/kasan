#include <linux/asan.h>

#include <linux/export.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/stacktrace.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/bug.h>
#include <asm/page.h>
#include <asm/page_64.h>
#include <asm/thread_info.h>

#include "asan.h"
#include "test.h"

#undef memset
#undef memcpy

/* CONFIG_ASAN is incompatible with CONFIG_DEBUG_SLAB */
/* CONFIG_ASAN is incompatible with CONFIG_KMEMCHECK */

static struct {
	int enabled;
	spinlock_t quarantine_lock;
	struct list_head quarantine_list;
	size_t quarantine_size;
} ctx = {
	.enabled = 0,
	.quarantine_lock = __SPIN_LOCK_UNLOCKED(ctx.quarantine_lock),
	.quarantine_list = LIST_HEAD_INIT(ctx.quarantine_list),
	.quarantine_size = 0,
};

static struct kmem_cache *virt_to_cache(const void *ptr)
{
	struct page *page = virt_to_head_page(ptr);
	return page->slab_cache;
}


pid_t asan_current_thread_id(void)
{
	return current_thread_info()->task->pid;
}

unsigned int asan_save_stack_trace(unsigned long *output,
				   unsigned int max_entries,
				   unsigned long strip_addr)
{
	unsigned long stack[ASAN_MAX_STACK_TRACE_FRAMES];
	unsigned int entries;
	unsigned int beg = 0, end;

	struct stack_trace trace_info = {
		.nr_entries = 0,
		.entries = stack,
		.max_entries = ASAN_MAX_STACK_TRACE_FRAMES,
		.skip = 0
	};
	save_stack_trace(&trace_info);
	entries = trace_info.nr_entries;

	while (stack[beg] != strip_addr && beg < entries)
		beg++;
	end = (entries - beg <= max_entries) ? entries : beg + max_entries;

	memcpy(output, stack + beg, (end - beg) * sizeof(unsigned long));
	return end - beg;
}

static void asan_quarantine_put(struct kmem_cache *cache, void *object)
{
	unsigned long size = cache->object_size;
	unsigned long rounded_up_size = round_up(size, ASAN_SHADOW_GRAIN);
	struct asan_redzone *redzone = object + rounded_up_size;
	struct chunk *chunk = &redzone->chunk;
	unsigned long flags;

	if (!ctx.enabled)
		return;

	spin_lock_irqsave(&ctx.quarantine_lock, flags);

	list_add(&chunk->list, &ctx.quarantine_list);
	ctx.quarantine_size += cache->object_size;

	spin_unlock_irqrestore(&ctx.quarantine_lock, flags);
}

static void asan_quarantine_flush(void)
{
	struct chunk *chunk;
	unsigned long flags;

	spin_lock_irqsave(&ctx.quarantine_lock, flags);

	while (ctx.quarantine_size > ASAN_QUARANTINE_SIZE) {
		BUG_ON(list_empty(&ctx.quarantine_list));

		chunk = list_entry(ctx.quarantine_list.prev,
				   struct chunk, list);
		list_del(ctx.quarantine_list.prev);

		ctx.quarantine_size -= chunk->cache->object_size;

		spin_unlock_irqrestore(&ctx.quarantine_lock, flags);
		kmem_cache_free(chunk->cache, chunk->object);
		spin_lock_irqsave(&ctx.quarantine_lock, flags);
	}

	spin_unlock_irqrestore(&ctx.quarantine_lock, flags);
}

static void asan_quarantine_drop_cache(struct kmem_cache *cache)
{
	unsigned long flags;
	struct list_head *pos, *n;
	struct chunk *chunk;

	spin_lock_irqsave(&ctx.quarantine_lock, flags);

	list_for_each_safe(pos, n, &ctx.quarantine_list) {
		chunk = list_entry(pos, struct chunk, list);
		if (chunk->cache == cache) {
			list_del(pos);

			ctx.quarantine_size -= chunk->cache->object_size;

			spin_unlock_irqrestore(&ctx.quarantine_lock, flags);
			kmem_cache_free(chunk->cache, chunk->object);
			spin_lock_irqsave(&ctx.quarantine_lock, flags);
		}
	}

	spin_unlock_irqrestore(&ctx.quarantine_lock, flags);
}

static bool asan_addr_is_in_mem(unsigned long addr)
{
	return (addr >= (unsigned long)(__va(0)) &&
		addr < (unsigned long)(__va(max_pfn << PAGE_SHIFT)));
}

unsigned long asan_mem_to_shadow(unsigned long addr)
{
	if (!asan_addr_is_in_mem(addr))
		return 0;
	return ((addr - PAGE_OFFSET) >> ASAN_SHADOW_SCALE)
		+ PAGE_OFFSET + ASAN_SHADOW_OFFSET;
}

unsigned long asan_shadow_to_mem(unsigned long shadow_addr)
{
	return ((shadow_addr - ASAN_SHADOW_OFFSET - PAGE_OFFSET)
		<< ASAN_SHADOW_SCALE) + PAGE_OFFSET;
}

/*
 * Poisons the shadow memory for 'size' bytes starting from 'addr'.
 * Memory addresses should be aligned to ASAN_SHADOW_GRAIN.
 */
static void
asan_poison_shadow(const void *address, unsigned long size, u8 value)
{
	unsigned long shadow_beg, shadow_end;
	unsigned long addr = (unsigned long)address;

	BUG_ON(!IS_ALIGNED(addr, ASAN_SHADOW_GRAIN));
	BUG_ON(!IS_ALIGNED(addr + size, ASAN_SHADOW_GRAIN));
	BUG_ON(!asan_addr_is_in_mem(addr));
	BUG_ON(!asan_addr_is_in_mem(addr + size - ASAN_SHADOW_GRAIN));

	shadow_beg = asan_mem_to_shadow(addr);
	shadow_end = asan_mem_to_shadow(addr + size - ASAN_SHADOW_GRAIN)
		     + 1;
	memset((void *)shadow_beg, value, shadow_end - shadow_beg);
}

static void asan_unpoison_shadow(const void *address, unsigned long size)
{
	asan_poison_shadow(address, size, 0);
}

static bool asan_memory_is_poisoned(unsigned long addr)
{
	const unsigned long ACCESS_SIZE = 1;
	u8 *shadow_addr = (u8 *)asan_mem_to_shadow(addr);
	s8 shadow_value = *shadow_addr;
	if (shadow_value != 0) {
		u8 last_accessed_byte = (addr & (ASAN_SHADOW_GRAIN - 1))
					+ ACCESS_SIZE - 1;
		return last_accessed_byte >= shadow_value;
	}
	return false;
}

static bool asan_mem_is_zero(const u8 *beg, unsigned long size)
{
	const u8 *end = beg + size;
	unsigned long beg_addr = (unsigned long)beg;
	unsigned long end_addr = (unsigned long)end;
	unsigned long *aligned_beg =
		(unsigned long *)round_up(beg_addr, sizeof(unsigned long));
	unsigned long *aligned_end =
		(unsigned long *)round_down(end_addr, sizeof(unsigned long));
	unsigned long all = 0;
	const u8 *mem;
	for (mem = beg; mem < (u8 *)aligned_beg && mem < end; mem++)
		all |= *mem;
	for (; aligned_beg < aligned_end; aligned_beg++)
		all |= *aligned_beg;
	if ((u8 *)aligned_end >= beg)
		for (mem = (u8 *)aligned_end; mem < end; mem++)
			all |= *mem;
	return all == 0;
}

/*
 * Returns pointer to the first poisoned byte if the region is in memory
 * and poisoned, returns NULL otherwise.
 */
static const void *asan_region_is_poisoned(const void *addr, unsigned long size)
{
	unsigned long beg, end;
	unsigned long aligned_beg, aligned_end;
	unsigned long shadow_beg, shadow_end;

	if (size == 0)
		return NULL;

	beg = (unsigned long)addr;
	end = beg + size;
	if (!asan_addr_is_in_mem(beg) || !asan_addr_is_in_mem(end))
		return NULL;

	aligned_beg = round_up(beg, ASAN_SHADOW_GRAIN);
	aligned_end = round_down(end, ASAN_SHADOW_GRAIN);
	shadow_beg = asan_mem_to_shadow(aligned_beg);
	shadow_end = asan_mem_to_shadow(aligned_end);
	if (!asan_memory_is_poisoned(beg) &&
	    !asan_memory_is_poisoned(end - 1) &&
	    (shadow_end <= shadow_beg ||
	     asan_mem_is_zero((const u8 *)shadow_beg, shadow_end - shadow_beg)))
		return NULL;
	for (; beg < end; beg++)
		if (asan_memory_is_poisoned(beg))
			return (const void *)beg;

	BUG(); /* Unreachable. */
	return NULL;
}

void asan_check_memory_region(const void *addr, unsigned long size, bool write)
{
	unsigned long poisoned_addr;
	unsigned long strip_addr;

	if (!ctx.enabled)
		return;

	poisoned_addr = (unsigned long)asan_region_is_poisoned(addr, size);

	if (poisoned_addr == 0)
		return;

	/* FIXME: still prints asan_memset frame. */
	strip_addr = (unsigned long)__builtin_return_address(0);
	asan_report_error(poisoned_addr, size, write, strip_addr);
}

static void asan_check_memory_word(unsigned long addr, unsigned long size,
				   bool write)
{
	u8 *shadow_addr;
	s8 shadow_value;
	u8 last_accessed_byte;
	unsigned long strip_addr;

	if (!ctx.enabled)
		return;

	if (!asan_addr_is_in_mem(addr) || !asan_addr_is_in_mem(addr + size))
		return;

	shadow_addr = (u8 *)asan_mem_to_shadow(addr);
	shadow_value = *shadow_addr;
	if (shadow_value == 0)
		return;

	last_accessed_byte = (addr & (ASAN_SHADOW_GRAIN - 1)) + size - 1;
	if (last_accessed_byte < shadow_value)
		return;

	strip_addr = (unsigned long)__builtin_return_address(0);
	asan_report_error(addr, size, write, strip_addr);
}

void __init asan_init_shadow(void)
{
	unsigned long shadow_size =
		(max_pfn << PAGE_SHIFT) >> ASAN_SHADOW_SCALE;
	unsigned long found_free_range = memblock_find_in_range(
		ASAN_SHADOW_OFFSET, ASAN_SHADOW_OFFSET + shadow_size,
		shadow_size, ASAN_SHADOW_GRAIN);
	void *shadow_beg = (void *)(PAGE_OFFSET + ASAN_SHADOW_OFFSET);

	pr_err("Shadow offset: %lx\n", ASAN_SHADOW_OFFSET);
	pr_err("Shadow size: %lx\n", shadow_size);

	if (found_free_range != ASAN_SHADOW_OFFSET ||
	    memblock_reserve(ASAN_SHADOW_OFFSET, shadow_size) != 0) {
		pr_err("Error: unable to reserve shadow!\n");
		return;
	}

	memset(shadow_beg, 0, shadow_size);
	asan_poison_shadow(shadow_beg, shadow_size, ASAN_SHADOW_GAP);

	ctx.enabled = 1;
}

void asan_cache_create(struct kmem_cache *cache, size_t *size)
{
	unsigned long object_size = cache->object_size;
	unsigned long rounded_up_object_size =
		round_up(object_size, sizeof(unsigned long));

	if (ASAN_HAS_REDZONE(cache)) {
		*size += ASAN_REDZONE_SIZE;

		/* Ensure that the cache is large enough. */
		BUG_ON(*size < rounded_up_object_size + ASAN_REDZONE_SIZE);
	}
}

void asan_cache_destroy(struct kmem_cache *cache)
{
	asan_quarantine_drop_cache(cache);
}

void asan_slab_create(struct kmem_cache *cache, void *slab)
{
	if (cache->flags & SLAB_DESTROY_BY_RCU)
		return;
	asan_poison_shadow(slab, (1 << cache->gfporder) << PAGE_SHIFT,
			   ASAN_HEAP_REDZONE);
	asan_quarantine_flush();
}

void asan_slab_destroy(struct kmem_cache *cache, void *slab)
{
	asan_unpoison_shadow(slab, (1 << cache->gfporder) << PAGE_SHIFT);
}

void asan_slab_alloc(struct kmem_cache *cache, void *object)
{
	unsigned long addr = (unsigned long)object;
	unsigned long size = cache->object_size;
	unsigned long rounded_down_size = round_down(size, ASAN_SHADOW_GRAIN);
	unsigned long rounded_up_size = round_up(size, ASAN_SHADOW_GRAIN);
	struct asan_redzone *redzone;
	unsigned long *alloc_stack;
	u8 *shadow;
	unsigned long strip_addr;

	asan_unpoison_shadow(object, rounded_down_size);
	if (rounded_down_size != size) {
		shadow = (u8 *)asan_mem_to_shadow(addr + rounded_down_size);
		*shadow = size & (ASAN_SHADOW_GRAIN - 1);
	}

	if (!ASAN_HAS_REDZONE(cache))
		return;

	redzone = object + rounded_up_size;
	alloc_stack = redzone->alloc_stack;

	/* Strip asan_slab_alloc and kmem_cache_alloc frames. */
	strip_addr = (unsigned long)__builtin_return_address(1);
	asan_save_stack_trace(alloc_stack, ASAN_STACK_TRACE_FRAMES,
			      strip_addr);

	redzone->alloc_thread_id = asan_current_thread_id();
	redzone->free_thread_id = -1;

	redzone->chunk.cache = cache;
	redzone->chunk.object = object;

	redzone->quarantine_flag = 0;
	redzone->kmalloc_size = 0;
}

bool asan_slab_free(struct kmem_cache *cache, void *object)
{
	unsigned long size = cache->object_size;
	unsigned long rounded_up_size = round_up(size, ASAN_SHADOW_GRAIN);
	struct asan_redzone *redzone;
	unsigned long *free_stack;
	unsigned long strip_addr;

	if (cache->flags & SLAB_DESTROY_BY_RCU)
		return true;

	/* XXX: double poisoning with quarantine. */
	asan_poison_shadow(object, rounded_up_size, ASAN_HEAP_FREE);

	if (!ASAN_HAS_REDZONE(cache))
		return true;

	redzone = object + rounded_up_size;
	free_stack = redzone->free_stack;

	/* Check if the object is in the quarantine. */
	if (redzone->quarantine_flag == 1)
		return true;

	/* Strip asan_slab_free and kmem_cache_free frames. */
	strip_addr = (unsigned long)__builtin_return_address(1);
	asan_save_stack_trace(free_stack, ASAN_STACK_TRACE_FRAMES,
			      strip_addr);

	redzone->free_thread_id = asan_current_thread_id();

	redzone->quarantine_flag = 1;
	asan_quarantine_put(cache, object);

	return false;
}

void asan_kmalloc(struct kmem_cache *cache, void *object, unsigned long size)
{
	unsigned long addr = (unsigned long)object;
	unsigned long object_size = cache->object_size;
	unsigned long rounded_up_object_size =
		round_up(object_size, ASAN_SHADOW_GRAIN);
	unsigned long rounded_down_kmalloc_size =
		round_down(size, ASAN_SHADOW_GRAIN);
	struct asan_redzone *redzone;
	u8 *shadow;

	if (object == NULL)
		return;

	asan_poison_shadow(object, rounded_up_object_size,
			   ASAN_HEAP_KMALLOC_REDZONE);
	asan_unpoison_shadow(object, rounded_down_kmalloc_size);
	if (rounded_down_kmalloc_size != size) {
		shadow = (u8 *)asan_mem_to_shadow(addr +
						  rounded_down_kmalloc_size);
		*shadow = size & (ASAN_SHADOW_GRAIN - 1);
	}

	if (!ASAN_HAS_REDZONE(cache))
		return;

	redzone = object + rounded_up_object_size;

	redzone->kmalloc_size = size;
}
EXPORT_SYMBOL(asan_kmalloc);

size_t asan_ksize(const void *ptr)
{
	struct kmem_cache *cache;
	const struct asan_redzone *redzone;

	BUG_ON(!ptr);
	if (unlikely(ptr == ZERO_SIZE_PTR))
		return 0;

	cache = virt_to_cache(ptr);
	if (ASAN_HAS_REDZONE(cache)) {
		redzone = ptr + round_up(cache->object_size, ASAN_SHADOW_GRAIN);
		if (redzone->kmalloc_size) {
			BUG_ON(redzone->kmalloc_size > cache->object_size);
			return redzone->kmalloc_size;
		}
	}
	return cache->object_size;
}
EXPORT_SYMBOL(asan_ksize);

void asan_krealloc(void *object, unsigned long new_size)
{
	asan_kmalloc(virt_to_cache(object), object, new_size);
}

void asan_on_kernel_init(void)
{
#if ASAN_TESTS_ENABLE
	asan_do_bo();
	asan_do_bo_left();
	asan_do_bo_kmalloc();
	asan_do_bo_kmalloc_node();
	asan_do_bo_krealloc();
	asan_do_bo_krealloc_less();
	asan_do_bo_16();
	asan_do_bo_4mb();
	asan_do_krealloc_more();
	asan_do_uaf();
	asan_do_uaf_quarantine();
	asan_do_uaf_memset();
#endif
}

void __kasan_read1(unsigned long addr)
{
	asan_check_memory_word(addr, 1, false);
}
EXPORT_SYMBOL(__kasan_read1);

void __kasan_read2(unsigned long addr)
{
	asan_check_memory_word(addr, 2, false);
}
EXPORT_SYMBOL(__kasan_read2);

void __kasan_read4(unsigned long addr)
{
	asan_check_memory_word(addr, 4, false);
}
EXPORT_SYMBOL(__kasan_read4);

void __kasan_read8(unsigned long addr)
{
	asan_check_memory_word(addr, 8, false);
}
EXPORT_SYMBOL(__kasan_read8);

void __kasan_read16(unsigned long addr)
{
	asan_check_memory_region((void *)addr, 16, false);
}
EXPORT_SYMBOL(__kasan_read16);

void __kasan_write1(unsigned long addr)
{
	asan_check_memory_word(addr, 1, true);
}
EXPORT_SYMBOL(__kasan_write1);

void __kasan_write2(unsigned long addr)
{
	asan_check_memory_word(addr, 2, true);
}
EXPORT_SYMBOL(__kasan_write2);

void __kasan_write4(unsigned long addr)
{
	asan_check_memory_word(addr, 4, true);
}
EXPORT_SYMBOL(__kasan_write4);

void __kasan_write8(unsigned long addr)
{
	asan_check_memory_word(addr, 8, true);
}
EXPORT_SYMBOL(__kasan_write8);

void __kasan_write16(unsigned long addr)
{
	asan_check_memory_region((void *)addr, 16, true);
}
EXPORT_SYMBOL(__kasan_write16);
