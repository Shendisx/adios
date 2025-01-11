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
#include <linux/math.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/rbtree.h>
#include <linux/sbitmap.h>
#include <linux/timekeeping.h>

#include "include/elevator.h"
#include "include/blk.h"
#include "include/blk-mq.h"
#include "include/blk-mq-sched.h"

#define ADIOS_VERSION "0.7.1"

static u64 global_latency_window = 16000000ULL;
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

static unsigned int adios_batch_size_limit[ADIOS_NUM_OPTYPES] = {
	[ADIOS_READ]    = 64,
	[ADIOS_WRITE]   = 32,
	[ADIOS_DISCARD] =  1,
	[ADIOS_OTHER]   =  1,
};

#define LM_BLOCK_SIZE_THRESHOLD 4096
#define LM_SAMPLES_THRESHOLD    1024
#define LM_INTERVAL_THRESHOLD   1500
#define LM_OUTLIER_PERCENTILE     99
#define LM_NUM_BUCKETS            64

struct latency_bucket {
	u64 count;
	u64 sum_latency;
	u64 sum_block_size;
};

struct latency_model {
	u64 base;
	u64 slope;
	u64 small_sum_delay;
	u64 small_count;
	u64 large_sum_delay;
	u64 large_sum_block_size;
	u64 last_updated_jiffies;
	spinlock_t lock;

	struct latency_bucket small_bucket[LM_NUM_BUCKETS];
	struct latency_bucket large_bucket[LM_NUM_BUCKETS];
	spinlock_t buckets_lock;
};

static unsigned int latency_model_input_bucket_index(
		struct latency_model *model, u64 measured, u64 predicted) {
	unsigned int bucket_index;

	if (measured < predicted * 2)
		bucket_index = (measured * 20) / predicted;
	else if (measured < predicted * 5)
		bucket_index = (measured * 10) / predicted + 20;
	else
		bucket_index = (measured * 3) / predicted + 40;
	
	return bucket_index;
}

static u32 latency_model_count_small_buckets(struct latency_model *model) {
	u32 total_count = 0;
	for (int i = 0; i < LM_NUM_BUCKETS; i++)
		total_count += model->small_bucket[i].count;
	return total_count;
}

static bool latency_model_update_small_buckets(
		struct latency_model *model, unsigned long flags,
		u32 total_count, bool count_all) {
	u32 threshold_count = 0;
	u32 cumulative_count = 0;
	u32 outlier_threshold_bucket = 0;
	u64 sum_latency = 0, sum_count = 0;
	u32 outlier_percentile = LM_OUTLIER_PERCENTILE;

	if (count_all)
		outlier_percentile = 100;

	// Calculate the threshold count for outlier detection
	threshold_count = (total_count * outlier_percentile) / 100;

	// Identify the bucket that corresponds to the outlier threshold
	for (int i = 0; i < LM_NUM_BUCKETS; i++) {
		cumulative_count += model->small_bucket[i].count;
		if (cumulative_count >= threshold_count) {
			outlier_threshold_bucket = i;
			break;
		}
	}

	// Calculate the average latency, excluding outliers
	for (int i = 0; i <= outlier_threshold_bucket; i++) {
		struct latency_bucket *bucket = &model->small_bucket[i];
		if (i < outlier_threshold_bucket) {
			sum_latency += bucket->sum_latency;
			sum_count += bucket->count;
		} else {
			// For the threshold bucket, calculate the contribution proportionally
			u64 remaining_count =
				threshold_count - (cumulative_count - bucket->count);
			if (bucket->count > 0) {
				sum_latency +=
					(bucket->sum_latency * remaining_count) / bucket->count;
			}
			sum_count += remaining_count;
		}
	}

	// Accumulate the average latency into the statistics
	model->small_sum_delay += sum_latency;
	model->small_count += sum_count;

	// Reset small bucket information
	memset(model->small_bucket, 0, sizeof(model->small_bucket[0]) * LM_NUM_BUCKETS);

	return true;
}

static u32 latency_model_count_large_buckets(struct latency_model *model) {
	u32 total_count = 0;
	for (int i = 0; i < LM_NUM_BUCKETS; i++)
		total_count += model->large_bucket[i].count;
	return total_count;
}

