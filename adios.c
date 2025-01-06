// SPDX-License-Identifier: GPL-2.0
/*
 * The Adaptive Deadline I/O Scheduler (ADIOS)
 * Based on mq-deadline and Kyber,
 * with learning-based adaptive latency control
 *
 * Copyright (C) 2025 Masahito Suzuki
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/rbtree.h>
#include <linux/sbitmap.h>

#include "include/elevator.h"
#include "include/blk.h"
#include "include/blk-mq.h"
#include "include/blk-mq-sched.h"


#define ADIOS_VERSION "0.3.0"

static u64 global_latency_window = 20000000ULL;
static int bq_refill_below_ratio = 15;

enum {
	ADIOS_READ,
	ADIOS_WRITE,
	ADIOS_DISCARD,
	ADIOS_OTHER,
	ADIOS_NUM_OPTYPES,
};

static unsigned int adios_optype(struct request *rq) {
	blk_opf_t opf = rq->cmd_flags;
	switch (opf & REQ_OP_MASK) {
	case REQ_OP_READ:
		return ADIOS_READ;
	case REQ_OP_WRITE:
		return ADIOS_WRITE;
	case REQ_OP_DISCARD:
		return ADIOS_DISCARD;
	default:
		return ADIOS_OTHER;
	}
}

static u64 adios_latency_targets[ADIOS_NUM_OPTYPES] = {
	[ADIOS_READ]    =    2ULL * NSEC_PER_MSEC,
	[ADIOS_WRITE]   =  750ULL * NSEC_PER_MSEC,
	[ADIOS_DISCARD] = 5000ULL * NSEC_PER_MSEC,
	[ADIOS_OTHER]   =    0ULL,
};

static unsigned int adios_max_batch_size[ADIOS_NUM_OPTYPES] = {
	[ADIOS_READ]    = 16,
	[ADIOS_WRITE]   =  8,
	[ADIOS_DISCARD] =  1,
	[ADIOS_OTHER]   =  1,
};

struct latency_model {
	u64 intercept;
	u64 slope;
	u64 small_sum_delay;
	u64 small_count;
	u64 large_sum_delay;
	u64 large_sum_block_size;
	spinlock_t lock;
};

#define BLOCK_SIZE_THRESHOLD 4096

static void latency_model_input(struct latency_model *model, u64 block_size, u64 latency) {
	if (block_size > BLOCK_SIZE_THRESHOLD) {
		if (!model->intercept) return;
		model->large_sum_delay +=
			(latency > model->intercept) ? latency - model->intercept : 0ULL;
		model->large_sum_block_size += (block_size - BLOCK_SIZE_THRESHOLD) >> 10;
	} else {
		model->small_sum_delay += latency;
		model->small_count++;
	}
}

static void latency_model_update(struct latency_model *model) {
	guard(spinlock)(&model->lock);
	model->intercept = model->small_count ?
		(u64)div_u64(model->small_sum_delay, model->small_count) : 0ULL;
	model->slope = model->large_sum_block_size ?
		(u64)div_u64(model->large_sum_delay, model->large_sum_block_size) : 0ULL;
}

static u64 latency_model_predict(struct latency_model *model, u64 block_size) {
	guard(spinlock)(&model->lock);
	u64 result = model->intercept + (block_size > BLOCK_SIZE_THRESHOLD) ?
		model->slope * div_u64(block_size - BLOCK_SIZE_THRESHOLD, 1024) : 0UL;

	return result;
}

/*
 * I/O statistics. It is fine if these counters overflow.
 * What matters is that these counters are at least as wide as
 * log2(max_outstanding_requests).
 */
struct io_stats {
	uint32_t inserted;
	uint32_t merged;
	uint32_t dispatched;
	atomic_t completed;

	uint32_t max_batch_count[ADIOS_NUM_OPTYPES];
};

#define ADIOS_NUM_BQ_PAGES 2

/*
 * Adios scheduler data. Requests are present on both sort_list and dispatch list.
 */
