/*
 * Copyright (c) 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "i915_drv.h"
#include "intel_drv.h"
#include "i915_scheduler.h"
#include <../drivers/android/sync.h>

#define for_each_scheduler_node(node, id)				\
	list_for_each_entry((node), &scheduler->node_queue[(id)], link)

#define assert_scheduler_lock_held(scheduler)				\
	do {								\
		WARN_ONCE(!spin_is_locked(&(scheduler)->lock), "Spinlock not locked!");	\
	} while(0)

/**
 * i915_scheduler_is_enabled - Returns true if the scheduler is enabled.
 * @dev: DRM device
 */
bool i915_scheduler_is_enabled(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;

	if (!i915.enable_scheduler)
		return false;

	return dev_priv->scheduler != NULL;
}

const char *i915_qe_state_str(struct i915_scheduler_queue_entry *node)
{
	static char	str[50];
	char		*ptr = str;

	*(ptr++) = node->bumped ? 'B' : '-',
	*(ptr++) = i915_gem_request_completed(node->params.request) ? 'C' : '-';

	*ptr = 0;

	return str;
}

char i915_scheduler_queue_status_chr(enum i915_scheduler_queue_status status)
{
	switch (status) {
	case I915_SQS_NONE:
	return 'N';

	case I915_SQS_QUEUED:
	return 'Q';

	case I915_SQS_POPPED:
	return 'X';

	case I915_SQS_FLYING:
	return 'F';

	case I915_SQS_COMPLETE:
	return 'C';

	case I915_SQS_DEAD:
	return 'D';

	default:
	break;
	}

	return '?';
}

const char *i915_scheduler_queue_status_str(
				enum i915_scheduler_queue_status status)
{
	static char	str[50];

	switch (status) {
	case I915_SQS_NONE:
	return "None";

	case I915_SQS_QUEUED:
	return "Queued";

	case I915_SQS_POPPED:
	return "Popped";

	case I915_SQS_FLYING:
	return "Flying";

	case I915_SQS_COMPLETE:
	return "Complete";

	case I915_SQS_DEAD:
	return "Dead";

	case I915_SQS_MAX:
	return "Invalid";

	default:
	break;
	}

	sprintf(str, "[Unknown_%d!]", status);
	return str;
}

const char *i915_scheduler_flag_str(uint32_t flags)
{
	static char str[100];
	char *ptr = str;

	*ptr = 0;

#define TEST_FLAG(flag, msg)						\
	do {								\
		if (flags & (flag)) {					\
			strcpy(ptr, msg);				\
			ptr += strlen(ptr);				\
			flags &= ~(flag);				\
		}							\
	} while (0)

	TEST_FLAG(I915_SF_INTERRUPTS_ENABLED, "IntOn|");
	TEST_FLAG(I915_SF_SUBMITTING,         "Submitting|");
	TEST_FLAG(I915_SF_DUMP_FORCE,         "DumpForce|");
	TEST_FLAG(I915_SF_DUMP_DETAILS,       "DumpDetails|");
	TEST_FLAG(I915_SF_DUMP_DEPENDENCIES,  "DumpDeps|");
	TEST_FLAG(I915_SF_DUMP_SEQNO,         "DumpSeqno|");

#undef TEST_FLAG

	if (flags) {
		sprintf(ptr, "Unknown_0x%X!", flags);
		ptr += strlen(ptr);
	}

	if (ptr == str)
		strcpy(str, "-");
	else
		ptr[-1] = 0;

	return str;
};

/**
 * i915_scheduler_init - Initialise the scheduler.
 * @dev: DRM device
 * Returns zero on success or -ENOMEM if memory allocations fail.
 */
int i915_scheduler_init(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	int r;

	if (scheduler)
		return 0;

	scheduler = kzalloc(sizeof(*scheduler), GFP_KERNEL);
	if (!scheduler)
		return -ENOMEM;

	spin_lock_init(&scheduler->lock);

	for (r = 0; r < I915_NUM_RINGS; r++)
		INIT_LIST_HEAD(&scheduler->node_queue[r]);
	INIT_LIST_HEAD(&scheduler->completed_queue);

	/* Default tuning values: */
	scheduler->priority_level_min     = -1023;
	scheduler->priority_level_max     = 1023;
	scheduler->priority_level_bump    = 50;
	scheduler->priority_level_preempt = 900;
	scheduler->min_flying             = 5;
	scheduler->file_queue_max         = 256;
	atomic_set(&scheduler->queue_len, 0);
	init_waitqueue_head(&scheduler->busy_queue);
	scheduler->dump_flags             = I915_SF_DUMP_FORCE   |
					    I915_SF_DUMP_DETAILS |
					    I915_SF_DUMP_SEQNO   |
					    I915_SF_DUMP_DEPENDENCIES;

	dev_priv->scheduler = scheduler;

	return 0;
}

/*
 * Add a popped node back in to the queue. For example, because the ring was
 * hung when execfinal() was called and thus the ring submission needs to be
 * retried later.
 */
static void i915_scheduler_node_requeue(struct i915_scheduler *scheduler,
					struct i915_scheduler_queue_entry *node)
{
	WARN_ON(!I915_SQS_IS_FLYING(node));

	/* Seqno will be reassigned on relaunch */
	node->params.request->seqno = 0;
	node->status = I915_SQS_QUEUED;
	trace_i915_scheduler_unfly(node->params.ring, node);
	trace_i915_scheduler_node_state_change(node->params.ring, node);
	scheduler->counts[node->params.ring->id].flying--;
	scheduler->counts[node->params.ring->id].queued++;
}

/*
 * Give up on a node completely. For example, because it is causing the
 * ring to hang or is using some resource that no longer exists.
 */
static void i915_scheduler_node_kill(struct i915_scheduler *scheduler,
				     struct i915_scheduler_queue_entry *node)
{
	assert_scheduler_lock_held(scheduler);

	WARN_ON(I915_SQS_IS_COMPLETE(node));

	if (I915_SQS_IS_FLYING(node)) {
		trace_i915_scheduler_unfly(node->params.ring, node);
		scheduler->stats[node->params.ring->id].kill_flying++;
		scheduler->counts[node->params.ring->id].flying--;
	} else {
		scheduler->stats[node->params.ring->id].kill_queued++;
		scheduler->counts[node->params.ring->id].queued--;
	}

	node->status = I915_SQS_DEAD;
	list_move(&node->link, &scheduler->completed_queue);
	trace_i915_scheduler_node_state_change(node->params.ring, node);
}

/* Mark a node as in flight on the hardware. */
static void i915_scheduler_node_fly(struct i915_scheduler_queue_entry *node)
{
	struct drm_i915_private *dev_priv = node->params.dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	struct intel_engine_cs *ring = node->params.ring;

	assert_scheduler_lock_held(scheduler);

	WARN_ON(node->status != I915_SQS_POPPED);

	/*
	 * Add the node (which should currently be in state popped) to the
	 * front of the queue. This ensure that flying nodes are always held
	 * in hardware submission order.
	 */
	list_add(&node->link, &scheduler->node_queue[ring->id]);

	node->status = I915_SQS_FLYING;

	trace_i915_scheduler_fly(ring, node);
	trace_i915_scheduler_node_state_change(ring, node);
	scheduler->counts[ring->id].flying++;

	if (!(scheduler->flags[ring->id] & I915_SF_INTERRUPTS_ENABLED)) {
		bool success = true;

		success = ring->irq_get(ring);
		if (success)
			scheduler->flags[ring->id] |= I915_SF_INTERRUPTS_ENABLED;
	}
}