static bool latency_model_update_large_buckets(
		struct latency_model *model, unsigned long flags,
		u32 total_count, bool count_all) {
	unsigned int threshold_count = 0;
	unsigned int cumulative_count = 0;
	unsigned int outlier_threshold_bucket = 0;
	s64 sum_latency = 0;
	u64 sum_block_size = 0, intercept;
	u32 outlier_percentile = LM_OUTLIER_PERCENTILE;

	if (count_all)
		outlier_percentile = 100;

	// Calculate the threshold count for outlier detection
	threshold_count = (total_count * outlier_percentile) / 100;

	// Identify the bucket that corresponds to the outlier threshold
	for (int i = 0; i < LM_NUM_BUCKETS; i++) {
		cumulative_count += model->large_bucket[i].count;
		if (cumulative_count >= threshold_count) {
			outlier_threshold_bucket = i;
			break;
		}
	}

	// Calculate the average latency and block size, excluding outliers
	for (int i = 0; i <= outlier_threshold_bucket; i++) {
		struct latency_bucket *bucket = &model->large_bucket[i];
		if (i < outlier_threshold_bucket) {
			sum_latency += bucket->sum_latency;
			sum_block_size += bucket->sum_block_size;
		} else {
			// For the threshold bucket, calculate the contribution proportionally
			u64 remaining_count = threshold_count - (cumulative_count - bucket->count);
			if (bucket->count > 0) {
				sum_latency +=
					(bucket->sum_latency * remaining_count) / bucket->count;
				sum_block_size +=
					(bucket->sum_block_size * remaining_count) / bucket->count;
			}
		}
	}

	// Accumulate the average delay into the statistics
	intercept = model->base * threshold_count;
	if (sum_latency > intercept)
		model->large_sum_delay += sum_latency - intercept;
	model->large_sum_block_size += sum_block_size;

	// Reset large bucket information
	memset(model->large_bucket, 0, sizeof(model->large_bucket[0]) * LM_NUM_BUCKETS);

	return true;
}

static void latency_model_update(struct latency_model *model) {
	unsigned long flags;
	u64 now;
	u32 small_count, large_count;
	bool time_elapsed;
	bool small_processed = false, large_processed;

	spin_lock_irqsave(&model->lock, flags);

	spin_lock_irqsave(&model->buckets_lock, flags);

	// Check if enough time has elapsed since the last update
	now = jiffies;
	time_elapsed = unlikely(!model->base) || model->last_updated_jiffies +
		msecs_to_jiffies(LM_INTERVAL_THRESHOLD) <= now;
	
	small_count = latency_model_count_small_buckets(model);
	large_count = latency_model_count_large_buckets(model);

	// Update small buckets
	if (small_count && (time_elapsed ||
			LM_SAMPLES_THRESHOLD <= small_count || !model->base))
		small_processed = latency_model_update_small_buckets(
			model, flags, small_count, !model->base);
	// Update large buckets
	if (large_count && (time_elapsed ||
			LM_SAMPLES_THRESHOLD <= large_count || !model->slope))
		large_processed = latency_model_update_large_buckets(
			model, flags, large_count, !model->slope);

	spin_unlock_irqrestore(&model->buckets_lock, flags);

	// Update the base parameter if small bucket was processed
	if (small_processed && model->small_count)
		model->base = div_u64(model->small_sum_delay, model->small_count);

	// Update the slope parameter if large bucket was processed
	if (large_processed && model->large_sum_block_size)
		model->slope = div_u64(model->large_sum_delay,
			DIV_ROUND_UP_ULL(model->large_sum_block_size, 1024));

	// Reset statistics and update last updated jiffies if time has elapsed
	if (time_elapsed)
		model->last_updated_jiffies = now;

	spin_unlock_irqrestore(&model->lock, flags);
}