struct ad_data {
	struct io_stats stats;
	struct list_head dispatch;
	struct rb_root sort_list;
	struct rb_root pos_list;

	u32 async_depth;

	spinlock_t lock;

	struct latency_model latency_model[ADIOS_NUM_OPTYPES];
	struct timer_list timer;

	int bq_page;
	bool more_bq_ready;
	struct list_head batch_queue[ADIOS_NUM_BQ_PAGES][ADIOS_NUM_OPTYPES];
	unsigned int batch_count[ADIOS_NUM_BQ_PAGES][ADIOS_NUM_OPTYPES];
	u64 total_predicted_latency;
};

struct ad_rq_data {
	u64 deadline;
	u64 predicted_latency;
	u64 block_size;

	struct rb_node pos_node;
};

static inline struct ad_rq_data *rq_data(struct request *rq) {
	return (struct ad_rq_data *)rq->elv.priv[1];
}

static void
adios_add_rq_rb_sort_list(struct ad_data *ad, struct request *rq) {
	struct rb_root *root = &ad->sort_list;
	struct rb_node **new = &(root->rb_node), *parent = NULL;
	struct ad_rq_data *rd = rq_data(rq);

	rd->block_size = blk_rq_bytes(rq);
	unsigned int optype = adios_optype(rq);
	rd->predicted_latency =
		latency_model_predict(&ad->latency_model[optype], rd->block_size);
	rd->deadline =
		rq->start_time_ns + adios_latency_targets[optype] + rd->predicted_latency;

	while (*new) {
		struct request *this = rb_entry_rq(*new);
		struct ad_rq_data *td = rq_data(this);
		s64 diff = td->deadline - rd->deadline;

		parent = *new;
		if (diff < 0)
			new = &((*new)->rb_left);
		else
			new = &((*new)->rb_right);
	}

	rb_link_node(&rq->rb_node, parent, new);
	rb_insert_color(&rq->rb_node, root);
}

static void
adios_add_rq_rb_pos_list(struct ad_data *ad, struct request *rq) {
	struct ad_rq_data *rd = rq_data(rq);
	sector_t pos = blk_rq_pos(rq);
	struct rb_root *root = &ad->pos_list;
	struct rb_node **new = &(root->rb_node), *parent = NULL;

	while (*new) {
		struct request *this = rb_entry_rq(*new);

		parent = *new;
		if (blk_rq_pos(this) >= pos)
			new = &((*new)->rb_left);
		else
			new = &((*new)->rb_right);
	}

	rb_link_node(&rd->pos_node, parent, new);
	rb_insert_color(&rd->pos_node, root);
}

static void
adios_add_rq_rb(struct ad_data *ad, struct request *rq) {
	adios_add_rq_rb_sort_list(ad, rq);
	adios_add_rq_rb_pos_list(ad, rq);
}

static inline void
adios_del_rq_rb_sort_list(struct ad_data *ad, struct request *rq) {
	rb_erase(&rq->rb_node, &ad->sort_list);
	RB_CLEAR_NODE(&rq->rb_node);
}

static inline void
adios_del_rq_rb_pos_list(struct ad_data *ad, struct request *rq) {
	struct ad_rq_data *rd = rq_data(rq);
	rb_erase(&rd->pos_node, &ad->pos_list);
	RB_CLEAR_NODE(&rd->pos_node);
}

static inline void
adios_del_rq_rb(struct ad_data *ad, struct request *rq) {
	adios_del_rq_rb_sort_list(ad, rq);
	adios_del_rq_rb_pos_list(ad, rq);
}

/*
 * remove rq from rbtree and dispatch list.
 */
static void adios_remove_request(struct ad_data *ad, struct request *rq) {
	struct request_queue *q = rq->q;

	list_del_init(&rq->queuelist);

	/*
	 * We might not be on the rbtree, if we are doing an insert merge
	 */
	if (!RB_EMPTY_NODE(&rq->rb_node))
		adios_del_rq_rb(ad, rq);

	elv_rqhash_del(q, rq);
	if (q->last_merge == rq)
		q->last_merge = NULL;
}

