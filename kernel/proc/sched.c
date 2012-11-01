#include <types.h>
#include <stddef.h>
#include <string.h>
#include "matrix/debug.h"
#include "matrix/const.h"
#include "hal/hal.h"
#include "hal/cpu.h"
#include "hal/spinlock.h"
#include "mm/malloc.h"
#include "sys/time.h"
#include "timer.h"
#include "proc/process.h"
#include "proc/sched.h"

/* Number of priority levels */
#define NR_PRIORITIES	32

/* Time quantum to give to threads */
#define THREAD_QUANTUM	32

/* Run queue structure */
struct sched_queue {
	uint32_t bitmap;			// Bitmap of queues with data
	struct list threads[NR_PRIORITIES];	// Queues of runnable threads
};
typedef struct sched_queue sched_queue_t;

/* Per-CPU scheduling information structure */
struct sched_cpu {
	struct spinlock lock;			// Lock to protect information/queues
	
	struct thread *prev_thread;		// Previously executed thread
	struct thread *idle_thread;		// Thread scheduled when no other threads runnable

	struct timer timer;			// Preemption timer
	struct sched_queue *active;		// Active queue
	struct sched_queue *expired;		// Expired queue
	struct sched_queue queues[2];		// Active and expired queues
	
	size_t total;				// Total running/ready thread count
};
typedef struct sched_cpu sched_cpu_t;

/* Idle process, this should be per CPU process */
// FixMe: change this to per CPU process
struct process *_idle_proc = NULL;

/* Whether scheduler is ready */
boolean_t _scheduler_ready = FALSE;

/* Total number of running or ready threads across all CPUs */
static int _nr_running_threads = 0;

/* The schedule queue for our kernel */
struct list _ready_queue[NR_PRIORITIES];

/* Dead process queue */
static struct list _dead_processes = {
	.prev = &_dead_processes,
	.next = &_dead_processes
};

/* Pointer to our various task */
struct process *_prev_proc = NULL;
struct process *_curr_proc = NULL;		// Current running process

/* Allocate a CPU for a thread to run on */
static struct cpu *sched_alloc_cpu(struct thread *t)
{
	size_t load, average, total;
	struct cpu *cpu, *other;
	struct list *l;

	/* On UP systems, the only choice is current CPU */
	if (_nr_cpus == 1) {
		return CURR_CPU;
	}

	/* Add 1 to the total number of threads to account for the thread we
	 * are adding.
	 */
	total = _nr_running_threads + 1;
	average = total / _nr_cpus;

	LIST_FOR_EACH(l, &_running_cpus) {
		other = LIST_ENTRY(l, struct cpu, link);
		load = other->sched->total;
		if (load < average) {
			cpu = other;
			break;
		}
	}
	
	return cpu;
}

/**
 * Add `p' to one of the queues of runnable processes. This function is responsible
 * for inserting a process into one of the scheduling queues. `p' must not in the
 * scheduling queues before enqueue.
 */
static void sched_enqueue(struct list *queue, struct process *p)
{
	int q;
#ifdef _DEBUG_SCHED
	struct process *proc;
	struct list *l;
#endif	/* _DEBUG_SCHED */

	ASSERT((p->type == PROC_MAGIC) && (p->size == sizeof(struct process)));
	
	/* Determine where to insert the process */
	q = p->priority;

#ifdef _DEBUG_SCHED
	LIST_FOR_EACH(l, &queue[q]) {
		proc = LIST_ENTRY(l, struct process, link);
		if (proc == p) {
			ASSERT(FALSE);
		}
	}
#endif	/* _DEBUG_SCHED */
	
	/* Now add the process to the tail of the queue. */
	ASSERT((q < NR_PRIORITIES) && (q >= 0));
	list_add_tail(&p->link, &queue[q]);
}

/**
 * A process must be removed from the scheduling queues, for example, because it has
 * been blocked.
 */
