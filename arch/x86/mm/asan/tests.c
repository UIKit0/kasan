#include <asm/atomic.h>
#include <asm/uaccess.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "asan.h"

/* Expected to produce a report. */
void asan_do_bo(void)
{
	char *ptr;

	pr_err("TEST: out-of-bounds:\n");
	ptr = kmalloc(17, GFP_KERNEL);
	*(ptr + 33) = 'x';
	kfree(ptr);
}

/* Expected to produce a report. */
void asan_do_bo_kmalloc(void)
{
	char *ptr;

	pr_err("TEST: out-of-bounds in kmalloc redzone:\n");
	ptr = kmalloc(17, GFP_KERNEL);
	*(ptr + 18) = 'x';
	kfree(ptr);
}

/* Expected to produce a report. */
void asan_do_bo_kmalloc_node(void)
{
	char *ptr;

	pr_err("TEST: out-of-bounds in kmalloc_node redzone:\n");
	ptr = kmalloc_node(17, GFP_KERNEL, 0);
	*(ptr + 18) = 'x';
	kfree(ptr);
}

/* Expected to produce a report. */
void asan_do_bo_krealloc(void)
{
	char *ptr1, *ptr2;

	pr_err("TEST: out-of-bounds after krealloc:\n");
	ptr1 = kmalloc(17, GFP_KERNEL);
	ptr2 = krealloc(ptr1, 19, GFP_KERNEL);
	ptr2[20] = 'x';
	kfree(ptr2);
}

/* Expected to produce a report. */
void asan_do_bo_krealloc_less(void)
{
	char *ptr1, *ptr2;

	pr_err("TEST: out-of-bounds after krealloc 2:\n");
	ptr1 = kmalloc(17, GFP_KERNEL);
	ptr2 = krealloc(ptr1, 15, GFP_KERNEL);
	ptr2[16] = 'x';
	kfree(ptr2);
}

/* Expected not to produce a report. */
void asan_do_krealloc_more(void)
{
	char *ptr1, *ptr2;

	pr_err("TEST: access addressable memory after krealloc.\n");
	ptr1 = kmalloc(17, GFP_KERNEL);
	ptr2 = krealloc(ptr1, 19, GFP_KERNEL);
	ptr2[18] = 'x';
	kfree(ptr2);
}

/* Expected to produce a report. */
void asan_do_bo_left(void)
{
	char *ptr;

	pr_err("TEST: out-of-bounds to the left:\n");
	ptr = kmalloc(17, GFP_KERNEL);
	*(ptr - 1) = 'x';
	kfree(ptr);
}

/* Expected to produce a report. */
void asan_do_bo_16(void)
{
	struct {
		unsigned long words[2];
	} *ptr1, *ptr2;

	pr_err("TEST: out-of-bounds for 16-bytes access:\n");
	ptr1 = kmalloc(10, GFP_KERNEL);
	ptr2 = kmalloc(16, GFP_KERNEL);
	*ptr1 = *ptr2;
	kfree(ptr1);
	kfree(ptr2);
}

/* Expected to produce a report. */
void asan_do_bo_4mb(void)
{
	char *ptr;

	pr_err("TEST: out-of-bounds in 4mb cache:\n");
	ptr = kmalloc((4 << 20) - 8 * 16 * 5, GFP_KERNEL);
	ptr[0] = ptr[(4 << 20) - 1];
}

/* Expected to produce a report. */
void asan_do_bo_memset(void)
{
	char *ptr;

	pr_err("TEST: out-of-bounds in memset:\n");
	ptr = kmalloc(33, GFP_KERNEL);
	memset(ptr, 0, 40);
	kfree(ptr);
}

/* Expected to produce a report. */
void asan_do_uaf(void)
{
	char *ptr;

	pr_err("TEST: use-after-free:\n");
	ptr = kmalloc(128, GFP_KERNEL);
	kfree(ptr);
	*(ptr + 126 - 64) = 'x';
}

/* Expected to produce a report. */
void asan_do_uaf_memset(void)
{
	char *ptr;

	pr_err("TEST: use-after-free in memset:\n");
	ptr = kmalloc(33, GFP_KERNEL);
	kfree(ptr);
	memset(ptr, 0, 30);
}

