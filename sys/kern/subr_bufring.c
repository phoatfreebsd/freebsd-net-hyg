/*-
 * Copyright (c) 2007-2015 Kip Macy <kmacy@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/counter.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/ktr.h>

#include <sys/buf_ring.h>
#include <sys/buf_ring_sc.h>

#define ALIGN_SCALE (CACHE_LINE_SIZE/sizeof(caddr_t))

static struct buf_ring *
buf_ring_alloc_(int count, struct malloc_type *type, int flags, struct mtx *lock, int brflags)
{
	struct buf_ring *br;
	int alloc_count;

	KASSERT(powerof2(count), ("buf ring must be size power of 2"));
	alloc_count = (brflags & BR_FLAGS_ALIGNED) ? (count * ALIGN_SCALE) : count; 

	br = malloc(sizeof(struct buf_ring) + alloc_count*sizeof(caddr_t),
	    type, flags|M_ZERO);
	if (br == NULL)
		return (NULL);
	br->br_flags = brflags;
#ifdef DEBUG_BUFRING
	br->br_lock = lock;
#endif	
	br->br_prod_size = br->br_cons_size = count;
	br->br_prod_mask = br->br_cons_mask = count-1;
	br->br_prod_head = br->br_cons_head = 0;
	br->br_prod_tail = br->br_cons_tail = 0;

	return (br);
}

struct buf_ring *
buf_ring_alloc(int count, struct malloc_type *type, int flags, struct mtx *lock)
{

	return (buf_ring_alloc_(count, type, flags, lock, 0));
}

struct buf_ring *
buf_ring_alloc_aligned(int count, struct malloc_type *type, int flags, struct mtx *lock)
{

	return (buf_ring_alloc_(count, type, flags, lock, BR_FLAGS_ALIGNED));
}

void
buf_ring_free(struct buf_ring *br, struct malloc_type *type)
{

	free(br, type);
}

/*
 * buf_ring_sc definitions follow
 */
#ifndef BR_ALIGN_ENTRIES
#define BR_ALIGN_ENTRIES 0
#endif

struct br_sc_entry_ {
	volatile void *bre_ptr;
#if BR_ALIGN_ENTRIES
	char pad[CACHE_LINE_SIZE-sizeof(void*)];
#endif
};

typedef union prod_state_ {
	uint32_t ps_value;
	struct ps_entries_flags_ {
		uint32_t pe_flags: 2;
		uint32_t pe_head: 30;
	} ps_entries;
} prod_state;

#define ps_flags ps_entries.pe_flags
#define ps_head ps_entries.pe_head

#define br_prod_value br_prod_state.ps_value
#define br_prod_flags br_prod_state.ps_flags
#define br_prod_head br_prod_state.ps_head

#define BR_RING_ABDICATING (1<<31)
#define BR_RING_STALLED    (1<<30)
#define BR_RING_IDLE       (1<<29)
#define BR_RING_MAX        (1<<28)
#define BR_RING_MASK       (BR_RING_MAX-1)
#define BR_RING_FLAGS_MASK       (~(BR_RING_MAX-1))

#define BR_INDEX(x) ((x) & BR_RING_MASK)
#define BR_HANDOFF(br) ((br)->br_cons & (BR_RING_ABDICATING|BR_RING_IDLE))
#define BR_STALLED(br) ((br)->br_cons & BR_RING_STALLED)
#define BR_PENDING      (1<<0)
#define BR_OWNED        (1<<1)
#define BR_RING_OWNED   (1<<31)
#define BR_RING_PENDING (1<<30)


struct buf_ring_sc {
	volatile prod_state	br_prod_state;
	volatile uint32_t	br_prod_tail;
	int              	br_prod_size;
	int              	br_prod_mask;
	volatile void *br_pending_buf;
	counter_u64_t		br_enqueues;
	counter_u64_t		br_drops;
	counter_u64_t		br_starts;
	counter_u64_t		br_restarts;
	/* cache line aligned to avoid cache line invalidate traffic
	 * between consumer and producer (false sharing)
	 *
	 */
	volatile uint32_t	br_cons __aligned(CACHE_LINE_SIZE);
	counter_u64_t		br_abdications;
	counter_u64_t		br_stalls;
	int		 	br_cons_size;
	int              	br_cons_mask;
	int			br_flags;
	/* cache line aligned to avoid false sharing with other data structures
	 * located just beyond the end of the ring
	 */
	struct br_sc_entry_	br_ring[0] __aligned(CACHE_LINE_SIZE);
};