static void sched_dequeue(struct list *queue, struct process *p)
{
	int q;
#ifdef _DEBUG_SCHED
	size_t found_times = 0;
	struct process *proc;
	struct list *l;
#endif	/* _DEBUG_SCHED */

	ASSERT((p->type == PROC_MAGIC) && (p->size = sizeof(struct process)));

	q = p->priority;

	/* Now make sure that the process is not in its ready queue. Remove the process
	 * if it was found.
	 */
	ASSERT((q < NR_PRIORITIES) && (q >= 0));
	list_del(&p->link);
	
#ifdef _DEBUG_SCHED
	/* You can also just remove the specified process, but I just make sure it is
	 * really in our ready queue.
	 */
	LIST_FOR_EACH(l, &queue[q]) {
		proc = LIST_ENTRY(l, struct process, link);
		if (proc == p) {
			found_times++;
		}
	}

	ASSERT(found_times == 0);		// The process should not be found
#endif	/* _DEBUG_SCHED */
}

static void sched_adjust_priority(struct sched_cpu *c, struct process *p)
{
	;
}

/**
 * Pick a new process from the queue to run
 */
static struct process *sched_pick_process(struct sched_cpu *c)
{
	struct process *p;
	struct list *l;
	int q;

	p = NULL;
	/* Check each of the scheduling queues for ready processes. The number of
	 * queues is defined in process.h. The lowest queue contains IDLE, which
	 * is always ready.
	 */
	for (q = NR_PRIORITIES - 1; q >= 0; q--) {
		if (!LIST_EMPTY(&_ready_queue[q])) {
			l = _ready_queue[q].next;
			p = LIST_ENTRY(l, struct process, link);
			ASSERT((p->type == PROC_MAGIC) &&
			       (p->size == sizeof(struct process)));
			sched_dequeue(_ready_queue, p);
			break;
		}
	}

	return p;
}

void sched_insert_proc(struct process *proc)
{
	sched_enqueue(_ready_queue, proc);
}

/**
 * Picks a new process to run and switches to it. Interrupts must be disable.
 * @param state		- Previous interrupt state
 */
void sched_reschedule(boolean_t state)
{
	struct sched_cpu *c;
	struct process *next;

	/* Check the interrupt state */
	ASSERT(irq_disable() == FALSE);

	/* Get current schedule CPU */
	c = CURR_CPU->sched;

	/* Adjust the priority of the thread based on whether it used up its quantum */
	sched_adjust_priority(c, CURR_PROC);

	/* Enqueue and dequeue the current process to update the process queue */
	if (CURR_PROC->state == PROCESS_RUNNING) {
		sched_enqueue(_ready_queue, CURR_PROC);
	} else if (CURR_PROC->state == PROCESS_DEAD) {
		DEBUG(DL_INF, ("sched_reschedule: p(%p), id(%d), name(%s) dead.\n",
			       CURR_PROC, CURR_PROC->id, CURR_PROC->name));
		list_add_tail(&CURR_PROC->link, &_dead_processes);
	} else {
		DEBUG(DL_DBG, ("sched_reschedule: p(%p), id(%d), name(%s), state(%d).\n",
			       CURR_PROC, CURR_PROC->id, CURR_PROC->name, CURR_PROC->state));
		ASSERT(CURR_PROC->state <= PROCESS_DEAD);
	}
	
	/* Find a new process to run. A NULL return value means no processes are
	 * ready, so we schedule the idle process in this case.
	 */
	next = sched_pick_process(c);
	ASSERT(next != NULL);
	next->quantum = P_QUANTUM;

	/* Move the next process to running state and set it as the current */
	_prev_proc = CURR_PROC;
	CURR_PROC = next;

	/* Perform the process switch if current process is not the same as previous
	 * one.
	 */
	if (CURR_PROC != _prev_proc) {
		DEBUG(DL_DBG, ("sched_reschedule: curr(%s:%d:%p) prev(%s:%d:%p).\n",
			       CURR_PROC->name, CURR_PROC->id, CURR_PROC->arch.kstack,
			       _prev_proc->name, CURR_CPU->id, _prev_proc->arch.kstack));
		
		/* Switch the address space. */
		mmu_switch_ctx(CURR_PROC->mmu_ctx);

		/* Perform the process switch */
		process_switch(CURR_PROC, _prev_proc);

		/* Restore the IRQ */
		irq_restore(state);
	} else {
		irq_restore(state);
	}
}

