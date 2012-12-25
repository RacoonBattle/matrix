#include <types.h>
#include <stddef.h>
#include <string.h>
#include "matrix/matrix.h"
#include "mm/mmu.h"
#include "mm/malloc.h"
#include "mm/slab.h"
#include "debug.h"
#include "kd.h"
#include "mutex.h"
#include "proc/thread.h"
#include "proc/process.h"
#include "bitmap.h"

uint32_t _value = 0;
struct mutex _mutex;

static void unit_test_thread(void *ctx)
{
	struct mutex *m;

	m = (struct mutex *)ctx;

	DEBUG(DL_DBG, ("unit test thread(%s) is running.\n", CURR_THREAD->name));

	while (_value < 0x00000FFF) {
		mutex_acquire(m);
		_value++;
		DEBUG(DL_DBG, ("thread(%s) mutex(%p:%s) acquired, value(%x).\n",
			       CURR_THREAD->name, m, m->name, _value));
		mutex_release(m);
	}
}

int unit_test(uint32_t round)
{
	int i, r, rc = 0;
	slab_cache_t ut_cache;
	void *obj[4];
	struct spinlock lock;
	void *buf_ptr;
	ptr_t start;
	size_t size;
	struct bitmap bm;
	u_long *bm_buf;
	char *pch;

	/* Kernel memory pool test */
	buf_ptr = kmalloc(4321, 0);
	if (!buf_ptr) {
		DEBUG(DL_DBG, ("malloc from kernel pool failed.\n"));
		goto out;
	} else {
		memset(buf_ptr, 0, 4321);
		pch = buf_ptr;
		for (i = 0; i < 4321; i++) {
			ASSERT(pch[i] == 0);
		}
		kfree(buf_ptr);
	}


	/* Memory map test */
	start = 0x40000000;
	size = 0x4000;
	rc = mmu_map(CURR_PROC->mmu_ctx, start, size, MAP_READ_F|MAP_WRITE_F|MAP_FIXED_F, NULL);
	if (rc != 0) {
		DEBUG(DL_DBG, ("mmu_map failed.\n"));
		goto out;
	} else {
		memset((void *)start, 0, size);
		rc = mmu_unmap(CURR_PROC->mmu_ctx, start, size);
		ASSERT(rc == 0);
	}
	DEBUG(DL_DBG, ("memory map test finished.\n"));
	

	/* Spinlock test */
	spinlock_init(&lock, "ut-lock");
	spinlock_acquire(&lock);
	spinlock_release(&lock);
	DEBUG(DL_DBG, ("spinlock test finished.\n"));


	/* Bitmap test */
	bm_buf = kmalloc(256/8, 0);
	memset((void *)bm_buf, 0, 256/8);
	for (i = 0; i < (256/(8*sizeof(u_long))); i++) {
		ASSERT(bm_buf[i] == 0);
	}
	bitmap_init(&bm, bm_buf, 256);
	bitmap_set(&bm, 34);
	ASSERT(bitmap_test(&bm, 34));
	bitmap_clear(&bm, 34);
	ASSERT(!bitmap_test(&bm, 34));
	kfree(bm_buf);
	DEBUG(DL_DBG, ("bitmap test finished.\n"));
	

	/* Create a slab cache to test the slab allocator */
	r = 0;
	memset(obj, 0, sizeof(obj));
	slab_cache_init(&ut_cache, "ut-cache", 256, NULL, NULL, 0);
	while (TRUE) {
		for (i = 0; i < 4; i++) {
			ASSERT(obj[i] == NULL);
			obj[i] = slab_cache_alloc(&ut_cache);
			if (!obj[i]) {
				DEBUG(DL_INF, ("slab_cache_alloc failed.\n"));
				goto out;
			} else {
				memset(obj[i], 0, 256);
			}
		}

		for (i = 0; i < 4; i++) {
			if (obj[i]) {
				slab_cache_free(&ut_cache, obj[i]);
				obj[i] = NULL;
			}
		}
		
		r++;
		if (r > round) {
			break;
		}
	}
	DEBUG(DL_DBG, ("slab cache test finished with round %d.\n", round));


	/* Create two kernel thread to do the mutex test */
	mutex_init(&_mutex, "ut-mutex", 0);
	rc = thread_create("unit-test1", CURR_PROC, 0, unit_test_thread, &_mutex, NULL);
	if (rc != 0) {
		DEBUG(DL_DBG, ("thread_create ut1 failed, err(%d).\n", rc));
		goto out;
	}
	rc = thread_create("unit-test2", CURR_PROC, 0, unit_test_thread, &_mutex, NULL);
	if (rc != 0) {
		DEBUG(DL_DBG, ("thread_create ut2 failed, err(%d).\n", rc));
		goto out;
	}
	DEBUG(DL_DBG, ("mutex test finished.\n"));


 out:
	for (i = 0; i < 4; i++) {
		if (obj[i]) {
			slab_cache_free(&ut_cache, obj[i]);
			obj[i] = NULL;
		}
	}
	slab_cache_delete(&ut_cache);
	
	return rc;
}