/*
 * Many architectures other than x86 permit speculative re-ordering
 * of loads. Unfortunately, atomic_load_acq_32() is comparatively
 * expensive so we'd rather elide it if possible.
 */
#if defined(__i386__) || defined(__amd64__)
#define ORDERED_LOAD_32(x) (*x)
#else
#define ORDERED_LOAD_32(x) atomic_load_acq_32((x))
#endif


/*
 * ring entry accessors to allow us to make ring entry
 * alignment determined at runtime
 */
static __inline void *
brsc_entry_get(struct buf_ring_sc *br, int i)
{
	volatile void *ent;

	if (br->br_flags & BR_FLAGS_ALIGNED)
		ent = br->br_ring[i*ALIGN_SCALE].bre_ptr;
	else
		ent = br->br_ring[i].bre_ptr;
	return ((void *)ent);
}

static __inline void
brsc_entry_set(struct buf_ring_sc *br, int i, void *buf)
{

	if (br->br_flags & BR_FLAGS_ALIGNED)
		br->br_ring[i*ALIGN_SCALE].bre_ptr = buf;
	else
		br->br_ring[i].bre_ptr = buf;
}



struct buf_ring_sc *
buf_ring_sc_alloc(int count, struct malloc_type *type, int flags, int brflags)
{
	struct buf_ring_sc *br;
	int alloc_count = count;

	KASSERT(powerof2(count), ("buf ring must be size power of 2"));
#if BR_ALIGN_ENTRIES == 0
	if (brflags & BR_FLAGS_ALIGNED)
		alloc_count = count*ALIGN_SCALE;
#endif
	br = malloc(sizeof(struct buf_ring) + alloc_count*sizeof(caddr_t),
	    type, flags|M_ZERO);
	if (br == NULL)
		return (NULL);

	br->br_flags = brflags;
	br->br_prod_size = br->br_cons_size = count;
	br->br_prod_mask = br->br_cons_mask = count-1;
	br->br_prod_value = br->br_prod_tail = 0;
	br->br_cons = 0;
	br->br_enqueues = counter_u64_alloc(flags);
	br->br_drops = counter_u64_alloc(flags);
	br->br_abdications = counter_u64_alloc(flags);
	br->br_stalls = counter_u64_alloc(flags);
	br->br_starts = counter_u64_alloc(flags);
	br->br_restarts = counter_u64_alloc(flags);
	buf_ring_sc_reset_stats(br);
	return (br);
}

void
buf_ring_sc_free(struct buf_ring_sc *br, struct malloc_type *type)
{
	counter_u64_free(br->br_enqueues);
	counter_u64_free(br->br_drops);
	counter_u64_free(br->br_abdications);
	counter_u64_free(br->br_stalls);
	counter_u64_free(br->br_starts);
	counter_u64_free(br->br_restarts);

	free(br, type);
}

void
buf_ring_sc_reset_stats(struct buf_ring_sc *br)
{

	counter_u64_zero(br->br_enqueues);
	counter_u64_zero(br->br_drops);
	counter_u64_zero(br->br_abdications);
	counter_u64_zero(br->br_stalls);
	counter_u64_zero(br->br_starts);
	counter_u64_zero(br->br_restarts);
}