static void ad_request_merged(struct request_queue *q, struct request *req,
				  enum elv_merge type) {
	struct ad_data *ad = q->elevator->elevator_data;

	/*
	 * if the merge was a front merge, we need to reposition request
	 */
	if (type == ELEVATOR_FRONT_MERGE) {
		adios_del_rq_rb(ad, req);
		adios_add_rq_rb(ad, req);
	}
}

/*
 * Callback function that is invoked after @next has been merged into @req.
 */
static void ad_merged_requests(struct request_queue *q, struct request *req,
				   struct request *next) {
	struct ad_data *ad = q->elevator->elevator_data;

	lockdep_assert_held(&ad->lock);

	ad->stats.merged++;

	/*
	 * kill knowledge of next, this one is a goner
	 */
	adios_remove_request(ad, next);
}

/* Number of requests queued. */
static u32 ad_queued(struct ad_data *ad) {
	const struct io_stats *stats = &ad->stats;

	lockdep_assert_held(&ad->lock);

	return stats->inserted - atomic_read(&stats->completed);
}

/*
 * Return the next request to dispatch using sector position sorted lists.
 */
static struct request *
adios_next_request(struct ad_data *ad) {
	if (RB_EMPTY_ROOT(&ad->sort_list))
		return NULL;

	return rb_entry_rq(rb_first(&ad->sort_list));
}

static void adios_reset_batch_queues(struct ad_data *ad, int page) {
	memset(&ad->batch_count[page], 0, sizeof(ad->batch_count[page]));
}

static void adios_init_batch_queues(struct ad_data *ad) {
	for (int page = 0; page < ADIOS_NUM_BQ_PAGES; page++) {
		adios_reset_batch_queues(ad, page);

		for (int optype = 0; optype < ADIOS_NUM_OPTYPES; optype++)
			INIT_LIST_HEAD(&ad->batch_queue[page][optype]);
	}
}

static bool adios_fill_batch_queues(struct ad_data *ad) {
	unsigned int count = 0;
	unsigned int optype_count[ADIOS_NUM_OPTYPES];
	memset(optype_count, 0, sizeof(optype_count));
	int page = (ad->bq_page + 1) % ADIOS_NUM_BQ_PAGES;

	adios_reset_batch_queues(ad, page);

	while (true) {
		struct request *rq = adios_next_request(ad);
		if (!rq)
			break;

		struct ad_rq_data *rd = rq_data(rq);
		unsigned int optype = adios_optype(rq);
		u64 lat = ad->total_predicted_latency + rd->predicted_latency;

		// Check batch size and total predicted latency
		if (count && (ad->batch_count[page][optype] >=
				adios_max_batch_size[optype] ||
			lat > global_latency_window)) {
			break;
		}

		adios_remove_request(ad, rq);

		// Add request to the corresponding batch queue
		list_add_tail(&rq->queuelist, &ad->batch_queue[page][optype]);
		ad->batch_count[page][optype]++;
		ad->total_predicted_latency = lat;
		optype_count[optype]++;
		count++;
	}
	if (count) {
		ad->more_bq_ready = true;
		for (int optype = 0; optype < ADIOS_NUM_OPTYPES; optype++) {
			if (ad->stats.max_batch_count[optype] < optype_count[optype])
				ad->stats.max_batch_count[optype] = optype_count[optype];
		}
	}
	return count;
}

static void adios_flip_bq(struct ad_data *ad) {
	ad->more_bq_ready = false;
	ad->bq_page = (ad->bq_page + 1) % 2;
}

static struct request *ad_dispatch_from_bq(struct ad_data *ad) {
	struct request *rq = NULL;
	bool fill_tried = false;

	if (!ad->more_bq_ready && ad->total_predicted_latency <
			global_latency_window * bq_refill_below_ratio / 100) {
		adios_fill_batch_queues(ad);
		fill_tried = true;
	}