static inline uint32_t i915_scheduler_count_flying(struct i915_scheduler *scheduler,
					    struct intel_engine_cs *ring)
{
	return scheduler->counts[ring->id].flying;
}

static void i915_scheduler_priority_bump_clear(struct i915_scheduler *scheduler)
{
	struct i915_scheduler_queue_entry *node;
	int i;

	assert_scheduler_lock_held(scheduler);

	/*
	 * Ensure circular dependencies don't cause problems and that a bump
	 * by object usage only bumps each using buffer once:
	 */
	for (i = 0; i < I915_NUM_RINGS; i++) {
		for_each_scheduler_node(node, i)
			node->bumped = false;
	}
}

static int i915_scheduler_priority_bump(struct i915_scheduler *scheduler,
				struct i915_scheduler_queue_entry *target,
				uint32_t bump)
{
	int32_t new_priority;
	int i, count = 0;
	LIST_HEAD(queue);

	if (target->bumped)
		return 0;

	list_add_tail(&target->deplink, &queue);
	target->bumped = true;

	while (!list_empty(&queue)) {
		struct i915_scheduler_queue_entry *qe;

		qe = list_first_entry(&queue,
				struct i915_scheduler_queue_entry, deplink);
		list_del(&qe->deplink);
		count++;

		if (qe->priority >= scheduler->priority_level_max)
			continue;

		new_priority = qe->priority + bump;
		if ((new_priority <= qe->priority) ||
		    (new_priority > scheduler->priority_level_max))
			qe->priority = scheduler->priority_level_max;
		else
			qe->priority = new_priority;

		for (i = 0; i < qe->num_deps; i++) {
			if (!qe->dep_list[i] || qe->dep_list[i]->bumped)
				continue;

			list_add_tail(&qe->dep_list[i]->deplink, &queue);
			/* need to set flag early to avoid entry being
			 * added to queue multiple times */
			qe->dep_list[i]->bumped = true;
		}
	}

	return count;
}

/*
 * Nodes are considered valid dependencies if they are queued on any ring or
 * if they are in flight on a different ring. In flight on the same ring is no
 * longer interesting for non-premptive nodes as the ring serialises execution.
 * For pre-empting nodes, all in flight dependencies are valid as they must not
 * be jumped by the act of pre-empting.
 *
 * Anything that is neither queued nor flying is uninteresting.
 */
static inline bool i915_scheduler_is_dependency_valid(
			struct i915_scheduler_queue_entry *node, uint32_t idx)
{
	struct i915_scheduler_queue_entry *dep;

	dep = node->dep_list[idx];
	if (!dep)
		return false;

	if (I915_SQS_IS_QUEUED(dep))
		return true;

	if (I915_SQS_IS_FLYING(dep)) {
		if (node->params.ring != dep->params.ring)
			return true;
	}

	return false;
}

/* Use a private structure in order to pass the 'dev' pointer through */
struct i915_sync_fence_waiter {
	struct sync_fence_waiter sfw;
	struct drm_device	 *dev;
	struct i915_scheduler_queue_entry *node;
};

/*
 * NB: This callback can be executed at interrupt time. Further, it can be
 * called from within the TDR reset sequence during a scheduler 'kill_all'
 * and thus be called while the scheduler spinlock is already held. Thus
 * it can grab neither the driver mutex nor the scheduler spinlock.
 */
static void i915_scheduler_wait_fence_signaled(struct sync_fence *fence,
				       struct sync_fence_waiter *waiter)
{
	struct i915_sync_fence_waiter *i915_waiter;
	struct drm_i915_private *dev_priv = NULL;

	i915_waiter = container_of(waiter, struct i915_sync_fence_waiter, sfw);
	dev_priv    = (i915_waiter && i915_waiter->dev) ?
					i915_waiter->dev->dev_private : NULL;

	/*
	 * NB: The callback is executed at interrupt time, thus it can not
	 * call _submit() directly. It must go via the delayed work handler.
	 */
	if (dev_priv)
		queue_work(dev_priv->wq, &dev_priv->mm.scheduler_work);

	kfree(waiter);
}

static bool i915_scheduler_async_fence_wait(struct drm_device *dev,
				     struct i915_scheduler_queue_entry *node)
{
	struct drm_i915_private *dev_priv = node->params.ring->dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	struct i915_sync_fence_waiter *fence_waiter;
	struct sync_fence *fence = node->params.fence_wait;
	int signaled;
	bool success = true;

	if ((node->flags & I915_QEF_FENCE_WAITING) == 0) {
		node->flags |= I915_QEF_FENCE_WAITING;
		scheduler->stats[node->params.ring->id].fence_wait++;
	} else {
		scheduler->stats[node->params.ring->id].fence_again++;
		return true;
	}

	if (fence == NULL)
		return false;

	signaled = sync_fence_is_signaled(fence);
	if (!signaled) {
		fence_waiter = kmalloc(sizeof(*fence_waiter), GFP_KERNEL);
		if (!fence_waiter) {
			success = false;
			goto end;
		}

		sync_fence_waiter_init(&fence_waiter->sfw,
				i915_scheduler_wait_fence_signaled);
		fence_waiter->node = node;
		fence_waiter->dev = dev;

		if (sync_fence_wait_async(fence, &fence_waiter->sfw)) {
			/*
			 * an error occurred, usually this is because the
			 * fence was signaled already
			 */
			signaled = sync_fence_is_signaled(fence);
			if (!signaled) {
				success = false;
				goto end;
			}
		}
	}
end:
	return success;
}

static int i915_scheduler_pop_from_queue_locked(struct intel_engine_cs *ring,
				struct i915_scheduler_queue_entry **pop_node)
{
	struct drm_i915_private *dev_priv = ring->dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	struct i915_scheduler_queue_entry *fence_wait = NULL;
	struct i915_scheduler_queue_entry *best_wait = NULL;
	struct i915_scheduler_queue_entry *best = NULL;
	struct i915_scheduler_queue_entry *node;
	int ret;
	int i;
	bool signalled = true, any_queued = false;
	bool has_local, has_remote, only_remote = false;

	assert_scheduler_lock_held(scheduler);

	*pop_node = NULL;
	ret = -ENODATA;

	for_each_scheduler_node(node, ring->id) {
		if (!I915_SQS_IS_QUEUED(node))
			continue;
		any_queued = true;

		if (node->params.fence_wait)
			signalled = sync_fence_is_signaled(node->params.fence_wait);
		else
			signalled = true;

		if (!signalled) {
			signalled = i915_safe_to_ignore_fence(ring, node->params.fence_wait);
			scheduler->stats[node->params.ring->id].fence_ignore++;
		}

		has_local  = false;
		has_remote = false;
		for (i = 0; i < node->num_deps; i++) {
			if (!i915_scheduler_is_dependency_valid(node, i))
				continue;

			if (node->dep_list[i]->params.ring == node->params.ring)
				has_local = true;
			else
				has_remote = true;
		}

		if (has_remote && !has_local)
			only_remote = true;

		if (!has_local && !has_remote) {
			if (signalled) {
				if (!best ||
				    (node->priority > best->priority))
					best = node;
			} else {
				if (!best_wait ||
				    (node->priority > best_wait->priority))
					best_wait = node;
			}
		}
	}

