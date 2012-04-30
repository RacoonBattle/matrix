#include <types.h>
#include <stddef.h>
#include <time.h>
#include "matrix/matrix.h"
#include "matrix/const.h"
#include "matrix/global.h"
#include "hal/isr.h"
#include "hal/hal.h"
#include "pit.h"
#include "proc/task.h"
#include "proc/sched.h"
#include "matrix/debug.h"
#include "tsc.h"
#include "cpu.h"
#include "clock.h"
#include "div64.h"

#define LEAPYEAR(y)	(((y) % 4) == 0 && (((y) % 100) != 0 || ((y) % 400) == 0))
#define DAYS(y)		(LEAPYEAR(y) ? 366 : 365)

/* Table containing number of days before a month */
static int _days_before_month[] = {
	0,
	0,
	31,
	31 + 28,
	31 + 28 + 31,
	31 + 28 + 31 + 30,
	31 + 28 + 31 + 30 + 31,
	31 + 28 + 31 + 30 + 31 + 30,
	31 + 28 + 31 + 30 + 31 + 30 + 31,
	31 + 28 + 31 + 30 + 31 + 30 + 31 + 31,
	31 + 28 + 31 + 30 + 31 + 30 + 31 + 31 + 30,
	31 + 28 + 31 + 30 + 31 + 30 + 31 + 31 + 30 + 31,
	31 + 28 + 31 + 30 + 31 + 30 + 31 + 31 + 30 + 31 + 30,
};

/* The number of microseconds since the Epoch the kernel was booted at */
static useconds_t _boot_time;

/* Hardware timer devices */
struct timer *_timers = NULL;

useconds_t system_time()
{
	ASSERT(CURR_CPU->arch.cycles_per_us != 0);
	
	return (useconds_t)((uint32_t)(x86_rdtsc() - CURR_CPU->arch.system_time_offset) /
			    CURR_CPU->arch.cycles_per_us);
}

void init_tsc_target()
{
	if (CURR_CPU == &_boot_cpu)
		CURR_CPU->arch.system_time_offset = x86_rdtsc();
}

useconds_t time_to_unix(uint32_t year, uint32_t mon, uint32_t day,
			uint32_t hour, uint32_t min, uint32_t sec)
{
	uint32_t seconds = 0;
	uint32_t i;

	seconds += sec;
	seconds += min * 60;
	seconds += hour * 60 * 60;
	seconds += (day - 1) * 24 * 60 * 60;

	/* Convert the month into seconds */
	seconds += _days_before_month[mon] * 24 * 60 * 60;

	/* If this is a leap year, and we've past February, we need to add
	 * another day.
	 */
	if (mon > 2 && LEAPYEAR(year))
		seconds += 24 * 60 * 60;

	/* Add the days in each year before this year from 1970 */
	for (i = 1970; i < year; i++)
		seconds += DAYS(i) * 24 * 60 * 60;

	return SECS2USECS(seconds);
}

boolean_t do_clocktick()
{
	useconds_t time;
	struct list *iter, *n;
	boolean_t preempt;
	
	if (!CURR_CPU->timer_enabled)
		return FALSE;

	time = system_time();
	preempt = FALSE;
	
	spinlock_acquire(&CURR_CPU->timer_lock);
	
	/* Check if a clock timer on the current CPU is expired and call its
	 * callback function
	 */
	LIST_FOR_EACH_SAFE(iter, n, &CURR_CPU->timers) {
		;
	}

	spinlock_release(&CURR_CPU->timer_lock);

	return preempt;
}

void init_clock()
{
	/* Initialize the boot time */
	_boot_time = platform_time_from_cmos() - system_time();
	DEBUG(DL_DBG, ("Boot time: %ld microseconds\n", _boot_time));
}