	while(true) {
		// Check if there are any requests in the batch queues
		for (int i = 0; i < ADIOS_NUM_OPTYPES; i++) {
			if (!list_empty(&ad->batch_queue[ad->bq_page][i])) {
				rq = list_first_entry(&ad->batch_queue[ad->bq_page][i],
										struct request, queuelist);
				list_del_init(&rq->queuelist);
				goto found;
			}
		}

		// If there's more batch queue page available, flip to it and retry
		if (ad->more_bq_ready) {
			adios_flip_bq(ad);
			continue;
		}

		if (fill_tried)
			break;

		if (adios_fill_batch_queues(ad))
			adios_flip_bq(ad);
		fill_tried = true;
	}

	if (!rq)
		return NULL;
found:
	ad->stats.dispatched++;
	rq->rq_flags |= RQF_STARTED;
	return rq;
}

/*
 * Called from blk_mq_run_hw_queue() -> __blk_mq_sched_dispatch_requests().
 *
 * One confusing aspect here is that we get called for a specific
 * hardware queue, but we may return a request that is for a
 * different hardware queue. This is because mq-adios has shared
 * state for all hardware queues, in terms of sorting, FIFOs, etc.
 */
static struct request *ad_dispatch_request(struct blk_mq_hw_ctx *hctx) {
	struct ad_data *ad = hctx->queue->elevator->elevator_data;
	struct request *rq;

	guard(spinlock)(&ad->lock);
	rq = ad_dispatch_from_bq(ad);

	return rq;
}

/*
 * 'depth' is a number in the range 1..INT_MAX representing a number of
 * requests. Scale it with a factor (1 << bt->sb.shift) / q->nr_requests since
 * 1..(1 << bt->sb.shift) is the range expected by sbitmap_get_shallow().
 * Values larger than q->nr_requests have the same effect as q->nr_requests.
 */
static int ad_to_word_depth(struct blk_mq_hw_ctx *hctx, unsigned int qdepth) {
	struct sbitmap_queue *bt = &hctx->sched_tags->bitmap_tags;
	const unsigned int nrr = hctx->queue->nr_requests;

	return ((qdepth << bt->sb.shift) + nrr - 1) / nrr;
}

/*
 * Called by __blk_mq_alloc_request(). The shallow_depth value set by this
 * function is used by __blk_mq_get_tag().
 */
static void ad_limit_depth(blk_opf_t opf, struct blk_mq_alloc_data *data) {
	struct ad_data *ad = data->q->elevator->elevator_data;

	/* Do not throttle synchronous reads. */
	if (op_is_sync(opf) && !op_is_write(opf))
		return;

	/*
	 * Throttle asynchronous requests and writes such that these requests
	 * do not block the allocation of synchronous requests.
	 */
	data->shallow_depth = ad_to_word_depth(data->hctx, ad->async_depth);
}

/* Called by blk_mq_update_nr_requests(). */
static void ad_depth_updated(struct blk_mq_hw_ctx *hctx) {
	struct request_queue *q = hctx->queue;
	struct ad_data *ad = q->elevator->elevator_data;
	struct blk_mq_tags *tags = hctx->sched_tags;

	ad->async_depth = q->nr_requests;

	sbitmap_queue_min_shallow_depth(&tags->bitmap_tags, 1);
}

/* Called by blk_mq_init_hctx() and blk_mq_init_sched(). */
static int ad_init_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx) {
	ad_depth_updated(hctx);
	return 0;
}

static void ad_exit_sched(struct elevator_queue *e) {
	struct ad_data *ad = e->elevator_data;

	timer_shutdown_sync(&ad->timer);

	WARN_ON_ONCE(!list_empty(&ad->dispatch));

	spin_lock(&ad->lock);
	u32 queued = ad_queued(ad);
	spin_unlock(&ad->lock);

	WARN_ONCE(queued != 0,
		  "statistics: i %u m %u d %u c %u\n",
		  ad->stats.inserted, ad->stats.merged,
		  ad->stats.dispatched, atomic_read(&ad->stats.completed));

	kfree(ad);
}