	if (best) {
		list_del(&best->link);

		INIT_LIST_HEAD(&best->link);
		best->status = I915_SQS_POPPED;

		trace_i915_scheduler_node_state_change(ring, best);
		scheduler->counts[ring->id].queued--;

		ret = 0;
	} else {
		/* Can only get here if:
		 * (a) there are no buffers in the queue
		 * (b) all queued buffers are dependent on other buffers
		 *     e.g. on a buffer that is in flight on a different ring
		 * (c) all independent buffers are waiting on fences
		 */
		if (best_wait) {
			/* Need to wait for something to be signalled.
			 *
			 * NB: do not really want to wait on one specific fd
			 * because there is no guarantee in the order that
			 * blocked buffers will be signalled. Need to wait on
			 * 'anything' and then rescan for best available, if
			 * still nothing then wait again...
			 *
			 * NB 2: The wait must also wake up if someone attempts
			 * to submit a new buffer. The new buffer might be
			 * independent of all others and thus could jump the
			 * queue and start running immediately.
			 *
			 * NB 3: Lastly, must not wait with the spinlock held!
			 *
			 * So rather than wait here, need to queue a deferred
			 * wait thread and just return 'nothing to do'.
			 *
			 * NB 4: Can't actually do the wait here because the
			 * spinlock is still held and the wait requires doing
			 * a memory allocation.
			 */
			fence_wait = best_wait;
			ret = -EAGAIN;
		} else if (only_remote) {
			/* The only dependent buffers are on another ring. */
			ret = -EAGAIN;
		} else if (any_queued) {
			/* It seems that something has gone horribly wrong! */
			WARN_ONCE(true, "Broken dependency tracking on ring %d!\n",
				  (int) ring->id);
		}
	}

	if (fence_wait) {
		/* It should be safe to sleep now... */
		/* NB: Need to release and reacquire the spinlock though */
		spin_unlock_irq(&scheduler->lock);
		i915_scheduler_async_fence_wait(ring->dev, fence_wait);
		spin_lock_irq(&scheduler->lock);
	}

	trace_i915_scheduler_pop_from_queue(ring, best);

	*pop_node = best;
	return ret;
}

/*
 * NB: The driver mutex lock must be held before calling this function. It is
 * only really required during the actual back end submission call. However,
 * attempting to acquire a mutex while holding a spin lock is a Bad Idea.
 * And releasing the one before acquiring the other leads to other code
 * being run and interfering.
 *
 * Hence any caller that does not already have the mutex lock for other
 * reasons should call i915_scheduler_submit_unlocked() instead in order to
 * obtain the lock first.
 */
static int i915_scheduler_submit(struct intel_engine_cs *ring)
{
	struct drm_device *dev = ring->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	struct i915_scheduler_queue_entry *node;
	int ret, count = 0, flying;

	WARN_ON(!mutex_is_locked(&dev->struct_mutex));

	spin_lock_irq(&scheduler->lock);

	WARN_ON(scheduler->flags[ring->id] & I915_SF_SUBMITTING);
	scheduler->flags[ring->id] |= I915_SF_SUBMITTING;

	/* First time around, complain if anything unexpected occurs: */
	ret = i915_scheduler_pop_from_queue_locked(ring, &node);
	if (ret)
		goto error;

	do {
		WARN_ON(node->params.ring != ring);
		WARN_ON(node->status != I915_SQS_POPPED);
		count++;

		/*
		 * The call to pop above will have removed the node from the
		 * list. So add it back in and mark it as in flight.
		 */
		i915_scheduler_node_fly(node);

		scheduler->stats[ring->id].submitted++;

		spin_unlock_irq(&scheduler->lock);
		ret = dev_priv->gt.execbuf_final(&node->params);
		if (!ret && (node->params.ctx->flags & CONTEXT_BOOST_FREQ))
			intel_queue_rps_boost_for_request(dev, node->params.request);
		spin_lock_irq(&scheduler->lock);

		/*
		 * Handle failed submission but first check that the
		 * watchdog/reset code has not nuked the node while we
		 * weren't looking:
		 */
		if (ret && (node->status != I915_SQS_DEAD)) {
			bool requeue = true;

			/*
			 * Oh dear! Either the node is broken or the ring is
			 * busy. So need to kill the node or requeue it and try
			 * again later as appropriate.
			 */

			switch (-ret) {
			case ENODEV:
			case ENOENT:
				/* Fatal errors. Kill the node. */
				requeue = false;
				scheduler->stats[ring->id].exec_dead++;
				i915_scheduler_node_kill(scheduler, node);
				break;

			case EAGAIN:
			case EBUSY:
			case EIO:
			case ENOMEM:
			case ERESTARTSYS:
			case EINTR:
				/* Supposedly recoverable errors. */
				scheduler->stats[ring->id].exec_again++;
				break;

			default:
				/*
				 * Assume the error is recoverable and hope
				 * for the best.
				 */
				MISSING_CASE(-ret);
				scheduler->stats[ring->id].exec_again++;
				break;
			}

			if (requeue) {
				i915_scheduler_node_requeue(scheduler, node);
				/*
				 * No point spinning if the ring is currently
				 * unavailable so just give up and come back
				 * later.
				 */
				break;
			}
		}

		/* Keep launching until the sky is sufficiently full. */
		flying = i915_scheduler_count_flying(scheduler, ring);
		if (flying >= scheduler->min_flying)
			break;

		/* Grab another node and go round again... */
		ret = i915_scheduler_pop_from_queue_locked(ring, &node);
	} while (ret == 0);

	/* Don't complain about not being able to submit extra entries */
	if (ret == -ENODATA)
		ret = 0;

	/*
	 * Bump the priority of everything that was not submitted to prevent
	 * starvation of low priority tasks by a spamming high priority task.
	 */
	i915_scheduler_priority_bump_clear(scheduler);
	for_each_scheduler_node(node, ring->id) {
		if (!I915_SQS_IS_QUEUED(node))
			continue;

		i915_scheduler_priority_bump(scheduler, node,
					     scheduler->priority_level_bump);
	}

	/* On success, return the number of buffers submitted. */
	if (ret == 0)
		ret = count;

error:
	scheduler->flags[ring->id] &= ~I915_SF_SUBMITTING;
	spin_unlock_irq(&scheduler->lock);
	return ret;
}

static int i915_scheduler_submit_unlocked(struct intel_engine_cs *ring)
{
	struct drm_device *dev = ring->dev;
	int ret;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	ret = i915_scheduler_submit(ring);

	mutex_unlock(&dev->struct_mutex);

	return ret;
}

/**
 * i915_scheduler_file_queue_inc - Increment the file's request queue count.
 * @file: File object to process.
 */
static void i915_scheduler_file_queue_inc(struct i915_scheduler *scheduler)
{
	atomic_inc(&scheduler->queue_len);
}

/**
 * i915_scheduler_file_queue_dec - Decrement the file's request queue count.
 * @file: File object to process.
 */
static void i915_scheduler_file_queue_dec(struct i915_scheduler *scheduler)
{
	atomic_dec(&scheduler->queue_len);
	wake_up(&scheduler->busy_queue);
}

static int i915_generate_dependencies(struct i915_scheduler *scheduler,
				       struct i915_scheduler_queue_entry *node)
{
	uint32_t count = 0;