static void latency_model_input(struct latency_model *model,
		u64 block_size, u64 latency, u64 predicted_latency) {
	unsigned long flags;
	unsigned int bucket_index;

	spin_lock_irqsave(&model->buckets_lock, flags);

	if (block_size <= LM_BLOCK_SIZE_THRESHOLD) {
		// --- Handling for small requests ---

		bucket_index =
			latency_model_input_bucket_index(model, latency, (model->base ?: 1));

		if (bucket_index >= LM_NUM_BUCKETS)
			bucket_index = LM_NUM_BUCKETS - 1;

		model->small_bucket[bucket_index].count++;
		model->small_bucket[bucket_index].sum_latency += latency;

		if (!model->base) {
			spin_unlock_irqrestore(&model->buckets_lock, flags);
			latency_model_update(model);
			return;
		}
	} else {
		// --- Handling for large requests ---
		if (!model->base) {
			spin_unlock_irqrestore(&model->buckets_lock, flags);
			return;
		}

		bucket_index =
			latency_model_input_bucket_index(model, latency, predicted_latency);

		if (bucket_index >= LM_NUM_BUCKETS)
			bucket_index = LM_NUM_BUCKETS - 1;

		model->large_bucket[bucket_index].count++;
		model->large_bucket[bucket_index].sum_latency += latency;
		model->large_bucket[bucket_index].sum_block_size += block_size;
	}

	spin_unlock_irqrestore(&model->buckets_lock, flags);
}

static u64 latency_model_predict(struct latency_model *model, u64 block_size) {
	unsigned long flags;
	u64 result;

	spin_lock_irqsave(&model->lock, flags);
	// Predict latency based on the model
	result = model->base;
	if (block_size > LM_BLOCK_SIZE_THRESHOLD)
		result += model->slope * div_u64(block_size - LM_BLOCK_SIZE_THRESHOLD, 1024);
	spin_unlock_irqrestore(&model->lock, flags);

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

	uint32_t batch_size_actual_highest[ADIOS_NUM_OPTYPES];
};

#define ADIOS_NUM_BQ_PAGES 2

/*
 * Adios scheduler data. Requests are present on dl_queue list.
 */
struct adios_data {
	struct io_stats stats;
	struct list_head prio_queue;
	struct rb_root dl_queue;

	u32 async_depth;

	spinlock_t lock;

	struct latency_model latency_model[ADIOS_NUM_OPTYPES];
	struct timer_list timer;

	int bq_page;
	bool more_bq_ready;
	struct list_head batch_queue[ADIOS_NUM_BQ_PAGES][ADIOS_NUM_OPTYPES];
	unsigned int batch_count[ADIOS_NUM_BQ_PAGES][ADIOS_NUM_OPTYPES];
	atomic64_t total_predicted_latency;

	/* Pre-allocated memory pool for adios_rq_data */
	struct kmem_cache *adios_rq_data_pool;
};

struct adios_rq_data {
	struct request *rq;
	
	u64 deadline;
	u64 predicted_latency;
	u64 block_size;
};

static inline struct adios_rq_data *rq_data(struct request *rq) {
	return (struct adios_rq_data *)rq->elv.priv[1];
}