void
buf_ring_sc_get_stats_v0(struct buf_ring_sc *br, struct buf_ring_sc_stats_v0 *brss)
{
	brss->brs_enqueues = counter_u64_fetch(br->br_enqueues);
	brss->brs_drops = counter_u64_fetch(br->br_drops);
	brss->brs_abdications = counter_u64_fetch(br->br_abdications);
	brss->brs_stalls = counter_u64_fetch(br->br_stalls);
	brss->brs_starts = counter_u64_fetch(br->br_starts);
	brss->brs_restarts = counter_u64_fetch(br->br_restarts);
}
/*
 * Multi-producer safe lock-free ring buffer enqueue
 *
 * Most architectures do not support the atomic update of multiple
 * discontiguous locations. So it is not possible to atomically update
 * the producer index and ring buffer entry. To side-step this limitation
 * we split update in to 3 steps:
 *      1) atomically acquiring an index
 *      2) updating the corresponding ring entry
 *      3) making the update available to the consumer
 * In order to split the index update in to an acquire and release
 * phase there are _two_ producer indexes. 'prod_head' is used for
 * step 1) and is thus only used by the enqueue itself. 'prod_tail'
 * is used for step 3) to signal to the consumer that the update is
 * complete. To guarantee memory ordering the update of 'prod_tail' is
 * done with a atomic_store_rel_32(...) and the corresponding
 * initial read of 'prod_tail' by the dequeue functions is done with
 * an atomic_load_acq_32(...).
 *
 * Regarding memory ordering - there are five variables in question:
 * (br_) prod_head, prod_tail, cons, ring[idx={cons, prod}]
 * It's easiest examine correctness by considering the consequence of
 * reading a stale value or having an update become visible prior to
 * preceding writes.
 *
 * - prod_head: this is only read by the enqueue routine, if the latter were to
 *   initially read a stale value for it the cmpxchg (atomic_cmpset_acq_32)
 *   would fail. However, the implied memory barrier in cmpxchg would cause the
 *   subsequent read of prod_head to read the up-to-date value permitting the
 *   cmpxchg to succeed the second time.
 *
 * - prod_tail: This value is used by dequeue to determine the effective
 *   producer index. On architectures with weaker memory ordering than x86 it
 *   needs special handling. In enqueue it needs to be updated with
 *   atomic_store_rel_32() (i.e. a write memory barrier before update) to
 *   guarantee that the new ring value is committed to memory before it is
 *   made available by prod_tail. In dequeue to guarantee that it is read before
 *   br_ring[cons] it needs to be read with atomic_load_acq_32().
 *
 *
 * - cons: This is used to communicate the latest consumer index between
 *   dequeue and enqueue. Reading a stale value in enqueue can cause an enqueue
 *   to fail erroneously. To avoid a load being re-ordered after a store (and
 *   thus permitting enqueue to store a new value before the old one has been
 *   consumed) it is updated with an atomic_store_rel_32() in deqeueue.
 *
 * - ring[idx] : Updates to this value need to reach memory before the subsequent
 *   update to prod_tail does. Reads need to happen before subsequent updates to
 *   cons.
 *
 * Some implementation notes:
 * - Much like a simpler single-producer single consumer ring buffer,
 *   the producer can not produce faster than the consumer. Hence the
 *   check of 'prod_head' + 1 against 'cons'.
 *
 * - The use of "prod_next = (prod_head + 1) & br->br_prod_mask" to
 *   calculate the next index is slightly cheaper than a modulo but
 *   requires the ring to be power-of-2 sized.
 *
 * - The critical_enter() / critical_exit() are not required for
 *   correctness. They prevent updates from stalling by having a producer be
 *   preempted after updating 'prod_head' but before updating 'prod_tail'.
 *
 * - The "while (br->br_prod_tail != prod_head)"
 *   check assures in order completion allows us to update
 *   'prod_tail' without a cmpxchg / LOCK prefix assures in order
 *   completion as a later producer might reach this point before an
 *   earlier consumer.
 *
 *
 *   This buf_ring has the following FSM:
 *     producer: 
 *     -  !owned              -> owned(curthread) + enqueue
 *     -  pending(!curthread) -> enqueue 
 *     -  owned + abdicating  -> owned + abdicating + pending(curthread)
 *     -  owned + abdicating + pending(curthread) -> 
 *            owned + abdicating + pending(curthread) + enqueue
 *     -  owned + abdicating + pending(curthread) + enqueue  ->
 *           wait(!owned)
 *     - !owned + abdicating + pending(curthread) ->
 *        owned + busy + pending(curthread) -> 
 *        owned + busy (consumer) 
 *     consumer (i.e. owned(curthread)):
 *      -  busy + owned -> abdicating + owned
 *      -  abdicating + owned + pending -> abdicating + unowned + pending
 *      -  abdicating + owned -> abdicating + unowned + enqueue tx task
 *      How do we handle abdication when the ring is full
 */