	struct i915_scheduler_obj_entry *this_oe, *that_oe;
	struct drm_i915_gem_request *req = node->params.request;
	struct drm_i915_gem_request *that = node->params.request;
	int i;
	struct hlist_head *rh = &node->params.ctx->req_head[node->params.user_ctx_id & CONTEXT_HLIST_MASK];

	hlist_for_each_entry(that, rh, ctx_link) {
		count++;

		if (!that->scheduler_qe || I915_SQS_IS_COMPLETE(that->scheduler_qe))
			continue;

		if (that->ring != node->params.ring)
			continue;

		if (that->scheduler_qe->params.user_ctx_id != node->params.user_ctx_id)
			continue;

		if (that->dep_uniq != req->uniq) {
			node->dep_list[node->num_deps] = that->scheduler_qe;
			node->num_deps++;
			that->dep_uniq = req->uniq;
		}
	}

	hlist_add_head(&req->ctx_link, rh);

	for (i = 0; i < node->num_objs; i++) {
		this_oe = node->objs + i;

		list_for_each_entry(that_oe, &this_oe->obj->req_head, req_link) {
			count++;
			that = that_oe->req;

			if (!that->scheduler_qe || I915_SQS_IS_COMPLETE(that->scheduler_qe))
				continue;

			/* Only need to worry about writes */
			if (this_oe->read_only && that_oe->read_only)
				continue;

			if (that->dep_uniq != req->uniq) {
				node->dep_list[node->num_deps] = that->scheduler_qe;
				node->num_deps++;
				that->dep_uniq = req->uniq;
			}
		}

		list_add_tail(&node->objs[i].req_link, &this_oe->obj->req_head);
		node->objs[i].req = req;
	}

	return count;
}

static int i915_scheduler_queue_execbuffer_bypass(struct i915_scheduler_queue_entry *qe)
{
	struct drm_i915_private *dev_priv = qe->params.dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	int ret;

	scheduler->stats[qe->params.ring->id].queued++;

	trace_i915_scheduler_queue(qe->params.ring, qe);

	WARN_ON(qe->params.fence_wait &&
		(!sync_fence_is_signaled(qe->params.fence_wait)));

	intel_ring_reserved_space_cancel(qe->params.request->ringbuf);

	scheduler->flags[qe->params.ring->id] |= I915_SF_SUBMITTING;
	ret = dev_priv->gt.execbuf_final(&qe->params);
	if (!ret && (qe->params.ctx->flags & CONTEXT_BOOST_FREQ))
		intel_queue_rps_boost_for_request(dev_priv->dev, qe->params.request);
	scheduler->stats[qe->params.ring->id].submitted++;
	scheduler->flags[qe->params.ring->id] &= ~I915_SF_SUBMITTING;

	/*
	 * Don't do any clean up on failure because the caller will
	 * do it all anyway.
	 */
	if (ret)
		return ret;

	/* Need to release any resources held by the node: */
	qe->status = I915_SQS_COMPLETE;
	i915_scheduler_clean_node(qe);

	scheduler->stats[qe->params.ring->id].expired++;

	return 0;
}

static inline uint32_t i915_scheduler_count_incomplete(struct i915_scheduler *scheduler)
{
	int r, incomplete = 0;

	for (r = 0; r < I915_NUM_RINGS; r++)
		incomplete += scheduler->counts[r].queued + scheduler->counts[r].flying;

	return incomplete;
}

/**
 * i915_scheduler_queue_execbuffer - Submit a batch buffer request to the
 * scheduler.
 * @qe: The batch buffer request to be queued.
 * The expectation is the qe passed in is a local stack variable. This
 * function will copy its contents into a freshly allocated list node. The
 * new node takes ownership of said contents so the original qe should simply
 * be discarded and not cleaned up (i.e. don't free memory it points to or
 * dereference objects it holds). The node is added to the scheduler's queue
 * and the batch buffer will be submitted to the hardware at some future
 * point in time (which may be immediately, before returning or may be quite
 * a lot later).
 */
int i915_scheduler_queue_execbuffer(struct i915_scheduler_queue_entry *qe)
{
	struct drm_i915_private *dev_priv = qe->params.dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	struct intel_engine_cs *ring = qe->params.ring;
	struct i915_scheduler_queue_entry *node;
	bool not_flying;
	int i;
	int incomplete;
	int count = 0;

	if (qe->params.fence_wait)
		scheduler->stats[ring->id].fence_got++;

	/* Bypass the scheduler and send the buffer immediately? */
	if (!i915.enable_scheduler)
		return i915_scheduler_queue_execbuffer_bypass(qe);

	node = kmalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	*node = *qe;
	INIT_LIST_HEAD(&node->link);
	node->status = I915_SQS_QUEUED;
	node->stamp  = jiffies;
	i915_gem_request_reference(node->params.request);

	intel_ring_reserved_space_cancel(node->params.request->ringbuf);

	WARN_ON(node->params.request->scheduler_qe);
	node->params.request->scheduler_qe = node;

	/*
	 * Need to determine the number of incomplete entries in the list as
	 * that will be the maximum size of the dependency list.
	 *
	 * Note that the allocation must not be made with the spinlock acquired
	 * as kmalloc can sleep. However, the unlock/relock is safe because no
	 * new entries can be queued up during the unlock as the i915 driver
	 * mutex is still held. Entries could be removed from the list but that
	 * just means the dep_list will be over-allocated which is fine.
	 */
	spin_lock_irq(&scheduler->lock);
	incomplete = i915_scheduler_count_incomplete(scheduler);

	/* Temporarily unlock to allocate memory: */
	spin_unlock_irq(&scheduler->lock);
	if (incomplete) {
		node->dep_list = kmalloc_array(incomplete,
					       sizeof(*node->dep_list),
					       GFP_KERNEL);
		if (!node->dep_list) {
			kfree(node);
			return -ENOMEM;
		}
	} else
		node->dep_list = NULL;

	spin_lock_irq(&scheduler->lock);
	node->num_deps = 0;

	count = i915_generate_dependencies(scheduler, node);

	WARN_ON(node->num_deps > incomplete);

	node->priority = clamp(node->priority,
			       scheduler->priority_level_min,
			       scheduler->priority_level_max);

	if ((node->priority > 0) && node->num_deps) {
		i915_scheduler_priority_bump_clear(scheduler);

		for (i = 0; i < node->num_deps; i++)
			i915_scheduler_priority_bump(scheduler,
					node->dep_list[i], node->priority);
	}

	list_add_tail(&node->link, &scheduler->node_queue[ring->id]);

	i915_scheduler_file_queue_inc(scheduler);

	not_flying = i915_scheduler_count_flying(scheduler, ring) <
						 scheduler->min_flying;

	scheduler->stats[ring->id].queued++;

	trace_i915_scheduler_queue(ring, node);
	trace_i915_scheduler_node_state_change(ring, node);
	scheduler->counts[ring->id].queued++;

	spin_unlock_irq(&scheduler->lock);

	if (not_flying)
		i915_scheduler_submit(ring);

	return 0;
}

/**
 * i915_scheduler_notify_request - Notify the scheduler that the given
 * request has completed on the hardware.
 * @req: Request structure which has completed
 * @preempt: Did it complete pre-emptively?
 * A sequence number has popped out of the hardware and the request handling
 * code has mapped it back to a request and will mark that request complete.
 * It also calls this function to notify the scheduler about the completion
 * so the scheduler's node can be updated appropriately.
 * Returns true if the request is scheduler managed, false if not. The return
 * value is combined for all freshly completed requests and if any were true
 * then i915_scheduler_wakeup() is called so the scheduler can do further
 * processing (submit more work) at the end.
 */