static void
adios_add_rq_rb(struct adios_data *ad, struct request *rq) {
	struct rb_root *root = &ad->dl_queue;
	struct rb_node **new = &(root->rb_node), *parent = NULL;
	struct adios_rq_data *rd = rq_data(rq);

	rd->block_size = blk_rq_bytes(rq);
	unsigned int optype = adios_optype(rq);
	rd->predicted_latency =
		latency_model_predict(&ad->latency_model[optype], rd->block_size);
	rd->deadline =
		rq->start_time_ns + adios_latency_targets[optype] + rd->predicted_latency;

	while (*new) {
		struct request *this = rb_entry_rq(*new);
		struct adios_rq_data *td = rq_data(this);

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

static inline void
adios_del_rq_rb(struct adios_data *ad, struct request *rq) {
	rb_erase(&rq->rb_node, &ad->dl_queue);
	RB_CLEAR_NODE(&rq->rb_node);
}

/*
 * remove rq from rbtree and dispatch list.
 */
static void adios_remove_request(struct adios_data *ad, struct request *rq) {
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

static void adios_request_merged(struct request_queue *q, struct request *req,
				  enum elv_merge type) {
	struct adios_data *ad = q->elevator->elevator_data;

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
static void adios_merged_requests(struct request_queue *q, struct request *req,
				   struct request *next) {
	struct adios_data *ad = q->elevator->elevator_data;

	lockdep_assert_held(&ad->lock);

	ad->stats.merged++;

	/*
	 * kill knowledge of next, this one is a goner
	 */
	adios_remove_request(ad, next);
}

/* Number of requests queued. */
static u32 adios_queued(struct adios_data *ad) {
	const struct io_stats *stats = &ad->stats;

	lockdep_assert_held(&ad->lock);

	return stats->inserted - atomic_read(&stats->completed);
}

/*
 * Return the next request to dispatch using sector position sorted lists.
 */
static struct request *
adios_next_request(struct adios_data *ad) {
	struct rb_root *root = &ad->dl_queue;

	if (RB_EMPTY_ROOT(root))
		return NULL;

	return rb_entry_rq(rb_first(root));
}

static void adios_reset_batch_counts(struct adios_data *ad, int page) {
	memset(&ad->batch_count[page], 0, sizeof(ad->batch_count[page]));
}

static void adios_init_batch_queues(struct adios_data *ad) {
	for (int page = 0; page < ADIOS_NUM_BQ_PAGES; page++) {
		adios_reset_batch_counts(ad, page);

		for (int optype = 0; optype < ADIOS_NUM_OPTYPES; optype++)
			INIT_LIST_HEAD(&ad->batch_queue[page][optype]);
	}
}

static bool adios_fill_batch_queues(struct adios_data *ad, u64 *tpl) {
	unsigned int count = 0;
	unsigned int optype_count[ADIOS_NUM_OPTYPES];
	memset(optype_count, 0, sizeof(optype_count));
	int page = (ad->bq_page + 1) % ADIOS_NUM_BQ_PAGES;
	u64 lat = tpl ? *tpl : atomic64_read(&ad->total_predicted_latency);

	adios_reset_batch_counts(ad, page);

	while (true) {
		struct request *rq = adios_next_request(ad);
		if (!rq)
			break;

		struct adios_rq_data *rd = rq_data(rq);
		unsigned int optype = adios_optype(rq);
		lat += rd->predicted_latency;

		// Check batch size and total predicted latency
		if (count && (!ad->latency_model[optype].base || 
			ad->batch_count[page][optype] >= adios_batch_size_limit[optype] ||
			lat > global_latency_window)) {
			break;
		}

		adios_remove_request(ad, rq);

		// Add request to the corresponding batch queue
		list_add_tail(&rq->queuelist, &ad->batch_queue[page][optype]);
		ad->batch_count[page][optype]++;
		atomic64_add(rd->predicted_latency, &ad->total_predicted_latency);
		optype_count[optype]++;
		count++;
	}
	if (count) {
		ad->more_bq_ready = true;
		for (int optype = 0; optype < ADIOS_NUM_OPTYPES; optype++) {
			if (ad->stats.batch_size_actual_highest[optype] < optype_count[optype])
				ad->stats.batch_size_actual_highest[optype] = optype_count[optype];
		}
	}
	return count;
}

static void adios_flip_bq(struct adios_data *ad) {
	ad->more_bq_ready = false;
	ad->bq_page = (ad->bq_page + 1) % 2;
}

static struct request *adios_dispatch_from_bq(struct adios_data *ad) {
	struct request *rq = NULL;
	bool fill_tried = false;
	u64 tpl = atomic64_read(&ad->total_predicted_latency);

	if (!ad->more_bq_ready &&
			tpl < global_latency_window * bq_refill_below_ratio / 100) {
		adios_fill_batch_queues(ad, &tpl);
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

		if (adios_fill_batch_queues(ad, NULL))
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
static struct request *adios_dispatch_request(struct blk_mq_hw_ctx *hctx) {
	struct adios_data *ad = hctx->queue->elevator->elevator_data;
	struct request *rq;

	guard(spinlock)(&ad->lock);

	if (!list_empty(&ad->prio_queue)) {
		rq = list_first_entry(&ad->prio_queue, struct request, queuelist);
		list_del_init(&rq->queuelist);
		goto done;
	}

	rq = adios_dispatch_from_bq(ad);

done:
	return rq;
}

/*
 * 'depth' is a number in the range 1..INT_MAX representing a number of
 * requests. Scale it with a factor (1 << bt->sb.shift) / q->nr_requests since
 * 1..(1 << bt->sb.shift) is the range expected by sbitmap_get_shallow().
 * Values larger than q->nr_requests have the same effect as q->nr_requests.
 */
static int adios_to_word_depth(struct blk_mq_hw_ctx *hctx, unsigned int qdepth) {
	struct sbitmap_queue *bt = &hctx->sched_tags->bitmap_tags;
	const unsigned int nrr = hctx->queue->nr_requests;

	return ((qdepth << bt->sb.shift) + nrr - 1) / nrr;
}

/*
 * Called by __blk_mq_alloc_request(). The shallow_depth value set by this
 * function is used by __blk_mq_get_tag().
 */
static void adios_limit_depth(blk_opf_t opf, struct blk_mq_alloc_data *data) {
	struct adios_data *ad = data->q->elevator->elevator_data;

	/* Do not throttle synchronous reads. */
	if (op_is_sync(opf) && !op_is_write(opf))
		return;

	/*
	 * Throttle asynchronous requests and writes such that these requests
	 * do not block the allocation of synchronous requests.
	 */
	data->shallow_depth = adios_to_word_depth(data->hctx, ad->async_depth);
}

/* Called by blk_mq_update_nr_requests(). */
static void adios_depth_updated(struct blk_mq_hw_ctx *hctx) {
	struct request_queue *q = hctx->queue;
	struct adios_data *ad = q->elevator->elevator_data;
	struct blk_mq_tags *tags = hctx->sched_tags;

	ad->async_depth = q->nr_requests;

	sbitmap_queue_min_shallow_depth(&tags->bitmap_tags, 1);
}

/* Called by blk_mq_init_hctx() and blk_mq_init_sched(). */
static int adios_init_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx) {
	adios_depth_updated(hctx);
	return 0;
}

static void adios_exit_sched(struct elevator_queue *e) {
	struct adios_data *ad = e->elevator_data;

	timer_shutdown_sync(&ad->timer);

	WARN_ON_ONCE(!list_empty(&ad->prio_queue));

	spin_lock(&ad->lock);
	u32 queued = adios_queued(ad);
	spin_unlock(&ad->lock);

	WARN_ONCE(queued != 0,
		  "statistics: i %u m %u d %u c %u\n",
		  ad->stats.inserted, ad->stats.merged,
		  ad->stats.dispatched, atomic_read(&ad->stats.completed));

	/* Free the memory pool */
	if (ad->adios_rq_data_pool)
		kmem_cache_destroy(ad->adios_rq_data_pool);

	kfree(ad);
}

static void adios_timer_fn(struct timer_list *t) {
	struct adios_data *ad = from_timer(ad, t, timer);
	unsigned int optype;

	for (optype = 0; optype < ADIOS_NUM_OPTYPES; optype++)
		latency_model_update(&ad->latency_model[optype]);
}

/*
 * initialize elevator private data (adios_data).
 */
static int adios_init_sched(struct request_queue *q, struct elevator_type *e) {
	struct adios_data *ad;
	struct elevator_queue *eq;
	int ret = -ENOMEM;
	unsigned int max_rq_data;

	eq = elevator_alloc(q, e);
	if (!eq)
		return ret;

	ad = kzalloc_node(sizeof(*ad), GFP_KERNEL, q->node);
	if (!ad)
		goto put_eq;

	/* Calculate the maximum number of adios_rq_data needed */
	max_rq_data = 0;
	for (int i = 0; i < ADIOS_NUM_OPTYPES; i++) {
		max_rq_data += adios_batch_size_limit[i];
	}
	max_rq_data *= 2; /* Double buffering */

	/* Create a memory pool for adios_rq_data */
	ad->adios_rq_data_pool = kmem_cache_create("adios_rq_data_pool",
						sizeof(struct adios_rq_data),
						0, SLAB_HWCACHE_ALIGN, NULL);
	if (!ad->adios_rq_data_pool) {
		pr_err("adios: Failed to create adios_rq_data_pool\n");
		goto free_ad;
	}

	/* Pre-allocate memory in the pool */
	for (int i = 0; i < max_rq_data; i++) {
		struct adios_rq_data *rd = kmem_cache_alloc(ad->adios_rq_data_pool, GFP_KERNEL);
		if (!rd) {
			pr_err("adios: Failed to pre-allocate memory in adios_rq_data_pool\n");
			goto destroy_pool;
		}
		kmem_cache_free(ad->adios_rq_data_pool, rd);
	}

	eq->elevator_data = ad;

	INIT_LIST_HEAD(&ad->prio_queue);
	ad->dl_queue = RB_ROOT;

	for (int i = 0; i < ADIOS_NUM_OPTYPES; i++) {
		struct latency_model *model = &ad->latency_model[i];
		spin_lock_init(&model->lock);
		spin_lock_init(&model->buckets_lock);
		for (int j = 0; j < LM_NUM_BUCKETS; j++) {
			memset(model->small_bucket, 0,
				sizeof(model->small_bucket[0]) * LM_NUM_BUCKETS);
			memset(model->large_bucket, 0,
				sizeof(model->large_bucket[0]) * LM_NUM_BUCKETS);
		}
		model->last_updated_jiffies = jiffies;
	}
	timer_setup(&ad->timer, adios_timer_fn, 0);
	adios_init_batch_queues(ad);

	spin_lock_init(&ad->lock);

	/* We dispatch from request queue wide instead of hw queue */
	blk_queue_flag_set(QUEUE_FLAG_SQ_SCHED, q);

	q->elevator = eq;
	return 0;

destroy_pool:
	kmem_cache_destroy(ad->adios_rq_data_pool);
free_ad:
	kfree(ad);
put_eq:
	kobject_put(&eq->kobj);
	return ret;
}

/*
 * Attempt to merge a bio into an existing request. This function is called
 * before @bio is associated with a request.
 */
static bool adios_bio_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs) {
	struct adios_data *ad = q->elevator->elevator_data;
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
static void adios_insert_request(struct blk_mq_hw_ctx *hctx, struct request *rq,
				  blk_insert_t flags, struct list_head *free) {
	struct request_queue *q = hctx->queue;
	struct adios_data *ad = q->elevator->elevator_data;

	lockdep_assert_held(&ad->lock);

	if (!rq->elv.priv[0]) {
		ad->stats.inserted++;
		rq->elv.priv[0] = (void *)(uintptr_t)1;
	}

	if (blk_mq_sched_try_insert_merge(q, rq, free))
		return;

	if (flags & BLK_MQ_INSERT_AT_HEAD) {
		list_add(&rq->queuelist, &ad->prio_queue);
		return;
	}

	adios_add_rq_rb(ad, rq);

	if (rq_mergeable(rq)) {
		elv_rqhash_add(q, rq);
		if (!q->last_merge)
			q->last_merge = rq;
	}
}

/*
 * Called from blk_mq_insert_request() or blk_mq_dispatch_plug_list().
 */
static void adios_insert_requests(struct blk_mq_hw_ctx *hctx,
				   struct list_head *list,
				   blk_insert_t flags) {
	struct request_queue *q = hctx->queue;
	struct adios_data *ad = q->elevator->elevator_data;
	LIST_HEAD(free);

	spin_lock(&ad->lock);
	while (!list_empty(list)) {
		struct request *rq;

		rq = list_first_entry(list, struct request, queuelist);
		list_del_init(&rq->queuelist);
		adios_insert_request(hctx, rq, flags, &free);
	}
	spin_unlock(&ad->lock);

	blk_mq_free_requests(&free);
}

/* Callback from inside blk_mq_rq_ctx_init(). */
static void adios_prepare_request(struct request *rq) {
	struct adios_data *ad = rq->q->elevator->elevator_data;
	struct adios_rq_data *rd;

	rq->elv.priv[0] = NULL;
	rq->elv.priv[1] = NULL;

	/* Allocate adios_rq_data from the memory pool */
	rd = kmem_cache_zalloc(ad->adios_rq_data_pool, GFP_ATOMIC);
	if (WARN(!rd, "adios_prepare_request: Failed to allocate memory from adios_rq_data_pool. rd is NULL\n"))
		return;

	rd->rq = rq;
	rq->elv.priv[1] = rd;
}

static void adios_completed_request(struct request *rq, u64 now) {
	struct adios_data *ad = rq->q->elevator->elevator_data;
	struct adios_rq_data *rd = rq_data(rq);

	atomic64_sub(rd->predicted_latency, &ad->total_predicted_latency);

	if (!rq->io_start_time_ns || !rd->block_size)
		return;
	u64 latency = now - rq->io_start_time_ns;
	unsigned int optype = adios_optype(rq);
	latency_model_input(&ad->latency_model[optype], rd->block_size, latency, rd->predicted_latency);
	timer_reduce(&ad->timer, jiffies + msecs_to_jiffies(100));
}

/*
 * Callback from inside blk_mq_free_request().
 */
static void adios_finish_request(struct request *rq) {
	struct adios_data *ad = rq->q->elevator->elevator_data;

	/*
	 * The block layer core may call adios_finish_request() without having
	 * called adios_insert_requests(). Skip requests that bypassed I/O
	 * scheduling. See also blk_mq_request_bypass_insert().
	 */
	if (rq->elv.priv[1]) {
		/* Free adios_rq_data back to the memory pool */
		kmem_cache_free(ad->adios_rq_data_pool, rq_data(rq));
		rq->elv.priv[1] = NULL;
	}
	if (rq->elv.priv[0]) {
		rq->elv.priv[0] = NULL;
		atomic_inc(&ad->stats.completed);
	}
}

static bool adios_has_work(struct blk_mq_hw_ctx *hctx) {
	struct adios_data *ad = hctx->queue->elevator->elevator_data;

	for (int page = 0; page < ADIOS_NUM_BQ_PAGES; page++)
		for (int optype = 0; optype < ADIOS_NUM_OPTYPES; optype++)
			if(!list_empty_careful(&ad->batch_queue[page][optype]))
				return true;

	return !RB_EMPTY_ROOT(&ad->dl_queue) ||
			!list_empty_careful(&ad->prio_queue);
}

#define SYSFS_OPTYPE_DECL(name, optype)					\
static ssize_t adios_lat_model_##name##_show(struct elevator_queue *e, char *page) { \
	struct adios_data *ad = e->elevator_data;				\
	struct latency_model *model = &ad->latency_model[optype];		\
	ssize_t len = 0;						\
	unsigned long flags; \
	spin_lock_irqsave(&model->lock, flags); \
	len += sprintf(page,       "base : %llu ns\n", model->base);	\
	len += sprintf(page + len, "slope: %llu ns / kB\n", model->slope);	\
	len += sprintf(page + len, "small: %llu ns / %llu rq\n", \
		model->small_sum_delay, model->small_count);\
	len += sprintf(page + len, "large: %llu ns / %llu B\n", \
		model->large_sum_delay, model->large_sum_block_size);\
	spin_unlock_irqrestore(&model->lock, flags); \
	return len;							\
} \
static ssize_t adios_lat_target_##name##_store( \
		struct elevator_queue *e, const char *page, size_t count) { \
	struct adios_data *ad = e->elevator_data;						\
	unsigned long nsec;								\
	int ret;									\
											\
	ret = kstrtoul(page, 10, &nsec);							\
	if (ret)									\
		return ret;									\
											\
	ad->latency_model[optype].base = 0ULL;					\
	adios_latency_targets[optype] = nsec;						\
											\
	return count;									\
}										\
static ssize_t adios_lat_target_##name##_show( \
		struct elevator_queue *e, char *page) { \
	return sprintf(page, "%llu\n", adios_latency_targets[optype]);			\
} \
static ssize_t adios_batch_size_limit_##name##_store( \
		struct elevator_queue *e, const char *page, size_t count) { \
	unsigned long max_batch;							\
	int ret;									\
											\
	ret = kstrtoul(page, 10, &max_batch);						\
	if (ret || max_batch == 0)							\
		return -EINVAL;								\
											\
	adios_batch_size_limit[optype] = max_batch;					\
											\
	return count;									\
}										\
static ssize_t adios_batch_size_limit_##name##_show( \
		struct elevator_queue *e, char *page) { \
	return sprintf(page, "%u\n", adios_batch_size_limit[optype]);				\
}