/* Expected to produce a report. */
void asan_do_uaf_quarantine(void)
{
	char *ptr1, *ptr2;

	pr_err("TEST: use-after-free in quarantine:\n");
	ptr1 = kmalloc(42, GFP_KERNEL);
	kfree(ptr1);
	ptr2 = kmalloc(42, GFP_KERNEL);
	ptr1[5] = 'x';
	kfree(ptr2);
}

/* Expected to produce a report and cause kernel panic. */
void asan_do_user_memory_access(void)
{
	char *ptr1 = (char *)(1UL << 24);
	char *ptr2;

	pr_err("TEST: user-memory-access:\n");
	ptr2 = kmalloc(10, GFP_KERNEL);
	ptr2[3] = *ptr1;
	kfree(ptr2);
}

/* Expected to produce a report. */
void asan_do_bo_atomic(void)
{
	atomic_t *ptr;

	pr_err("TEST: out-of-bounds in atomic:\n");
	ptr = kmalloc(sizeof(atomic_t), GFP_KERNEL);
	atomic_dec(ptr + 1);
	kfree(ptr);
}

/* Expected to produce a report. */
void asan_do_bo_atomic_rmwcc(void)
{
	atomic_t *ptr;

	pr_err("TEST: out-of-bounds in atomic with RMWcc:\n");
	ptr = kmalloc(sizeof(atomic_t), GFP_KERNEL);
	atomic_dec_and_test(ptr + 1);
	kfree(ptr);
}

/* Expected to produce a report. */
void asan_do_bo_stack(void)
{
	char a[16];
	volatile int sixteen = 16;

	pr_err("TEST: stack-out-of-bounds:\n");
	pr_err("%c\n", a[sixteen]);
}

void asan_do_bo_quarantine_flush(void)
{
	size_t old_size;
	size_t mem_recycled = 0;

	pr_err("TEST: quarantine flush\n");
	old_size = asan_quarantine_size();
	pr_err("Quarantine size %ld\n", old_size);

	while (asan_quarantine_size() >= old_size
			&& mem_recycled < ASAN_QUARANTINE_SIZE * 2) {
		void *p = kmalloc(2048, GFP_KERNEL);

		kfree(p);
		mem_recycled += 2058;
	}
	pr_err("Memory cycled %ld\n", mem_recycled);
	pr_err("New quarantine size %ld\n", asan_quarantine_size());
}

struct test_struct {
	int field;
};

void asan_do_bo_kmem_cache(void)
{
	struct kmem_cache *cache;
	int i;

	pr_err("TEST: quarantine kmem_cache\n");
	cache = KMEM_CACHE(test_struct, SLAB_TRACE);
	for (i = 0; i < 100; i++) {
		void *p = kmem_cache_alloc(cache, GFP_KERNEL);

		kmem_cache_free(cache, p);
	}

	kmem_cache_destroy(cache);
}

void asan_run_tests(void)
{
	asan_do_bo();
	asan_do_bo_left();
	asan_do_bo_kmalloc();
	asan_do_bo_kmalloc_node();
	asan_do_bo_krealloc();
	asan_do_bo_krealloc_less();
	asan_do_krealloc_more();
	asan_do_bo_16();
	asan_do_bo_4mb();
	asan_do_bo_memset();
	asan_do_uaf();
	asan_do_uaf_memset();
	asan_do_uaf_quarantine();
	/* asan_do_user_memory_access(); */
	asan_do_bo_atomic();
	asan_do_bo_atomic_rmwcc();
	asan_do_bo_quarantine_flush();
	asan_do_bo_kmem_cache();
}

/* TODO: remove definition */
void asan_enable_stack(void);

void asan_run_stack(void)
{
	asan_enable_stack();
	asan_do_bo_stack();
}

static ssize_t asan_tests_write(struct file *file, const char __user *buf,
				size_t count, loff_t *offset)
{
	char buffer[16];

	memset(buffer, 0, sizeof(buffer));
	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	if (!strcmp(buffer, "asan_run_tests\n"))
		asan_run_tests();

	if (!strcmp(buffer, "asan_run_stack\n"))
		asan_run_stack();

	if (!strcmp(buffer, "asan_enable\n"))
		asan_enable();
	else if (!strcmp(buffer, "asan_disable\n"))
		asan_disable();
	return count;
}

static const struct file_operations asan_tests_operations = {
	.write		= asan_tests_write,
};

static int __init asan_tests_init(void)
{
	proc_create("kasan_tests", S_IWUSR, NULL, &asan_tests_operations);
	return 0;
}

device_initcall(asan_tests_init);