bool i915_scheduler_notify_request(struct drm_i915_gem_request *req)
{
	struct drm_i915_private *dev_priv = to_i915(req->ring->dev);
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	struct i915_scheduler_queue_entry *node = req->scheduler_qe;
	unsigned long flags;

	trace_i915_scheduler_landing(req);

	if (!node)
		return false;

	spin_lock_irqsave(&scheduler->lock, flags);

	WARN_ON(!I915_SQS_IS_FLYING(node));

	/* Node was in flight so mark it as complete. */
	if (req->cancelled) {
		node->status = I915_SQS_DEAD;
		scheduler->stats[req->ring->id].kill_flying++;
	} else {
		node->status = I915_SQS_COMPLETE;
		scheduler->stats[req->ring->id].completed++;
	}

	trace_i915_scheduler_node_state_change(req->ring, node);
	scheduler->counts[req->ring->id].flying--;

	list_move(&node->link, &scheduler->completed_queue);

	spin_unlock_irqrestore(&scheduler->lock, flags);

	return true;
}

static int i915_scheduler_remove_dependent(struct i915_scheduler *scheduler,
				struct i915_scheduler_queue_entry *remove)
{
	struct i915_scheduler_queue_entry *node;
	int i, r;
	int count = 0;

	/*
	 * Ensure that a node is not being removed which is still dependent
	 * upon other (not completed) work. If that happens, it implies
	 * something has gone very wrong with the dependency tracking! Note
	 * that there is no need to worry if this node has been explicitly
	 * killed for some reason - it might be being killed before it got
	 * sent to the hardware.
	 */
	if (remove->status != I915_SQS_DEAD) {
		for (i = 0; i < remove->num_deps; i++)
			if ((remove->dep_list[i]) &&
			    (!I915_SQS_IS_COMPLETE(remove->dep_list[i])))
				count++;
		WARN_ON(count);
	}

	/*
	 * Because of the split list, the loop below will no longer remove
	 * dependencies between completed nodes. Thus you could (briefly)
	 * end up with a dangling pointer when one completed node is freed
	 * but still referenced as a dependency by another completed node.
	 * In practice I don't think there is anywhere that pointer could
	 * get dereferenced. However, clearing num_deps here should enforce
	 * that.
	 *
	 * Note that the dereference above is safe because all the
	 * remove_dependent calls are done en masse for the entire current
	 * set of completed nodes. Then a later step starts freeing them up.
	 * And a node being processed in this pass cannot have a dangling
	 * reference to a completed node from a previous pass because at
	 * that point this node must have been incomplete and would
	 * therefore have had the dependency stripped.
	 */
	remove->num_deps = 0;

	/*
	 * Remove this node from the dependency lists of any other node which
	 * might be waiting on it.
	 */
	for (r = 0; r < I915_NUM_RINGS; r++) {
		for_each_scheduler_node(node, r) {
			for (i = 0; i < node->num_deps; i++) {
				if (node->dep_list[i] != remove)
					continue;

				node->dep_list[i] = NULL;
			}
		}
	}

	return 0;
}

/**
 * i915_scheduler_wakeup - wake the scheduler's worker thread
 * @dev: DRM device
 * Called at the end of seqno interrupt processing if any request has
 * completed that corresponds to a scheduler node.
 */
void i915_scheduler_wakeup(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);

	queue_work(dev_priv->wq, &dev_priv->mm.scheduler_work);
}

/**
 * i915_scheduler_clean_node - free up any allocations/references
 * associated with the given scheduler queue entry.
 * @node: Queue entry structure which is complete
 * After a give batch buffer completes on the hardware, all the information
 * required to resubmit it is no longer required. However, the node entry
 * itself might still be required for tracking purposes for a while longer.
 * This function should be called as soon as the node is known to be complete
 * so that these resources may be freed even though the node itself might
 * hang around.
 */
void i915_scheduler_clean_node(struct i915_scheduler_queue_entry *node)
{
	int i;

	if (!I915_SQS_IS_COMPLETE(node)) {
		WARN(!node->params.request->cancelled,
		     "Cleaning active node: %d!\n", node->status);
		return;
	}

	if (node->params.batch_obj) {
		/*
		 * The batch buffer must be unpinned before it is unreferenced
		 * otherwise the unpin fails with a missing vma!?
		 */
		if (node->params.dispatch_flags & I915_DISPATCH_SECURE)
			i915_gem_execbuff_release_batch_obj(node->params.batch_obj);

		node->params.batch_obj = NULL;
	}

	/* Release the locked buffers: */
	for (i = 0; i < node->num_objs; i++) {
		list_del(&node->objs[i].req_link);
		drm_gem_object_unreference(&node->objs[i].obj->base);
	}
	kfree(node->objs);
	node->objs = NULL;
	node->num_objs = 0;

	/* Context too: */
	if (node->params.ctx) {
		hlist_del_init(&node->params.request->ctx_link);
		i915_gem_context_unreference(node->params.ctx);
		node->params.ctx = NULL;
	}

	/* And anything else owned by the node: */
	if (node->params.fence_wait) {
		sync_fence_put(node->params.fence_wait);
		node->params.fence_wait = 0;
	}

	if (node->params.cliprects) {
		kfree(node->params.cliprects);
		node->params.cliprects = NULL;
	}
}

void i915_scheduler_reset_cleanup(struct intel_engine_cs *ring)
{
	struct drm_i915_private *dev_priv = ring->dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;

	if (scheduler->flags[ring->id] & I915_SF_INTERRUPTS_ENABLED) {
		ring->irq_put(ring);
		scheduler->flags[ring->id] &= ~I915_SF_INTERRUPTS_ENABLED;
	}
}

static bool i915_scheduler_remove(struct i915_scheduler *scheduler,
				  struct intel_engine_cs *ring,
				  struct list_head *remove)
{
	struct i915_scheduler_queue_entry *node;
	bool do_submit;

	spin_lock_irq(&scheduler->lock);

	INIT_LIST_HEAD(remove);

	list_for_each_entry(node, &scheduler->completed_queue, link) {
		WARN_ON(!I915_SQS_IS_COMPLETE(node));

		scheduler->stats[node->params.ring->id].expired++;

		/* Strip the dependency info while the mutex is still locked */
		i915_scheduler_remove_dependent(scheduler, node);

		/* Likewise clean up the file pointer. */
		if (node->params.file) {
			i915_scheduler_file_queue_dec(scheduler);
			node->params.file = NULL;
		}
	}
	list_splice_init(&scheduler->completed_queue, remove);

	/*
	 * Release the interrupt reference count if there are no longer any
	 * nodes to worry about.
	 */
	if (list_empty(&scheduler->node_queue[ring->id]) &&
	    (scheduler->flags[ring->id] & I915_SF_INTERRUPTS_ENABLED)) {
		ring->irq_put(ring);
		scheduler->flags[ring->id] &= ~I915_SF_INTERRUPTS_ENABLED;
	}

	/* Launch more packets now? */
	do_submit = scheduler->counts[ring->id].queued > 0 &&
		    scheduler->counts[ring->id].flying < scheduler->min_flying;

	trace_i915_scheduler_remove(ring, do_submit);

	spin_unlock_irq(&scheduler->lock);

	return do_submit;
}