static void adios_timer_fn(struct timer_list *t) {
	struct ad_data *ad = from_timer(ad, t, timer);
	unsigned int optype;

	for (optype = 0; optype < ADIOS_NUM_OPTYPES; optype++)
		latency_model_update(&ad->latency_model[optype]);
}

/*
 * initialize elevator private data (adios_data).
 */
static int ad_init_sched(struct request_queue *q, struct elevator_type *e) {
	struct ad_data *ad;
	struct elevator_queue *eq;
	int ret = -ENOMEM;

	eq = elevator_alloc(q, e);
	if (!eq)
		return ret;

	ad = kzalloc_node(sizeof(*ad), GFP_KERNEL, q->node);
	if (!ad)
		goto put_eq;

	eq->elevator_data = ad;

	INIT_LIST_HEAD(&ad->dispatch);
	ad->sort_list = RB_ROOT;
	ad->pos_list = RB_ROOT;

	for (int i = 0; i < ADIOS_NUM_OPTYPES; i++)
		spin_lock_init(&ad->latency_model[i].lock);
	timer_setup(&ad->timer, adios_timer_fn, 0);
	adios_init_batch_queues(ad);

	spin_lock_init(&ad->lock);

	/* We dispatch from request queue wide instead of hw queue */
	blk_queue_flag_set(QUEUE_FLAG_SQ_SCHED, q);

	q->elevator = eq;
	return 0;

put_eq:
	kobject_put(&eq->kobj);
	return ret;
}

/*
 * Try to merge @bio into an existing request. If @bio has been merged into
 * an existing request, store the pointer to that request into *@rq.
 */
static int ad_request_merge(struct request_queue *q, struct request **rq,
				struct bio *bio) {
	struct ad_data *ad = q->elevator->elevator_data;
	sector_t sector = bio_end_sector(bio);
	struct request *__rq;

	__rq = elv_rb_find(&ad->pos_list, sector);
	if (__rq) {
		BUG_ON(sector != blk_rq_pos(__rq));

		if (elv_bio_merge_ok(__rq, bio)) {
			*rq = __rq;
			if (blk_discard_mergable(__rq))
				return ELEVATOR_DISCARD_MERGE;
			return ELEVATOR_FRONT_MERGE;
		}
	}

	return ELEVATOR_NO_MERGE;
}

/*
 * Attempt to merge a bio into an existing request. This function is called
 * before @bio is associated with a request.
 */
static bool ad_bio_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs) {
	struct ad_data *ad = q->elevator->elevator_data;
	struct request *free = NULL;
	bool ret;

	spin_lock(&ad->lock);
	ret = blk_mq_sched_try_merge(q, bio, nr_segs, &free);
	spin_unlock(&ad->lock);

	if (free)
		blk_mq_free_request(free);

	return ret;
}

/*
 * add rq to rbtree and dispatch list
 */
static void ad_insert_request(struct blk_mq_hw_ctx *hctx, struct request *rq,
				  blk_insert_t flags, struct list_head *free) {
	struct request_queue *q = hctx->queue;
	struct ad_data *ad = q->elevator->elevator_data;

	lockdep_assert_held(&ad->lock);

	if (!rq->elv.priv[0]) {
		ad->stats.inserted++;
		rq->elv.priv[0] = (void *)(uintptr_t)1;
	}

	if (blk_mq_sched_try_insert_merge(q, rq, free))
		return;

	adios_add_rq_rb(ad, rq);

	if (rq_mergeable(rq)) {
		elv_rqhash_add(q, rq);
		if (!q->last_merge)
			q->last_merge = rq;
	}

	list_add_tail(&rq->queuelist, &ad->dispatch);
}

/*
 * Called from blk_mq_insert_request() or blk_mq_dispatch_plug_list().
 */