int
buf_ring_sc_enqueue(struct buf_ring_sc *br, void *buf)
{
	uint32_t prod_head, prod_next, cons, value;
	uint32_t pidx, cidx;
	int pending, rc;
	prod_state state;
#ifdef DEBUG_BUFRING
	int i;
	for (i = br->br_cons; i != ORDERED_LOAD_32(&br->br_prod_tail);
	     i = ((i + 1) & br->br_cons_mask))
		if(brsc_entry_get(br, i) == buf)
			panic("buf=%p already enqueue at %d prod=%d cons=%d",
			    buf, i, br->br_prod_tail, br->br_cons);
#endif
	critical_enter();

	state = br->br_prod_state;
	pending = false;
	rc = 0;
	/*
	 * If the current consumer abdicated we loop until the pending bit is
	 * set and if we set it we're the next lock holder - or if the owner
	 * drops the lock before we can do that then the lock will be
	 * re-acquired normally
	 */
	while (BR_HANDOFF(br) && state.ps_flags == BR_OWNED) {
		value = state.ps_value;
		state.ps_flags |= BR_PENDING;
		if (atomic_cmpset_acq_32(&br->br_prod_value, value, state.ps_value)) {
			pending = true;
			break;
		}
		state = br->br_prod_state;
	}
	do {
		rc = 0;
		prod_head = br->br_prod_head;
		pidx = BR_INDEX(prod_head);
		cons = br->br_cons;
		cidx = BR_INDEX(cons);
		prod_next = (pidx + 1) & br->br_prod_mask;


		if (prod_next == cidx) {
			/* ensure that we only return ENOBUFS
			 * if the latest value matches what we read
			 */
			if (pidx != BR_INDEX(atomic_load_acq_32(&br->br_prod_value)) ||
			    cidx != BR_INDEX(atomic_load_acq_32(&br->br_cons)))
				continue;

			if (pending) {
				/* no space in ring - but we're the next owner
				 * acquire the lock and insert it as the pending buf
				 */
				while ((br->br_prod_flags & BR_OWNED) == BR_OWNED)
					cpu_spinwait();
				atomic_set_acq_32(&br->br_prod_value, BR_RING_OWNED);
				br->br_cons &= ~(BR_RING_ABDICATING|BR_RING_IDLE);
				atomic_clear_rel_32(&br->br_prod_value, BR_RING_PENDING);
				br->br_pending_buf = buf;
				counter_u64_add(br->br_enqueues, 1);
				return (EOWNED);
			}

			critical_exit();
			counter_u64_add(br->br_drops, 1);
			return (ENOBUFS);
		}

		if (BR_STALLED(br))
			rc = ESTALLED;
		else if (pending) {
			prod_next |= BR_RING_OWNED|BR_RING_PENDING;
			rc = EOWNED;
		} else if (state.ps_flags == 0) {
			prod_next |= BR_RING_OWNED;
			rc = EOWNED;
		} else
			prod_next |= (state.ps_value & BR_RING_FLAGS_MASK);

		/*
		 * no point in doing the CAS until it might succeed
		 */
		if (pending)
			while ((br->br_prod_flags & BR_OWNED) == BR_OWNED)
				cpu_spinwait();

		/*
		 * If there is no owner we need to loop until there is an owner
		 * and return true if we're the one to set it
		 */

	} while (!atomic_cmpset_acq_32(&br->br_prod_value, prod_head, prod_next));
	if (rc == EOWNED)
		br->br_cons &= BR_RING_MASK;
    /*
	 * we became owner by way of the contested abdicate clear pending
	 */
	if (pending)
		atomic_clear_rel_32(&br->br_prod_value, BR_RING_PENDING);

#ifdef DEBUG_BUFRING
	if (brsc_entry_get(br, prod_head) != NULL)
		panic("dangling value in enqueue");
#endif
	brsc_entry_set(br, prod_head, buf);

	/*
	 * If there are other enqueues in progress
	 * that preceded us, we need to wait for them
	 * to complete
	 * re-ordering of reads would not effect correctness
	 */
	while (br->br_prod_tail != prod_head)
		cpu_spinwait();
	/* ensure  that the ring update reaches memory before the new
	 * value of prod_tail
	 */
	atomic_store_rel_32(&br->br_prod_tail, prod_next);

	critical_exit();
	counter_u64_add(br->br_enqueues, 1);
	return (rc);
}