static void i915_scheduler_process_work(struct intel_engine_cs *ring)
{
	struct drm_i915_private *dev_priv = ring->dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	struct i915_scheduler_queue_entry *node;
	bool do_submit;
	struct list_head remove;

	if (list_empty(&scheduler->node_queue[ring->id]) &&
	    list_empty(&scheduler->completed_queue))
		return;

	/* Remove completed nodes. */
	do_submit = i915_scheduler_remove(scheduler, ring, &remove);

	if (!do_submit && list_empty(&remove))
		return;

	/* Need to grab the pm lock outside of the mutex lock */
	if (do_submit)
		intel_runtime_pm_get(dev_priv);

	mutex_lock(&ring->dev->struct_mutex);

	if (do_submit)
		i915_scheduler_submit(ring);

	while (!list_empty(&remove)) {
		node = list_first_entry(&remove, typeof(*node), link);
		list_del(&node->link);

		trace_i915_scheduler_destroy(node->params.ring, node);

		/* Free up all the DRM references */
		i915_scheduler_clean_node(node);

		/* And anything else owned by the node: */
		node->params.request->scheduler_qe = NULL;
		i915_gem_request_unreference(node->params.request);
		kfree(node->dep_list);
		kfree(node);
	}

	mutex_unlock(&ring->dev->struct_mutex);

	if (do_submit)
		intel_runtime_pm_put(dev_priv);
}

/**
 * i915_scheduler_work_handler - scheduler's work handler callback.
 * @work: Work structure
 * A lot of the scheduler's work must be done asynchronously in response to
 * an interrupt or other event. However, that work cannot be done at
 * interrupt time or in the context of the event signaller (which might in
 * fact be an interrupt). Thus a worker thread is required. This function
 * will cause the thread to wake up and do its processing.
 */
void i915_scheduler_work_handler(struct work_struct *work)
{
	struct intel_engine_cs *ring;
	struct drm_i915_private *dev_priv;
	int i;

	dev_priv = container_of(work, struct drm_i915_private, mm.scheduler_work);

	for_each_ring(ring, dev_priv, i)
		i915_scheduler_process_work(ring);
}

/**
 * i915_scheduler_file_queue_wait - Waits for space in the per file queue.
 * @file: File object to process.
 * This allows throttling of applications by limiting the total number of
 * outstanding requests to a specified level. Once that limit is reached,
 * this call will stall waiting on the oldest outstanding request. If it can
 * not stall for any reason it returns true to mean that the queue is full
 * and no more requests should be accepted.
 */
bool i915_scheduler_file_queue_wait(struct drm_file *file)
{
	struct drm_i915_file_private *file_priv = file->driver_priv;
	struct drm_i915_private *dev_priv  = file_priv->dev_priv;
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	int ret;

#define COND (atomic_read(&scheduler->queue_len) < scheduler->file_queue_max)

	if (COND)
		return false;

	ret = wait_event_interruptible(scheduler->busy_queue, COND);
	if (ret < 0)
		return true;

	return false;

#undef COND
}

static void
i915_scheduler_dump_node_details(struct i915_scheduler *scheduler,
				 struct intel_engine_cs *ring,
				 struct i915_scheduler_queue_entry *node,
				 uint32_t counts[])
{
	int i, deps;
	uint32_t count;

	if (node->status < I915_SQS_MAX) {
		count = counts[node->status]++;
	} else {
		DRM_DEBUG_DRIVER("<%s>   Unknown status: %d!\n",
				ring->name, node->status);
		count = -1;
	}

	deps = 0;
	for (i = 0; i < node->num_deps; i++)
		if (i915_scheduler_is_dependency_valid(node, i))
			deps++;

	DRM_DEBUG_DRIVER("<%s>   %c:%02d> uniq = %d, seqno"
			 " = %d/%s, deps = %d / %d, fence = %p/%d, %s [pri = "
			 "%4d]\n", ring->name,
			 i915_scheduler_queue_status_chr(node->status),
			 count,
			 node->params.request->uniq,
			 node->params.request->seqno,
			 node->params.ring->name,
			 deps, node->num_deps,
			 node->params.fence_wait,
			 node->params.fence_wait ? sync_fence_is_signaled(node->params.fence_wait) : 0,
			 i915_qe_state_str(node),
			 node->priority);

	if ((scheduler->flags[ring->id] & I915_SF_DUMP_DEPENDENCIES) == 0)
		return;

	for (i = 0; i < node->num_deps; i++) {
		if (!node->dep_list[i])
			continue;

		DRM_DEBUG_DRIVER("<%s>       |-%c:"
				 "%02d%c uniq = %d, seqno = %d/%s, %s [pri = %4d]\n",
				 ring->name,
				 i915_scheduler_queue_status_chr(node->dep_list[i]->status),
				 i,
				 i915_scheduler_is_dependency_valid(node, i)
				 ? '>' : '#',
				 node->dep_list[i]->params.request->uniq,
				 node->dep_list[i]->params.request->seqno,
				 node->dep_list[i]->params.ring->name,
				 i915_qe_state_str(node->dep_list[i]),
				 node->dep_list[i]->priority);
	}
}

static int i915_scheduler_dump_locked(struct intel_engine_cs *ring,
				      const char *msg)
{
	struct drm_i915_private *dev_priv = ring->dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	struct i915_scheduler_queue_entry *node;
	int flying = 0, queued = 0, complete = 0, other = 0;
	static int old_flying = -1, old_queued = -1, old_complete = -1;
	bool b_dump;
	char brkt[2] = { '<', '>' };

	if (!ring)
		return -EINVAL;

	for_each_scheduler_node(node, ring->id) {
		if (I915_SQS_IS_QUEUED(node))
			queued++;
		else if (I915_SQS_IS_FLYING(node))
			flying++;
		else
			other++;
	}

	list_for_each_entry(node, &scheduler->completed_queue, link) {
		if (node->params.ring == ring)
			complete++;
	}

	b_dump = (flying != old_flying) ||
		 (queued != old_queued) ||
		 (complete != old_complete);
	if (scheduler->flags[ring->id] & I915_SF_DUMP_FORCE) {
		if (!b_dump) {
			b_dump = true;
			brkt[0] = '{';
			brkt[1] = '}';
		}

		scheduler->flags[ring->id] &= ~I915_SF_DUMP_FORCE;
	}

	if (b_dump) {
		old_flying   = flying;
		old_queued   = queued;
		old_complete = complete;
		DRM_DEBUG_DRIVER("<%s> Q:%02d, F:%02d, C:%02d, O:%02d, "
				 "Flags = %s, Next = %d:%d %c%s%c\n",
				 ring->name, queued, flying, complete, other,
				 i915_scheduler_flag_str(scheduler->flags[ring->id]),
				 dev_priv->request_uniq, dev_priv->next_seqno,
				 brkt[0], msg, brkt[1]);
	} else {
		/*DRM_DEBUG_DRIVER("<%s> Q:%02d, F:%02d, C:%02d, O:%02d"
				 ", Flags = %s, Next = %d:%d [%s]\n",
				 ring->name,
				 queued, flying, complete, other,
				 i915_scheduler_flag_str(scheduler->flags[ring->id]),
				 dev_priv->request_uniq, dev_priv->next_seqno, msg); */

		return 0;
	}

	if (scheduler->flags[ring->id] & I915_SF_DUMP_SEQNO) {
		uint32_t seqno;

		seqno = ring->get_seqno(ring, true);

		DRM_DEBUG_DRIVER("<%s> Seqno = %d\n", ring->name, seqno);
	}

	if (scheduler->flags[ring->id] & I915_SF_DUMP_DETAILS) {
		uint32_t counts[I915_SQS_MAX];

		memset(counts, 0x00, sizeof(counts));

		for_each_scheduler_node(node, ring->id) {
			i915_scheduler_dump_node_details(scheduler,
							 ring, node, counts);
		}

		list_for_each_entry(node, &scheduler->completed_queue, link) {
			if (node->params.ring != ring)
				continue;
			i915_scheduler_dump_node_details(scheduler,
							 ring, node, counts);
		}
	}

	return 0;
}