static void ad_insert_requests(struct blk_mq_hw_ctx *hctx,
				   struct list_head *list,
				   blk_insert_t flags) {
	struct request_queue *q = hctx->queue;
	struct ad_data *ad = q->elevator->elevator_data;
	LIST_HEAD(free);

	spin_lock(&ad->lock);
	while (!list_empty(list)) {
		struct request *rq;

		rq = list_first_entry(list, struct request, queuelist);
		list_del_init(&rq->queuelist);
		ad_insert_request(hctx, rq, flags, &free);
	}
	spin_unlock(&ad->lock);

	blk_mq_free_requests(&free);
}

/* Callback from inside blk_mq_rq_ctx_init(). */
static void ad_prepare_request(struct request *rq) {
	rq->elv.priv[0] = NULL;

	struct ad_rq_data *rd = kzalloc(sizeof(*rd), GFP_KERNEL);
	if (!rd)
		return;

	rq->elv.priv[1] = rd;
}

static void ad_completed_request(struct request *rq, u64 now) {
	struct ad_data *ad = rq->q->elevator->elevator_data;
	struct ad_rq_data *rd = rq_data(rq);

	ad->total_predicted_latency -= rd->predicted_latency;
	if (!rq->io_start_time_ns)
		return;

	unsigned int optype = adios_optype(rq);
	u64 latency = now - rq->io_start_time_ns;
	latency_model_input(&ad->latency_model[optype], rd->block_size, latency);
	mod_timer(&ad->timer, jiffies + msecs_to_jiffies(100));
}

/*
 * Callback from inside blk_mq_free_request().
 */
static void ad_finish_request(struct request *rq) {
	struct ad_data *ad = rq->q->elevator->elevator_data;

	/*
	 * The block layer core may call ad_finish_request() without having
	 * called ad_insert_requests(). Skip requests that bypassed I/O
	 * scheduling. See also blk_mq_request_bypass_insert().
	 */
	if (rq->elv.priv[0]) {
		kfree(rq_data(rq));
		rq->elv.priv[0] = NULL;
		atomic_inc(&ad->stats.completed);
	}
}

static bool ad_has_work_for_ad(struct ad_data *ad) {
	for (int page = 0; page < ADIOS_NUM_BQ_PAGES; page++)
		for (int i = 0; i < ADIOS_NUM_OPTYPES; i++)
			if(!list_empty_careful(&ad->batch_queue[page][i]))
				return true;

	return !list_empty_careful(&ad->dispatch) ||
		!RB_EMPTY_ROOT(&ad->sort_list);
}

static bool ad_has_work(struct blk_mq_hw_ctx *hctx) {
	struct ad_data *ad = hctx->queue->elevator->elevator_data;

	return ad_has_work_for_ad(ad);
}

#define SYSFS_OPTYPE_DECL(name, optype)					\
static ssize_t adios_##name##_lat_model_show(struct elevator_queue *e, char *page) { \
	struct ad_data *ad = e->elevator_data;				\
	struct latency_model *model = &ad->latency_model[optype];		\
	ssize_t len = 0;						\
	guard(spinlock)(&model->lock);					\
	len += sprintf(page, "intercept: %llu\n", model->intercept);	\
	len += sprintf(page + len, "slope: %llu\n", model->slope);	\
	len += sprintf(page + len, "small_sum_delay: %llu\n", model->small_sum_delay);\
	len += sprintf(page + len, "small_count: %llu\n", model->small_count);\
	len += sprintf(page + len, "large_sum_delay: %llu\n", model->large_sum_delay);\
	len += sprintf(page + len, "large_sum_block_size: %llu\n", model->large_sum_block_size);\
	return len;							\
} \
static ssize_t adios_##name##_lat_target_store( \
		struct elevator_queue *e, const char *page, size_t count) { \
	struct ad_data *ad = e->elevator_data;						\
	unsigned long nsec;								\
	int ret;									\
											\
	ret = kstrtoul(page, 10, &nsec);							\
	if (ret)									\
		return ret;									\
											\
	ad->latency_model[optype].intercept = 0ULL;					\
	adios_latency_targets[optype] = nsec;						\
											\
	return count;									\
}										\
static ssize_t adios_##name##_lat_target_show( \
		struct elevator_queue *e, char *page) { \
	return sprintf(page, "%llu\n", adios_latency_targets[optype]);			\
} \
static ssize_t adios_##name##_max_batch_size_store( \
		struct elevator_queue *e, const char *page, size_t count) { \
	unsigned long max_batch;							\
	int ret;									\
											\
	ret = kstrtoul(page, 10, &max_batch);						\
	if (ret || max_batch == 0)							\
		return -EINVAL;								\
											\
	adios_max_batch_size[optype] = max_batch;					\
											\
	return count;									\
}										\
static ssize_t adios_##name##_max_batch_size_show( \
		struct elevator_queue *e, char *page) { \
	return sprintf(page, "%u\n", adios_max_batch_size[optype]);				\
}