static void sched_reaper_proc(void *ctx)
{
	/* If this is the first time reaper run, you should enable IRQ first */
	while (TRUE) {
		/* Reaper the dead threads */
		if (!LIST_EMPTY(&_dead_processes)) {
			struct list *p, *l;
			struct process *proc;
			
			LIST_FOR_EACH_SAFE(l, p, &_dead_processes) {
				proc = LIST_ENTRY(l, struct process, link);
				ASSERT((proc->type == PROC_MAGIC) &&
				       (proc->size == sizeof(struct process)));

				list_del(&proc->link);
				DEBUG(DL_INF, ("sched_reaper_proc: destroy process(%d:%s).\n",
					       proc->id, proc->name));
				process_destroy(proc);
			}
		}
		
		sched_reschedule(FALSE);
	}
}

static void sched_idle_proc(void *ctx)
{
	/* We run the loop with interrupts disabled. The cpu_idle() function
	 * is expected to re-enable interrupts as required.
	 */
	irq_disable();

	while (TRUE) {
		sched_reschedule(FALSE);
		cpu_idle();
	}
}

void init_sched_percpu()
{
	int i, j, rc = -1;
	char name[P_NAME_LEN];

	/* Initialize the scheduler for the current CPU */
	CURR_CPU->sched = kmalloc(sizeof(struct sched_cpu), 0);
	ASSERT(CURR_CPU->sched != NULL);
	
	CURR_CPU->sched->total = 0;
	CURR_CPU->sched->active = &CURR_CPU->sched->queues[0];
	CURR_CPU->sched->expired = &CURR_CPU->sched->queues[1];

	/* Initialize the Per CPU priority queues for process */
	for (i = 0; i < NR_PRIORITIES; i++) {
		LIST_INIT(&_ready_queue[i]);
	}

	/* Create the per CPU idle process */
	strcpy(name, "idle-");
	rc = process_create(name, NULL, 14, sched_idle_proc, &_idle_proc);
	ASSERT((rc == 0) && (_idle_proc != NULL));
	DEBUG(DL_DBG, ("init_sched_percpu: p(%p).\n", _idle_proc));

	/* Create the preemption timer */
	init_timer(&CURR_CPU->sched->timer);

	/* Initialize queues */
	for (i = 0; i < 2; i++) {
		CURR_CPU->sched->queues[i].bitmap = 0;
		for (j = 0; j < NR_PRIORITIES; j++) {
			LIST_INIT(&CURR_CPU->sched->queues[i].threads[j]);
		}
	}
}

void init_sched()
{
	int rc = -1;
	struct process *p = NULL;

	/* Create kernel mode reaper process for the whole system */
	rc = process_create("reaper", NULL, 15, sched_reaper_proc, &p);
	ASSERT((rc == 0) && (p != NULL));

	/* We don't need to disable IRQ here because before we enter scheduler
	 * no process will be scheduled
	 */
	sched_insert_proc(p);

	DEBUG(DL_DBG, ("init_sched: sched queues initialization done.\n"));
}

void sched_enter()
{
	boolean_t state;

	/* Disable irq first as sched_insert_proc and process_switch requires */
	state = irq_disable();

	CURR_PROC = _idle_proc;
	ASSERT(CURR_PROC != NULL);

	DEBUG(DL_DBG, ("sched_enter: idle process(%p), ctx(%p).\n",
		       CURR_PROC, CURR_PROC->mmu_ctx));

	/* Set scheduler to ready */
	_scheduler_ready = TRUE;
	
	/* Switch to current process */
	process_switch(CURR_PROC, NULL);
}