/**
 * i915_scheduler_dump - dump the scheduler's internal state to the debug log.
 * @ring: Ring to dump info for
 * @msg: A reason why it is being dumped
 * For debugging purposes, it can be very useful to see the internal state of
 * the scheduler for a given ring.
 */
int i915_scheduler_dump(struct intel_engine_cs *ring, const char *msg)
{
	struct drm_i915_private *dev_priv = ring->dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&scheduler->lock, flags);
	ret = i915_scheduler_dump_locked(ring, msg);
	spin_unlock_irqrestore(&scheduler->lock, flags);

	return ret;
}

static int i915_scheduler_dump_all_locked(struct drm_device *dev,
					  const char *msg)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	struct intel_engine_cs *ring;
	int i, r, ret = 0;

	for_each_ring(ring, dev_priv, i) {
		scheduler->flags[ring->id] |= scheduler->dump_flags & I915_SF_DUMP_MASK;
		r = i915_scheduler_dump_locked(ring, msg);
		if (ret == 0)
			ret = r;
	}

	return ret;
}

/**
 * i915_scheduler_dump_all - dump the scheduler's internal state to the debug
 * log.
 * @dev: DRM device
 * @msg: A reason why it is being dumped
 * For debugging purposes, it can be very useful to see the internal state of
 * the scheduler.
 */
int i915_scheduler_dump_all(struct drm_device *dev, const char *msg)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&scheduler->lock, flags);
	ret = i915_scheduler_dump_all_locked(dev, msg);
	spin_unlock_irqrestore(&scheduler->lock, flags);

	return ret;
}

/**
 * i915_scheduler_query_stats - return various scheduler statistics
 * @ring: Ring to report on
 * @stats: Stats structure to be filled in
 * For various reasons (debugging, performance analysis, curiosity) it is
 * useful to see statistics about what the scheduler is doing. This function
 * returns the stats that have been gathered in a data structure. The
 * expectation is that this will be returned to the user via debugfs.
 */
int i915_scheduler_query_stats(struct intel_engine_cs *ring,
			       struct i915_scheduler_stats_nodes *stats)
{
	struct drm_i915_private *dev_priv = ring->dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	struct i915_scheduler_queue_entry *node;

	memset(stats, 0x00, sizeof(*stats));

	spin_lock_irq(&scheduler->lock);

	for_each_scheduler_node(node, ring->id) {
		if (node->status >= I915_SQS_MAX) {
			DRM_DEBUG_DRIVER("Invalid node state: %d! [uniq = %d, seqno = %d]\n",
					 node->status, node->params.request->uniq,
					 node->params.request->seqno);

			stats->counts[I915_SQS_MAX]++;
			continue;
		}

		stats->counts[node->status]++;
	}

	list_for_each_entry(node, &scheduler->completed_queue, link) {
		if (node->params.ring != ring)
			continue;

		if (node->status >= I915_SQS_MAX) {
			DRM_DEBUG_DRIVER("Invalid node state: %d! [uniq = %d, seqno = %d]\n",
					 node->status, node->params.request->uniq,
					 node->params.request->seqno);

			stats->counts[I915_SQS_MAX]++;
			continue;
		}
		stats->counts[node->status]++;
	}

	if (stats->counts[I915_SQS_QUEUED] != scheduler->counts[ring->id].queued)
		printk(KERN_ERR "%s:%d> \x1B[31;1mQueued count mis-match: %d vs %d!\x1B[0m\n", __func__, __LINE__, stats->counts[I915_SQS_QUEUED], scheduler->counts[ring->id].queued);
	if (stats->counts[I915_SQS_FLYING] != scheduler->counts[ring->id].flying)
		printk(KERN_ERR "%s:%d> \x1B[31;1mFlying count mis-match: %d vs %d!\x1B[0m\n", __func__, __LINE__, stats->counts[I915_SQS_FLYING], scheduler->counts[ring->id].flying);

	spin_unlock_irq(&scheduler->lock);

	return 0;
}

static int i915_scheduler_submit_max_priority(struct intel_engine_cs *ring,
					      bool is_locked)
{
	struct i915_scheduler_queue_entry *node;
	struct drm_i915_private *dev_priv = ring->dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	int ret, count = 0;
	bool found;

	do {
		found = false;
		spin_lock_irq(&scheduler->lock);
		for_each_scheduler_node(node, ring->id) {
			if (!I915_SQS_IS_QUEUED(node))
				continue;

			if (node->priority < scheduler->priority_level_max)
				continue;

			found = true;
			break;
		}
		spin_unlock_irq(&scheduler->lock);

		if (!found)
			break;

		if (is_locked)
			ret = i915_scheduler_submit(ring);
		else
			ret = i915_scheduler_submit_unlocked(ring);
		if (ret < 0)
			return ret;

		count += ret;
	} while (found);

	return count;
}

/**
 * i915_scheduler_flush_request - force a given request through the scheduler.
 * @req: Request to be flushed
 * @is_locked: Is the driver mutex lock held?
 * For various reasons it is sometimes necessary to flush a request from the
 * scheduler's queue and through the hardware immediately rather than at some
 * vague time in the future.
 * Returns zero on success or -EAGAIN if the scheduler is busy (e.g. waiting
 * for a pre-emption event to complete) but the mutex lock is held which
 * would prevent the scheduler's asynchronous processing from completing.
 */
int i915_scheduler_flush_request(struct drm_i915_gem_request *req,
				 bool is_locked)
{
	struct drm_i915_private *dev_priv;
	struct i915_scheduler *scheduler;
	unsigned long flags;
	int flush_count;
	uint32_t engine_id;

	if (!req)
		return -EINVAL;

	dev_priv  = to_i915(req->ring->dev);
	scheduler = dev_priv->scheduler;

	if (!scheduler)
		return 0;

	if (!req->scheduler_qe)
		return 0;

	if (!I915_SQS_IS_QUEUED(req->scheduler_qe))
		return 0;

	engine_id = req->ring->id;
	if (is_locked && (scheduler->flags[engine_id] & I915_SF_SUBMITTING)) {
		/* Scheduler is busy already submitting another batch,
		 * come back later rather than going recursive... */
		return -EAGAIN;
	}

	if (list_empty(&scheduler->node_queue[engine_id]))
		return 0;

	spin_lock_irqsave(&scheduler->lock, flags);

	scheduler->stats[engine_id].flush_req++;

	i915_scheduler_priority_bump_clear(scheduler);

	flush_count = i915_scheduler_priority_bump(scheduler,
			    req->scheduler_qe, scheduler->priority_level_max);
	scheduler->stats[engine_id].flush_bump += flush_count;

	spin_unlock_irqrestore(&scheduler->lock, flags);

	if (flush_count) {
		DRM_DEBUG_DRIVER("<%s> Bumped %d entries\n", req->ring->name, flush_count);
		flush_count = i915_scheduler_submit_max_priority(req->ring, is_locked);
		if (flush_count > 0)
			scheduler->stats[engine_id].flush_submit += flush_count;
	}

	return flush_count;
}