SYSFS_OPTYPE_DECL(read, ADIOS_READ);
SYSFS_OPTYPE_DECL(write, ADIOS_WRITE);
SYSFS_OPTYPE_DECL(discard, ADIOS_DISCARD);
SYSFS_OPTYPE_DECL(other, ADIOS_OTHER);

static ssize_t adios_max_batch_count_show(struct elevator_queue *e, char *page) {
	struct ad_data *ad = e->elevator_data;
	unsigned int read_count, write_count, discard_count, other_count;

	guard(spinlock)(&ad->lock);
	read_count = ad->stats.max_batch_count[ADIOS_READ];
	write_count = ad->stats.max_batch_count[ADIOS_WRITE];
	discard_count = ad->stats.max_batch_count[ADIOS_DISCARD];
	other_count = ad->stats.max_batch_count[ADIOS_OTHER];

	return sprintf(page,
		"Read: %u\nWrite: %u\nDiscard: %u\nOther: %u\n",
		read_count, write_count, discard_count, other_count);
}

static ssize_t adios_reset_bq_stats_store(struct elevator_queue *e, const char *page, size_t count) {
	struct ad_data *ad = e->elevator_data;
	unsigned long val;
	int ret;

	ret = kstrtoul(page, 10, &val);
	if (ret || val != 1)
		return -EINVAL;

	guard(spinlock)(&ad->lock);
	for (int i = 0; i < ADIOS_NUM_OPTYPES; i++)
		ad->stats.max_batch_count[i] = 0;

	return count;
}

static ssize_t adios_reset_latency_model_store(struct elevator_queue *e, const char *page, size_t count) {
	struct ad_data *ad = e->elevator_data;
	unsigned long val;
	int ret;

	ret = kstrtoul(page, 10, &val);
	if (ret || val != 1)
		return -EINVAL;

	guard(spinlock)(&ad->lock);
	for (int i = 0; i < ADIOS_NUM_OPTYPES; i++) {
		struct latency_model *model = &ad->latency_model[i];
		spin_lock(&model->lock);
		model->intercept = 0ULL;
		model->slope = 0ULL;
		model->small_sum_delay = 0ULL;
		model->small_count = 0ULL;
		model->large_sum_delay = 0ULL;
		model->large_sum_block_size = 0ULL;
		spin_unlock(&model->lock);
	}

	return count;
}

static ssize_t adios_global_latency_window_store(struct elevator_queue *e, const char *page, size_t count)
{
	unsigned long nsec;
	int ret;

	ret = kstrtoul(page, 10, &nsec);
	if (ret)
		return ret;

	global_latency_window = nsec;

	return count;
}

static ssize_t adios_global_latency_window_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%llu\n", global_latency_window);
}

#define DD_ATTR(name, show_func, store_func) \
	__ATTR(name, 0644, show_func, store_func)

static ssize_t adios_bq_refill_below_ratio_show(
		struct elevator_queue *e, char *page) {
	return sprintf(page, "%d\n", bq_refill_below_ratio);
}