SYSFS_OPTYPE_DECL(read, ADIOS_READ);
SYSFS_OPTYPE_DECL(write, ADIOS_WRITE);
SYSFS_OPTYPE_DECL(discard, ADIOS_DISCARD);
SYSFS_OPTYPE_DECL(other, ADIOS_OTHER);

static ssize_t adios_batch_size_actual_highest_show(struct elevator_queue *e, char *page) {
	struct adios_data *ad = e->elevator_data;
	unsigned int read_count, write_count, discard_count, other_count;

	guard(spinlock)(&ad->lock);
	read_count = ad->stats.batch_size_actual_highest[ADIOS_READ];
	write_count = ad->stats.batch_size_actual_highest[ADIOS_WRITE];
	discard_count = ad->stats.batch_size_actual_highest[ADIOS_DISCARD];
	other_count = ad->stats.batch_size_actual_highest[ADIOS_OTHER];

	return sprintf(page,
		"Read   : %u\nWrite  : %u\nDiscard: %u\nOther  : %u\n",
		read_count, write_count, discard_count, other_count);
}

static ssize_t adios_reset_bq_stats_store(struct elevator_queue *e, const char *page, size_t count) {
	struct adios_data *ad = e->elevator_data;
	unsigned long val;
	int ret;

	ret = kstrtoul(page, 10, &val);
	if (ret || val != 1)
		return -EINVAL;

	guard(spinlock)(&ad->lock);
	for (int i = 0; i < ADIOS_NUM_OPTYPES; i++)
		ad->stats.batch_size_actual_highest[i] = 0;

	return count;
}