/**
 * i915_scheduler_flush_stamp - force requests of a given age through the
 * scheduler.
 * @ring: Ring to be flushed
 * @target: Jiffy based time stamp to flush up to
 * @is_locked: Is the driver mutex lock held?
 * DRM has a throttle by age of request facility. This requires waiting for
 * outstanding work over a given age. This function helps that by forcing
 * queued batch buffers over said age through the system.
 * Returns zero on success or -EAGAIN if the scheduler is busy (e.g. waiting
 * for a pre-emption event to complete) but the mutex lock is held which
 * would prevent the scheduler's asynchronous processing from completing.
 */
int i915_scheduler_flush_stamp(struct intel_engine_cs *ring,
			       unsigned long target,
			       bool is_locked)
{
	struct i915_scheduler_queue_entry *node;
	struct drm_i915_private *dev_priv;
	struct i915_scheduler *scheduler;
	int flush_count = 0;

	if (!ring)
		return -EINVAL;

	dev_priv  = ring->dev->dev_private;
	scheduler = dev_priv->scheduler;

	if (!scheduler)
		return 0;

	if (is_locked && (scheduler->flags[ring->id] & I915_SF_SUBMITTING)) {
		/*
		 * Scheduler is busy already submitting another batch,
		 * come back later rather than going recursive...
		 */
		return -EAGAIN;
	}

	spin_lock_irq(&scheduler->lock);
	scheduler->stats[ring->id].flush_stamp++;
	i915_scheduler_priority_bump_clear(scheduler);
	for_each_scheduler_node(node, ring->id) {
		if (!I915_SQS_IS_QUEUED(node))
			continue;

		if (node->stamp > target)
			continue;

		flush_count = i915_scheduler_priority_bump(scheduler,
					node, scheduler->priority_level_max);
		scheduler->stats[ring->id].flush_bump += flush_count;
	}
	spin_unlock_irq(&scheduler->lock);

	if (flush_count) {
		DRM_DEBUG_DRIVER("<%s> Bumped %d entries\n", ring->name, flush_count);
		flush_count = i915_scheduler_submit_max_priority(ring, is_locked);
		if (flush_count > 0)
			scheduler->stats[ring->id].flush_submit += flush_count;
	}

	return flush_count;
}

/**
 * i915_scheduler_flush - force all requests through the scheduler.
 * @ring: Ring to be flushed
 * @is_locked: Is the driver mutex lock held?
 * For various reasons it is sometimes necessary to the scheduler out, e.g.
 * due to ring reset.
 * Returns zero on success or -EAGAIN if the scheduler is busy (e.g. waiting
 * for a pre-emption event to complete) but the mutex lock is held which
 * would prevent the scheduler's asynchronous processing from completing.
 */
int i915_scheduler_flush(struct intel_engine_cs *ring, bool is_locked)
{
	struct i915_scheduler_queue_entry *node;
	struct drm_i915_private *dev_priv;
	struct i915_scheduler *scheduler;
	bool found;
	int ret;
	uint32_t count = 0;

	if (!ring)
		return -EINVAL;

	dev_priv  = ring->dev->dev_private;
	scheduler = dev_priv->scheduler;

	if (!scheduler)
		return 0;

	WARN_ON(is_locked && (scheduler->flags[ring->id] & I915_SF_SUBMITTING));

	scheduler->stats[ring->id].flush_all++;

	do {
		found = false;
		spin_lock_irq(&scheduler->lock);
		for_each_scheduler_node(node, ring->id) {
			if (!I915_SQS_IS_QUEUED(node))
				continue;

			found = true;
			break;
		}
		spin_unlock_irq(&scheduler->lock);

		if (found) {
			if (is_locked)
				ret = i915_scheduler_submit(ring);
			else
				ret = i915_scheduler_submit_unlocked(ring);
			scheduler->stats[ring->id].flush_submit++;
			if (ret < 0)
				return ret;

			count += ret;
		}
	} while (found);

	return count;
}

/**
 * i915_scheduler_is_request_tracked - return info to say what the scheduler's
 * connection to this request is (if any).
 * @req: request to be queried
 * @compeleted: if non-null, set to completion status
 * @busy: if non-null set to busy status
 *
 * Looks up the given request in the scheduler's internal queue and reports
 * on whether the request has completed or is still pending.
 * Returns true if the request was found or false if it was not.
 */
bool i915_scheduler_is_request_tracked(struct drm_i915_gem_request *req,
				       bool *completed, bool *busy)
{
	struct drm_i915_private *dev_priv = req->ring->dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;

	if (!scheduler)
		return false;

	if (req->scheduler_qe == NULL)
		return false;

	if (completed)
		*completed = I915_SQS_IS_COMPLETE(req->scheduler_qe);
	if (busy)
		*busy      = I915_SQS_IS_QUEUED(req->scheduler_qe);

	return true;
}

/**
 * i915_scheduler_closefile - notify the scheduler that a DRM file handle
 * has been closed.
 * @dev: DRM device
 * @file: file being closed
 *
 * Goes through the scheduler's queues and removes all connections to the
 * disappearing file handle that still exist. There is an argument to say
 * that this should also flush such outstanding work through the hardware.
 * However, with pre-emption, TDR and other such complications doing so
 * becomes a locking nightmare. So instead, just warn with a debug message
 * if the application is leaking uncompleted work and make sure a null
 * pointer dereference will not follow.
 */
void i915_scheduler_closefile(struct drm_device *dev, struct drm_file *file)
{
	struct i915_scheduler_queue_entry *node;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	struct intel_engine_cs *ring;
	int i;

	if (!scheduler)
		return;

	spin_lock_irq(&scheduler->lock);

	for_each_ring(ring, dev_priv, i) {
		for_each_scheduler_node(node, ring->id) {
			if (node->params.file != file)
				continue;

			DRM_DEBUG_DRIVER("Closing file handle with outstanding work: %d:%d/%s on %s\n",
					 node->params.request->uniq,
					 node->params.request->seqno,
					 i915_qe_state_str(node),
					 ring->name);

			i915_scheduler_file_queue_dec(scheduler);
			node->params.file = NULL;
		}
	}

	spin_unlock_irq(&scheduler->lock);
}

/**
 * i915_scheduler_is_ring_flying - does the given ring have in flight batches?
 * @ring: Ring to query
 * Used by TDR to distinguish hung rings (not moving but with work to do)
 * from idle rings (not moving because there is nothing to do). Returns true
 * if the given ring has batches currently executing on the hardware.
 */
bool i915_scheduler_is_ring_flying(struct intel_engine_cs *ring)
{
	struct drm_i915_private *dev_priv = ring->dev->dev_private;
	struct i915_scheduler *scheduler = dev_priv->scheduler;
	struct i915_scheduler_queue_entry *node;
	unsigned long flags;
	bool found = false;

	/* With the scheduler in bypass mode, no information can be returned. */
	if (!i915.enable_scheduler)
		return true;

	spin_lock_irqsave(&scheduler->lock, flags);

	for_each_scheduler_node(node, ring->id) {
		if (I915_SQS_IS_FLYING(node)) {
			found = true;
			break;
		}
	}

	spin_unlock_irqrestore(&scheduler->lock, flags);

	return found;
}