static ssize_t adios_bq_refill_below_ratio_store(
		struct elevator_queue *e, const char *page, size_t count) {
	int ratio;
	int ret;

	ret = kstrtoint(page, 10, &ratio);
	if (ret || ratio < 0 || ratio > 100)
		return -EINVAL;

	bq_refill_below_ratio = ratio;
	return count;
}

static ssize_t adios_version_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%s\n", ADIOS_VERSION);
}

static struct elv_fs_entry adios_sched_attrs[] = {
	DD_ATTR(lat_model_read, adios_read_lat_model_show, NULL),
	DD_ATTR(lat_model_write, adios_write_lat_model_show, NULL),
	DD_ATTR(lat_model_discard, adios_discard_lat_model_show, NULL),
	DD_ATTR(lat_model_other, adios_other_lat_model_show, NULL),

	DD_ATTR(lat_target_read, adios_read_lat_target_show, adios_read_lat_target_store),
	DD_ATTR(lat_target_write, adios_write_lat_target_show, adios_write_lat_target_store),
	DD_ATTR(lat_target_discard, adios_discard_lat_target_show, adios_discard_lat_target_store),
	DD_ATTR(lat_target_other, adios_other_lat_target_show, adios_other_lat_target_store),

	DD_ATTR(max_batch_size_read, adios_read_max_batch_size_show, adios_read_max_batch_size_store),
	DD_ATTR(max_batch_size_write, adios_write_max_batch_size_show, adios_write_max_batch_size_store),
	DD_ATTR(max_batch_size_discard, adios_discard_max_batch_size_show, adios_discard_max_batch_size_store),
	DD_ATTR(max_batch_size_other, adios_other_max_batch_size_show, adios_other_max_batch_size_store),

    DD_ATTR(max_batch_count, adios_max_batch_count_show, NULL),
    DD_ATTR(reset_bq_stats, NULL, adios_reset_bq_stats_store),
    DD_ATTR(reset_latency_model, NULL, adios_reset_latency_model_store),

	DD_ATTR(global_latency_window, adios_global_latency_window_show, adios_global_latency_window_store),
	DD_ATTR(bq_refill_below_ratio, adios_bq_refill_below_ratio_show, adios_bq_refill_below_ratio_store),

	DD_ATTR(adios_version, adios_version_show, NULL),

	__ATTR_NULL
};

static struct elevator_type mq_adios = {
	.ops = {
		.depth_updated		= ad_depth_updated,
		.limit_depth		= ad_limit_depth,
		.insert_requests	= ad_insert_requests,
		.dispatch_request	= ad_dispatch_request,
		.prepare_request	= ad_prepare_request,
		.completed_request	= ad_completed_request,
		.finish_request		= ad_finish_request,
		.next_request		= elv_rb_latter_request,
		.former_request		= elv_rb_former_request,
		.bio_merge			= ad_bio_merge,
		.request_merge		= ad_request_merge,
		.requests_merged	= ad_merged_requests,
		.request_merged		= ad_request_merged,
		.has_work			= ad_has_work,
		.init_sched			= ad_init_sched,
		.exit_sched			= ad_exit_sched,
		.init_hctx			= ad_init_hctx,
	},
#ifdef CONFIG_BLK_DEBUG_FS
#endif
	.elevator_attrs = adios_sched_attrs,
	.elevator_name = "adios",
	.elevator_owner = THIS_MODULE,
};
MODULE_ALIAS("mq-adios-iosched");

static int __init adios_init(void) {
	printk(KERN_INFO "Adaptive Deadline I/O Scheduler %s by Masahito Suzuki", ADIOS_VERSION);
	return elv_register(&mq_adios);
}

static void __exit adios_exit(void) {
	elv_unregister(&mq_adios);
}

module_init(adios_init);
module_exit(adios_exit);

MODULE_AUTHOR("Masahito Suzuki");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Adaptive Deadline I/O scheduler");