/*
 * populate ents with up to count values from the ring
 * and return the number of entries
 */int
buf_ring_sc_peek(struct buf_ring_sc *br, void *ents[], uint16_t count)
{
	uint32_t cons;
	uint32_t prod_tail;
	int avail;
	int cidx, i = 0;

	KASSERT(count > 0, ("peeking for zero entries"));
	KASSERT(br->br_prod_flags == BR_OWNED, ("peeking without lock being held"));
	if (__predict_false(br->br_pending_buf != NULL)) {
		ents[0] = (void *)br->br_pending_buf;
		i = 1;
	}
	/*
	 * for correctness prod_tail must be read before ring[cons]
	 */
	cons = br->br_cons;
	prod_tail = ORDERED_LOAD_32(&br->br_prod_tail);
	avail = prod_tail - cons;

	if (avail == 0)
		return (i);
	if (avail < 0)
		avail += br->br_prod_size;
	if (avail > count)
		avail = count;
	for (cidx = 0; i < avail; i++, cidx++)
		ents[i] = brsc_entry_get(br, (cons + cidx) & br->br_cons_mask);

	return (avail);
}

/*
 * Used to return a buffer (most likely already there)
 * to the top od the ring. The caller should *not*
 * have used any dequeue to pull it out of the ring
 * but instead should have used the peek() function.
 * This is normally used where the transmit queue
 * of a driver is full, and an mubf must be returned.
 * Most likely whats in the ring-buffer is what
 * is being put back (since it was not removed), but
 * sometimes the lower transmit function may have
 * done a pullup or other function that will have
 * changed it. As an optimization we always put it
 * back (since jhb says the store is probably cheaper),
 * if we have to do a multi-queue version we will need
 * the compare and an atomic.
 *
 */
void
buf_ring_sc_putback(struct buf_ring_sc *br, void *new, int idx)
{
	KASSERT(br->br_cons != br->br_prod_tail,
			("Buf-Ring has none in putback")) ;
	if (idx == 0 & br->br_pending_buf != NULL)
		br->br_pending_buf = new;
	else
		brsc_entry_set(br, br->br_cons + idx, new);
}

/*
 * @count: the number of entries by which to advance the consumer index
 *
 */
void
buf_ring_sc_advance(struct buf_ring_sc *br, int count)
{
	uint32_t cons, cons_next;
	uint32_t prod_tail;
	int i, advance_count;

	advance_count = count;
	KASSERT(count > 0, ("invalid advance count"));
	if (__predict_false(br->br_pending_buf != NULL)) {
		br->br_pending_buf = NULL;
		advance_count--;
	}
	cons = br->br_cons;
	prod_tail = br->br_prod_tail;
	cons_next = (cons + advance_count) & br->br_cons_mask;

	/*
	 * Storing NULL here serves two purposes:
	 * 1) it assures that the load of ring[cons] has completed
	 *    (only the most perverted architecture or compiler would
	 *    consider re-ordering a = *x; *x = b)
	 * 2) it allows us to enforce global ordering of the cons
	 *    update with an atomic_store_rel_32
	 */
	for (i = 0; i < advance_count; i++)
		brsc_entry_set(br, (cons + i) & br->br_cons_mask, NULL);

	atomic_store_rel_32(&br->br_cons, cons_next);
}

/*
 * mark the ring as being abdicated
 */
void
buf_ring_sc_abdicate(struct buf_ring_sc *br)
{
	uint32_t cons_next;

	cons_next = br->br_cons | BR_RING_ABDICATING;
	counter_u64_add(br->br_abdications, 1);
	critical_enter();

	atomic_store_rel_32(&br->br_cons, cons_next);
}

int
buf_ring_sc_count(struct buf_ring_sc *br)
{
	/*  br_cons and br_prod_tail may be stale but the consumer
	 * understands that this is only a point in time snapshot
	 */

	return ((br->br_prod_size + br->br_prod_tail - BR_INDEX(br->br_cons))
	    & br->br_prod_mask);
}