static ssize_t adios_reset_latency_model_store(struct elevator_queue *e, const char *page, size_t count) {
	struct adios_data *ad = e->elevator_data;
	unsigned long val;
	int ret;

	ret = kstrtoul(page, 10, &val);
	if (ret || val != 1)
		return -EINVAL;

	guard(spinlock)(&ad->lock);
	for (int i = 0; i < ADIOS_NUM_OPTYPES; i++) {
		struct latency_model *model = &ad->latency_model[i];
		unsigned long flags;
		spin_lock_irqsave(&model->lock, flags);
		model->base = 0ULL;
		model->slope = 0ULL;
		model->small_sum_delay = 0ULL;
		model->small_count = 0ULL;
		model->large_sum_delay = 0ULL;
		model->large_sum_block_size = 0ULL;
		spin_unlock_irqrestore(&model->lock, flags);
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
#define DD_ATTR_RW(name) \
	__ATTR(name, 0644, adios_##name##_show, adios_##name##_store)
#define DD_ATTR_RO(name) \
	__ATTR(name, 0644, adios_##name##_show, NULL)
#define DD_ATTR_WO(name) \
	__ATTR(name, 0644, NULL, adios_##name##_store)

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
	DD_ATTR(adios_version, adios_version_show, NULL),
	DD_ATTR_RO(batch_size_actual_highest),
	DD_ATTR_RW(bq_refill_below_ratio),
	DD_ATTR_RW(global_latency_window),

	DD_ATTR_RW(batch_size_limit_read),
	DD_ATTR_RW(batch_size_limit_write),
	DD_ATTR_RW(batch_size_limit_discard),
	DD_ATTR_RW(batch_size_limit_other),

	DD_ATTR_RO(lat_model_read),
	DD_ATTR_RO(lat_model_write),
	DD_ATTR_RO(lat_model_discard),
	DD_ATTR_RO(lat_model_other),

	DD_ATTR_RW(lat_target_read),
	DD_ATTR_RW(lat_target_write),
	DD_ATTR_RW(lat_target_discard),
	DD_ATTR_RW(lat_target_other),

	DD_ATTR_WO(reset_bq_stats),
	DD_ATTR_WO(reset_latency_model),

	__ATTR_NULL
};

static struct elevator_type mq_adios = {
	.ops = {
		.depth_updated		= adios_depth_updated,
		.limit_depth		= adios_limit_depth,
		.insert_requests	= adios_insert_requests,
		.dispatch_request	= adios_dispatch_request,
		.prepare_request	= adios_prepare_request,
		.completed_request	= adios_completed_request,
		.finish_request		= adios_finish_request,
		.next_request		= elv_rb_latter_request,
		.former_request		= elv_rb_former_request,
		.bio_merge			= adios_bio_merge,
		.requests_merged	= adios_merged_requests,
		.request_merged		= adios_request_merged,
		.has_work			= adios_has_work,
		.init_sched			= adios_init_sched,
		.exit_sched			= adios_exit_sched,
		.init_hctx			= adios_init_hctx,
	},
#ifdef CONFIG_BLK_DEBUG_FS
#endif
	.elevator_attrs = adios_sched_attrs,
	.elevator_name = "adios",
	.elevator_owner = THIS_MODULE,
};
MODULE_ALIAS("mq-adios-iosched");

static int __init adios_init(void) {
	printk(KERN_INFO "Adaptive Deadline I/O Scheduler %s by Masahito Suzuki\n", ADIOS_VERSION);
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