int
buf_ring_sc_empty(struct buf_ring_sc *br)
{
	/*  br_prod_tail may be stale but the consumer understands that this is
	*  only a point in time snapshot
	*/

	return (BR_INDEX(br->br_cons) == br->br_prod_tail);
}

int
buf_ring_sc_full(struct buf_ring_sc *br)
{
	/* br_cons may be stale but the caller understands that this is
	* only a point in time snapshot
	*/
	return (((br->br_prod_tail + 1) & br->br_prod_mask) == BR_INDEX(br->br_cons));
}

/*
 * Note that this will block until the current consumer stalls
 * or goes idle without any intervening consumers - thus is only
 * recommend when flushing the ring
 */
void
buf_ring_sc_lock(struct buf_ring_sc *br)
{
	uint32_t value, new_value;

	do {
		while ((value = br->br_prod_value) & BR_RING_PENDING)
			cpu_spinwait();
		new_value = value | BR_RING_PENDING;
	} while (!atomic_cmpset_acq_32(&br->br_prod_value, value, new_value));
	do {
		while ((value = br->br_prod_value) & BR_RING_OWNED)
			cpu_spinwait();
		new_value = value | BR_RING_OWNED;
	} while (!atomic_cmpset_acq_32(&br->br_prod_value, value, new_value));
	if (br->br_cons & BR_RING_IDLE)
		counter_u64_add(br->br_starts, 1);
	else if (br->br_cons & BR_RING_STALLED)
		counter_u64_add(br->br_restarts, 1);
	br->br_cons &= ~(BR_RING_IDLE|BR_RING_ABDICATING|BR_RING_STALLED);
	atomic_clear_rel_32(&br->br_prod_value, BR_RING_PENDING);
}

int
buf_ring_sc_trylock(struct buf_ring_sc *br)
{
	uint32_t value, new_value;

	do {
		if ((value = br->br_prod_value) & (BR_RING_OWNED|BR_RING_PENDING))
			return (0);
		new_value = value | BR_RING_OWNED;
	} while (!atomic_cmpset_acq_32(&br->br_prod_value, value, new_value));

	if (br->br_cons & BR_RING_IDLE)
		counter_u64_add(br->br_starts, 1);
	else if (br->br_cons & BR_RING_STALLED)
		counter_u64_add(br->br_restarts, 1);
	br->br_cons &= ~(BR_RING_IDLE|BR_RING_ABDICATING|BR_RING_STALLED);
	return (1);
}

int
buf_ring_sc_unlock(struct buf_ring_sc *br, br_unlock_reason reason)
{
	prod_state state;
	uint32_t prod_value, cons_next;
	int pending;

	KASSERT(br->br_prod_flags & BR_OWNED, ("unlocking unowned ring"));
	/*
	 * we treat IDLE the same as ABDICATE to avoid a race
	 * with enqueue - they only differ for purposes of stats
	 * keeping
	 */
	if (reason == BR_UNLOCK_IDLE) {
		cons_next = br->br_cons | BR_RING_IDLE;
		critical_enter();
		atomic_store_rel_32(&br->br_cons, cons_next);
	} else if ((reason == BR_UNLOCK_ABDICATE) &&
			   (br->br_cons & BR_RING_ABDICATING) == 0) {
		cons_next = br->br_cons | BR_RING_ABDICATING;
		counter_u64_add(br->br_abdications, 1);
		critical_enter();
		atomic_store_rel_32(&br->br_cons, cons_next);
	} else if (reason == BR_UNLOCK_STALLED) {
		cons_next = br->br_cons | BR_RING_STALLED;
		counter_u64_add(br->br_stalls, 1);
		critical_enter();
		atomic_store_rel_32(&br->br_cons, cons_next);
	}
	do {
		prod_value = state.ps_value = br->br_prod_value;
		pending = !!(state.ps_flags & BR_PENDING);
		state.ps_flags &= ~BR_OWNED;
	} while (!atomic_cmpset_rel_32(&br->br_prod_value, prod_value, state.ps_value));
	critical_exit();
	return (pending);
}
