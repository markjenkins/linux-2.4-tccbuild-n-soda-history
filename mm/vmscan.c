/*
 *  linux/mm/vmscan.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, Stephen Tweedie.
 *  kswapd added: 7.1.96  sct
 *  Removed kswapd_ctl limits, and swap out as many pages as needed
 *  to bring the system back to freepages.high: 2.4.97, Rik van Riel.
 *  Zone aware kswapd started 02/00, Kanoj Sarcar (kanoj@sgi.com).
 *  Multiqueue VM started 5.8.00, Rik van Riel.
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/file.h>
#include <linux/writeback.h>
#include <linux/suspend.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	/* for try_to_release_page(),
					buffer_heads_over_limit */
#include <linux/mm_inline.h>
#include <linux/pagevec.h>
#include <linux/backing-dev.h>
#include <linux/rmap-locking.h>
#include <linux/topology.h>

#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <asm/div64.h>

#include <linux/swapops.h>

/*
 * From 0 .. 100.  Higher means more swappy.
 */
int vm_swappiness = 60;
static long total_memory;

#ifdef ARCH_HAS_PREFETCH
#define prefetch_prev_lru_page(_page, _base, _field)			\
	do {								\
		if ((_page)->lru.prev != _base) {			\
			struct page *prev;				\
									\
			prev = list_entry(_page->lru.prev,		\
					struct page, lru);		\
			prefetch(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetch_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

#ifdef ARCH_HAS_PREFETCHW
#define prefetchw_prev_lru_page(_page, _base, _field)			\
	do {								\
		if ((_page)->lru.prev != _base) {			\
			struct page *prev;				\
									\
			prev = list_entry(_page->lru.prev,		\
					struct page, lru);		\
			prefetchw(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetchw_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

/*
 * The list of shrinker callbacks used by to apply pressure to
 * ageable caches.
 */
struct shrinker {
	shrinker_t		shrinker;
	struct list_head	list;
	int			seeks;	/* seeks to recreate an obj */
	long			nr;	/* objs pending delete */
};

static LIST_HEAD(shrinker_list);
static DECLARE_MUTEX(shrinker_sem);

/*
 * Add a shrinker callback to be called from the vm
 */
struct shrinker *set_shrinker(int seeks, shrinker_t theshrinker)
{
        struct shrinker *shrinker;

        shrinker = kmalloc(sizeof(*shrinker), GFP_KERNEL);
        if (shrinker) {
	        shrinker->shrinker = theshrinker;
	        shrinker->seeks = seeks;
	        shrinker->nr = 0;
	        down(&shrinker_sem);
	        list_add(&shrinker->list, &shrinker_list);
	        up(&shrinker_sem);
	}
	return shrinker;
}

EXPORT_SYMBOL(set_shrinker);

/*
 * Remove one
 */
void remove_shrinker(struct shrinker *shrinker)
{
	down(&shrinker_sem);
	list_del(&shrinker->list);
	up(&shrinker_sem);
	kfree(shrinker);
}

EXPORT_SYMBOL(remove_shrinker);
 
#define SHRINK_BATCH 128
/*
 * Call the shrink functions to age shrinkable caches
 *
 * Here we assume it costs one seek to replace a lru page and that it also
 * takes a seek to recreate a cache object.  With this in mind we age equal
 * percentages of the lru and ageable caches.  This should balance the seeks
 * generated by these structures.
 *
 * If the vm encounted mapped pages on the LRU it increase the pressure on
 * slab to avoid swapping.
 *
 * We do weird things to avoid (scanned*seeks*entries) overflowing 32 bits.
 */
static int shrink_slab(unsigned long scanned, unsigned int gfp_mask)
{
	struct shrinker *shrinker;
	long pages;

	if (down_trylock(&shrinker_sem))
		return 0;

	pages = nr_used_zone_pages();
	list_for_each_entry(shrinker, &shrinker_list, list) {
		unsigned long long delta;

		delta = (4 * scanned) / shrinker->seeks;
		delta *= (*shrinker->shrinker)(0, gfp_mask);
		do_div(delta, pages + 1);
		shrinker->nr += delta;
		if (shrinker->nr > SHRINK_BATCH) {
			long nr_to_scan = shrinker->nr;

			shrinker->nr = 0;
			mod_page_state(slabs_scanned, nr_to_scan);
			while (nr_to_scan) {
				long this_scan = nr_to_scan;

				if (this_scan > 128)
					this_scan = 128;
				(*shrinker->shrinker)(this_scan, gfp_mask);
				nr_to_scan -= this_scan;
				cond_resched();
			}
		}
	}
	up(&shrinker_sem);
	return 0;
}

/* Must be called with page's pte_chain_lock held. */
static inline int page_mapping_inuse(struct page *page)
{
	struct address_space *mapping = page->mapping;

	/* Page is in somebody's page tables. */
	if (page_mapped(page))
		return 1;

	/* XXX: does this happen ? */
	if (!mapping)
		return 0;

	/* Be more reluctant to reclaim swapcache than pagecache */
	if (PageSwapCache(page))
		return 1;

	/* File is mmap'd by somebody. */
	if (!list_empty(&mapping->i_mmap))
		return 1;
	if (!list_empty(&mapping->i_mmap_shared))
		return 1;

	return 0;
}

static inline int is_page_cache_freeable(struct page *page)
{
	return page_count(page) - !!PagePrivate(page) == 2;
}

static int may_write_to_queue(struct backing_dev_info *bdi)
{
	if (current_is_kswapd())
		return 1;
	if (current_is_pdflush())	/* This is unlikely, but why not... */
		return 1;
	if (!bdi_write_congested(bdi))
		return 1;
	if (bdi == current->backing_dev_info)
		return 1;
	return 0;
}

/*
 * We detected a synchronous write error writing a page out.  Probably
 * -ENOSPC.  We need to propagate that into the address_space for a subsequent
 * fsync(), msync() or close().
 *
 * The tricky part is that after writepage we cannot touch the mapping: nothing
 * prevents it from being freed up.  But we have a ref on the page and once
 * that page is locked, the mapping is pinned.
 *
 * We're allowed to run sleeping lock_page() here because we know the caller has
 * __GFP_FS.
 */
static void handle_write_error(struct address_space *mapping,
				struct page *page, int error)
{
	lock_page(page);
	if (page->mapping == mapping) {
		if (error == -ENOSPC)
			set_bit(AS_ENOSPC, &mapping->flags);
		else
			set_bit(AS_EIO, &mapping->flags);
	}
	unlock_page(page);
}

/*
 * shrink_list returns the number of reclaimed pages
 */
static int
shrink_list(struct list_head *page_list, unsigned int gfp_mask, int *nr_scanned)
{
	struct address_space *mapping;
	LIST_HEAD(ret_pages);
	struct pagevec freed_pvec;
	int pgactivate = 0;
	int ret = 0;

	cond_resched();

	pagevec_init(&freed_pvec, 1);
	while (!list_empty(page_list)) {
		struct page *page;
		int may_enter_fs;
		int referenced;

		page = list_entry(page_list->prev, struct page, lru);
		list_del(&page->lru);

		if (TestSetPageLocked(page))
			goto keep;

		/* Double the slab pressure for mapped and swapcache pages */
		if (page_mapped(page) || PageSwapCache(page))
			(*nr_scanned)++;

		BUG_ON(PageActive(page));

		if (PageWriteback(page))
			goto keep_locked;

		pte_chain_lock(page);
		referenced = page_referenced(page);
		if (referenced && page_mapping_inuse(page)) {
			/* In active use or really unfreeable.  Activate it. */
			pte_chain_unlock(page);
			goto activate_locked;
		}

		mapping = page->mapping;

#ifdef CONFIG_SWAP
		/*
		 * Anonymous process memory without backing store. Try to
		 * allocate it some swap space here.
		 *
		 * XXX: implement swap clustering ?
		 */
		if (page_mapped(page) && !mapping && !PagePrivate(page)) {
			pte_chain_unlock(page);
			if (!add_to_swap(page))
				goto activate_locked;
			pte_chain_lock(page);
			mapping = page->mapping;
		}
#endif /* CONFIG_SWAP */

		may_enter_fs = (gfp_mask & __GFP_FS) ||
				(PageSwapCache(page) && (gfp_mask & __GFP_IO));

		/*
		 * The page is mapped into the page tables of one or more
		 * processes. Try to unmap it here.
		 */
		if (page_mapped(page) && mapping) {
			switch (try_to_unmap(page)) {
			case SWAP_FAIL:
				pte_chain_unlock(page);
				goto activate_locked;
			case SWAP_AGAIN:
				pte_chain_unlock(page);
				goto keep_locked;
			case SWAP_SUCCESS:
				; /* try to free the page below */
			}
		}
		pte_chain_unlock(page);

		/*
		 * If the page is dirty, only perform writeback if that write
		 * will be non-blocking.  To prevent this allocation from being
		 * stalled by pagecache activity.  But note that there may be
		 * stalls if we need to run get_block().  We could test
		 * PagePrivate for that.
		 *
		 * If this process is currently in generic_file_write() against
		 * this page's queue, we can perform writeback even if that
		 * will block.
		 *
		 * If the page is swapcache, write it back even if that would
		 * block, for some throttling. This happens by accident, because
		 * swap_backing_dev_info is bust: it doesn't reflect the
		 * congestion state of the swapdevs.  Easy to fix, if needed.
		 * See swapfile.c:page_queue_congested().
		 */
		if (PageDirty(page)) {
			if (referenced)
				goto keep_locked;
			if (!is_page_cache_freeable(page))
				goto keep_locked;
			if (!mapping)
				goto keep_locked;
			if (mapping->a_ops->writepage == NULL)
				goto activate_locked;
			if (!may_enter_fs)
				goto keep_locked;
			if (!may_write_to_queue(mapping->backing_dev_info))
				goto keep_locked;
			spin_lock(&mapping->page_lock);
			if (test_clear_page_dirty(page)) {
				int res;
				struct writeback_control wbc = {
					.sync_mode = WB_SYNC_NONE,
					.nr_to_write = SWAP_CLUSTER_MAX,
					.nonblocking = 1,
					.for_reclaim = 1,
				};

				list_move(&page->list, &mapping->locked_pages);
				spin_unlock(&mapping->page_lock);

				SetPageReclaim(page);
				res = mapping->a_ops->writepage(page, &wbc);
				if (res < 0)
					handle_write_error(mapping, page, res);
				if (res == WRITEPAGE_ACTIVATE) {
					ClearPageReclaim(page);
					goto activate_locked;
				}
				if (!PageWriteback(page)) {
					/* synchronous write or broken a_ops? */
					ClearPageReclaim(page);
				}
				goto keep;
			}
			spin_unlock(&mapping->page_lock);
		}

		/*
		 * If the page has buffers, try to free the buffer mappings
		 * associated with this page. If we succeed we try to free
		 * the page as well.
		 *
		 * We do this even if the page is PageDirty().
		 * try_to_release_page() does not perform I/O, but it is
		 * possible for a page to have PageDirty set, but it is actually
		 * clean (all its buffers are clean).  This happens if the
		 * buffers were written out directly, with submit_bh(). ext3
		 * will do this, as well as the blockdev mapping. 
		 * try_to_release_page() will discover that cleanness and will
		 * drop the buffers and mark the page clean - it can be freed.
		 *
		 * Rarely, pages can have buffers and no ->mapping.  These are
		 * the pages which were not successfully invalidated in
		 * truncate_complete_page().  We try to drop those buffers here
		 * and if that worked, and the page is no longer mapped into
		 * process address space (page_count == 0) it can be freed.
		 * Otherwise, leave the page on the LRU so it is swappable.
		 */
		if (PagePrivate(page)) {
			if (!try_to_release_page(page, gfp_mask))
				goto activate_locked;
			if (!mapping && page_count(page) == 1)
				goto free_it;
		}

		if (!mapping)
			goto keep_locked;	/* truncate got there first */

		spin_lock(&mapping->page_lock);

		/*
		 * The non-racy check for busy page.  It is critical to check
		 * PageDirty _after_ making sure that the page is freeable and
		 * not in use by anybody. 	(pagecache + us == 2)
		 */
		if (page_count(page) != 2 || PageDirty(page)) {
			spin_unlock(&mapping->page_lock);
			goto keep_locked;
		}

#ifdef CONFIG_SWAP
		if (PageSwapCache(page)) {
			swp_entry_t swap = { .val = page->index };
			__delete_from_swap_cache(page);
			spin_unlock(&mapping->page_lock);
			swap_free(swap);
			__put_page(page);	/* The pagecache ref */
			goto free_it;
		}
#endif /* CONFIG_SWAP */

		__remove_from_page_cache(page);
		spin_unlock(&mapping->page_lock);
		__put_page(page);

free_it:
		unlock_page(page);
		ret++;
		if (!pagevec_add(&freed_pvec, page))
			__pagevec_release_nonlru(&freed_pvec);
		continue;

activate_locked:
		SetPageActive(page);
		pgactivate++;
keep_locked:
		unlock_page(page);
keep:
		list_add(&page->lru, &ret_pages);
		BUG_ON(PageLRU(page));
	}
	list_splice(&ret_pages, page_list);
	if (pagevec_count(&freed_pvec))
		__pagevec_release_nonlru(&freed_pvec);
	mod_page_state(pgactivate, pgactivate);
	return ret;
}

/*
 * zone->lru_lock is heavily contented.  We relieve it by quickly privatising
 * a batch of pages and working on them outside the lock.  Any pages which were
 * not freed will be added back to the LRU.
 *
 * shrink_cache() is passed the number of pages to scan and returns the number
 * of pages which were reclaimed.
 *
 * For pagecache intensive workloads, the first loop here is the hottest spot
 * in the kernel (apart from the copy_*_user functions).
 */
static int
shrink_cache(struct zone *zone, unsigned int gfp_mask,
		int max_scan, int *total_scanned)
{
	LIST_HEAD(page_list);
	struct pagevec pvec;
	int ret = 0;

	pagevec_init(&pvec, 1);

	lru_add_drain();
	spin_lock_irq(&zone->lru_lock);
	while (max_scan > 0) {
		struct page *page;
		int nr_taken = 0;
		int nr_scan = 0;
		int nr_freed;

		while (nr_scan++ < SWAP_CLUSTER_MAX &&
				!list_empty(&zone->inactive_list)) {
			page = list_entry(zone->inactive_list.prev,
						struct page, lru);

			prefetchw_prev_lru_page(page,
						&zone->inactive_list, flags);

			if (!TestClearPageLRU(page))
				BUG();
			list_del(&page->lru);
			if (page_count(page) == 0) {
				/* It is currently in pagevec_release() */
				SetPageLRU(page);
				list_add(&page->lru, &zone->inactive_list);
				continue;
			}
			list_add(&page->lru, &page_list);
			page_cache_get(page);
			nr_taken++;
		}
		zone->nr_inactive -= nr_taken;
		zone->pages_scanned += nr_taken;
		spin_unlock_irq(&zone->lru_lock);

		if (nr_taken == 0)
			goto done;

		max_scan -= nr_scan;
		if (current_is_kswapd())
			mod_page_state_zone(zone, pgscan_kswapd, nr_scan);
		else
			mod_page_state_zone(zone, pgscan_direct, nr_scan);
		nr_freed = shrink_list(&page_list, gfp_mask, total_scanned);
		*total_scanned += nr_taken;
		if (current_is_kswapd())
			mod_page_state(kswapd_steal, nr_freed);
		mod_page_state_zone(zone, pgsteal, nr_freed);

		ret += nr_freed;
		if (nr_freed <= 0 && list_empty(&page_list))
			goto done;

		spin_lock_irq(&zone->lru_lock);
		/*
		 * Put back any unfreeable pages.
		 */
		while (!list_empty(&page_list)) {
			page = list_entry(page_list.prev, struct page, lru);
			if (TestSetPageLRU(page))
				BUG();
			list_del(&page->lru);
			if (PageActive(page))
				add_page_to_active_list(zone, page);
			else
				add_page_to_inactive_list(zone, page);
			if (!pagevec_add(&pvec, page)) {
				spin_unlock_irq(&zone->lru_lock);
				__pagevec_release(&pvec);
				spin_lock_irq(&zone->lru_lock);
			}
		}
  	}
	spin_unlock_irq(&zone->lru_lock);
done:
	pagevec_release(&pvec);
	return ret;
}

/*
 * This moves pages from the active list to the inactive list.
 *
 * We move them the other way if the page is referenced by one or more
 * processes, from rmap.
 *
 * If the pages are mostly unmapped, the processing is fast and it is
 * appropriate to hold zone->lru_lock across the whole operation.  But if
 * the pages are mapped, the processing is slow (page_referenced()) so we
 * should drop zone->lru_lock around each page.  It's impossible to balance
 * this, so instead we remove the pages from the LRU while processing them.
 * It is safe to rely on PG_active against the non-LRU pages in here because
 * nobody will play with that bit on a non-LRU page.
 *
 * The downside is that we have to touch page->count against each page.
 * But we had to alter page->flags anyway.
 */
static void
refill_inactive_zone(struct zone *zone, const int nr_pages_in,
			struct page_state *ps)
{
	int pgmoved;
	int pgdeactivate = 0;
	int nr_pages = nr_pages_in;
	LIST_HEAD(l_hold);	/* The pages which were snipped off */
	LIST_HEAD(l_inactive);	/* Pages to go onto the inactive_list */
	LIST_HEAD(l_active);	/* Pages to go onto the active_list */
	struct page *page;
	struct pagevec pvec;
	int reclaim_mapped = 0;
	long mapped_ratio;
	long distress;
	long swap_tendency;

	lru_add_drain();
	pgmoved = 0;
	spin_lock_irq(&zone->lru_lock);
	while (nr_pages && !list_empty(&zone->active_list)) {
		page = list_entry(zone->active_list.prev, struct page, lru);
		prefetchw_prev_lru_page(page, &zone->active_list, flags);
		if (!TestClearPageLRU(page))
			BUG();
		list_del(&page->lru);
		if (page_count(page) == 0) {
			/* It is currently in pagevec_release() */
			SetPageLRU(page);
			list_add(&page->lru, &zone->active_list);
		} else {
			page_cache_get(page);
			list_add(&page->lru, &l_hold);
			pgmoved++;
		}
		nr_pages--;
	}
	zone->nr_active -= pgmoved;
	spin_unlock_irq(&zone->lru_lock);

	/*
	 * `distress' is a measure of how much trouble we're having reclaiming
	 * pages.  0 -> no problems.  100 -> great trouble.
	 */
	distress = 100 >> zone->prev_priority;

	/*
	 * The point of this algorithm is to decide when to start reclaiming
	 * mapped memory instead of just pagecache.  Work out how much memory
	 * is mapped.
	 */
	mapped_ratio = (ps->nr_mapped * 100) / total_memory;

	/*
	 * Now decide how much we really want to unmap some pages.  The mapped
	 * ratio is downgraded - just because there's a lot of mapped memory
	 * doesn't necessarily mean that page reclaim isn't succeeding.
	 *
	 * The distress ratio is important - we don't want to start going oom.
	 *
	 * A 100% value of vm_swappiness overrides this algorithm altogether.
	 */
	swap_tendency = mapped_ratio / 2 + distress + vm_swappiness;

	/*
	 * Now use this metric to decide whether to start moving mapped memory
	 * onto the inactive list.
	 */
	if (swap_tendency >= 100)
		reclaim_mapped = 1;

	while (!list_empty(&l_hold)) {
		page = list_entry(l_hold.prev, struct page, lru);
		list_del(&page->lru);
		if (page_mapped(page)) {
			if (!reclaim_mapped) {
				list_add(&page->lru, &l_active);
				continue;
			}
			pte_chain_lock(page);
			if (page_referenced(page)) {
				pte_chain_unlock(page);
				list_add(&page->lru, &l_active);
				continue;
			}
			pte_chain_unlock(page);
		}
		/*
		 * FIXME: need to consider page_count(page) here if/when we
		 * reap orphaned pages via the LRU (Daniel's locking stuff)
		 */
		if (total_swap_pages == 0 && !page->mapping &&
						!PagePrivate(page)) {
			list_add(&page->lru, &l_active);
			continue;
		}
		list_add(&page->lru, &l_inactive);
	}

	pagevec_init(&pvec, 1);
	pgmoved = 0;
	spin_lock_irq(&zone->lru_lock);
	while (!list_empty(&l_inactive)) {
		page = list_entry(l_inactive.prev, struct page, lru);
		prefetchw_prev_lru_page(page, &l_inactive, flags);
		if (TestSetPageLRU(page))
			BUG();
		if (!TestClearPageActive(page))
			BUG();
		list_move(&page->lru, &zone->inactive_list);
		pgmoved++;
		if (!pagevec_add(&pvec, page)) {
			zone->nr_inactive += pgmoved;
			spin_unlock_irq(&zone->lru_lock);
			pgdeactivate += pgmoved;
			pgmoved = 0;
			if (buffer_heads_over_limit)
				pagevec_strip(&pvec);
			__pagevec_release(&pvec);
			spin_lock_irq(&zone->lru_lock);
		}
	}
	zone->nr_inactive += pgmoved;
	pgdeactivate += pgmoved;
	if (buffer_heads_over_limit) {
		spin_unlock_irq(&zone->lru_lock);
		pagevec_strip(&pvec);
		spin_lock_irq(&zone->lru_lock);
	}

	pgmoved = 0;
	while (!list_empty(&l_active)) {
		page = list_entry(l_active.prev, struct page, lru);
		prefetchw_prev_lru_page(page, &l_active, flags);
		if (TestSetPageLRU(page))
			BUG();
		BUG_ON(!PageActive(page));
		list_move(&page->lru, &zone->active_list);
		pgmoved++;
		if (!pagevec_add(&pvec, page)) {
			zone->nr_active += pgmoved;
			pgmoved = 0;
			spin_unlock_irq(&zone->lru_lock);
			__pagevec_release(&pvec);
			spin_lock_irq(&zone->lru_lock);
		}
	}
	zone->nr_active += pgmoved;
	spin_unlock_irq(&zone->lru_lock);
	pagevec_release(&pvec);

	mod_page_state_zone(zone, pgrefill, nr_pages_in - nr_pages);
	mod_page_state(pgdeactivate, pgdeactivate);
}

/*
 * Scan `nr_pages' from this zone.  Returns the number of reclaimed pages.
 * This is a basic per-zone page freer.  Used by both kswapd and direct reclaim.
 */
static int
shrink_zone(struct zone *zone, int max_scan, unsigned int gfp_mask,
		int *total_scanned, struct page_state *ps)
{
	unsigned long ratio;
	int count;

	/*
	 * Try to keep the active list 2/3 of the size of the cache.  And
	 * make sure that refill_inactive is given a decent number of pages.
	 *
	 * The "ratio+1" here is important.  With pagecache-intensive workloads
	 * the inactive list is huge, and `ratio' evaluates to zero all the
	 * time.  Which pins the active list memory.  So we add one to `ratio'
	 * just to make sure that the kernel will slowly sift through the
	 * active list.
	 */
	ratio = (unsigned long)SWAP_CLUSTER_MAX * zone->nr_active /
				((zone->nr_inactive | 1) * 2);

	atomic_add(ratio+1, &zone->nr_scan_active);
	count = atomic_read(&zone->nr_scan_active);
	if (count >= SWAP_CLUSTER_MAX) {
		atomic_set(&zone->nr_scan_active, 0);
		refill_inactive_zone(zone, count, ps);
	}

	atomic_add(max_scan, &zone->nr_scan_inactive);
	count = atomic_read(&zone->nr_scan_inactive);
	if (count >= SWAP_CLUSTER_MAX) {
		atomic_set(&zone->nr_scan_inactive, 0);
		return shrink_cache(zone, gfp_mask, count, total_scanned);
	}
	return 0;
}

/*
 * This is the direct reclaim path, for page-allocating processes.  We only
 * try to reclaim pages from zones which will satisfy the caller's allocation
 * request.
 *
 * We reclaim from a zone even if that zone is over pages_high.  Because:
 * a) The caller may be trying to free *extra* pages to satisfy a higher-order
 *    allocation or
 * b) The zones may be over pages_high but they must go *over* pages_high to
 *    satisfy the `incremental min' zone defense algorithm.
 *
 * Returns the number of reclaimed pages.
 *
 * If a zone is deemed to be full of pinned pages then just give it a light
 * scan then give up on it.
 */
static int
shrink_caches(struct zone **zones, int priority, int *total_scanned,
		int gfp_mask, struct page_state *ps)
{
	int ret = 0;
	int i;

	for (i = 0; zones[i] != NULL; i++) {
		struct zone *zone = zones[i];
		int max_scan;

		if (zone->free_pages < zone->pages_high)
			zone->temp_priority = priority;

		if (zone->all_unreclaimable && priority != DEF_PRIORITY)
			continue;	/* Let kswapd poll it */

		max_scan = zone->nr_inactive >> priority;
		ret += shrink_zone(zone, max_scan, gfp_mask, total_scanned, ps);
	}
	return ret;
}
 
/*
 * This is the main entry point to direct page reclaim.
 *
 * If a full scan of the inactive list fails to free enough memory then we
 * are "out of memory" and something needs to be killed.
 *
 * If the caller is !__GFP_FS then the probability of a failure is reasonably
 * high - the zone may be full of dirty or under-writeback pages, which this
 * caller can't do much about.  So for !__GFP_FS callers, we just perform a
 * small LRU walk and if that didn't work out, fail the allocation back to the
 * caller.  GFP_NOFS allocators need to know how to deal with it.  Kicking
 * bdflush, waiting and retrying will work.
 *
 * This is a fairly lame algorithm - it can result in excessive CPU burning and
 * excessive rotation of the inactive list, which is _supposed_ to be an LRU,
 * yes?
 */
int try_to_free_pages(struct zone **zones,
		unsigned int gfp_mask, unsigned int order)
{
	int priority;
	int ret = 0;
	int nr_reclaimed = 0;
	struct reclaim_state *reclaim_state = current->reclaim_state;
	int i;

	inc_page_state(allocstall);

	for (i = 0; zones[i] != 0; i++)
		zones[i]->temp_priority = DEF_PRIORITY;

	for (priority = DEF_PRIORITY; priority >= 0; priority--) {
		int total_scanned = 0;
		struct page_state ps;

		get_page_state(&ps);
		nr_reclaimed += shrink_caches(zones, priority, &total_scanned,
						gfp_mask, &ps);
		shrink_slab(total_scanned, gfp_mask);
		if (reclaim_state) {
			nr_reclaimed += reclaim_state->reclaimed_slab;
			reclaim_state->reclaimed_slab = 0;
		}
		if (nr_reclaimed >= SWAP_CLUSTER_MAX) {
			ret = 1;
			goto out;
		}
		if (!(gfp_mask & __GFP_FS))
			break;		/* Let the caller handle it */
		/*
		 * Try to write back as many pages as we just scanned.  Not
		 * sure if that makes sense, but it's an attempt to avoid
		 * creating IO storms unnecessarily
		 */
		wakeup_bdflush(total_scanned);

		/* Take a nap, wait for some writeback to complete */
		if (total_scanned && priority < DEF_PRIORITY - 2)
			blk_congestion_wait(WRITE, HZ/10);
	}
	if ((gfp_mask & __GFP_FS) && !(gfp_mask & __GFP_NORETRY))
		out_of_memory();
out:
	for (i = 0; zones[i] != 0; i++)
		zones[i]->prev_priority = zones[i]->temp_priority;
	return ret;
}

/*
 * For kswapd, balance_pgdat() will work across all this node's zones until
 * they are all at pages_high.
 *
 * If `nr_pages' is non-zero then it is the number of pages which are to be
 * reclaimed, regardless of the zone occupancies.  This is a software suspend
 * special.
 *
 * Returns the number of pages which were actually freed.
 *
 * There is special handling here for zones which are full of pinned pages.
 * This can happen if the pages are all mlocked, or if they are all used by
 * device drivers (say, ZONE_DMA).  Or if they are all in use by hugetlb.
 * What we do is to detect the case where all pages in the zone have been
 * scanned twice and there has been zero successful reclaim.  Mark the zone as
 * dead and from now on, only perform a short scan.  Basically we're polling
 * the zone for when the problem goes away.
 *
 * kswapd scans the zones in the highmem->normal->dma direction.  It skips
 * zones which have free_pages > pages_high, but once a zone is found to have
 * free_pages <= pages_high, we scan that zone and the lower zones regardless
 * of the number of free pages in the lower zones.  This interoperates with
 * the page allocator fallback scheme to ensure that aging of pages is balanced
 * across the zones.
 */
static int balance_pgdat(pg_data_t *pgdat, int nr_pages, struct page_state *ps)
{
	int to_free = nr_pages;
	int priority;
	int i;
	struct reclaim_state *reclaim_state = current->reclaim_state;

	inc_page_state(pageoutrun);

	for (i = 0; i < pgdat->nr_zones; i++) {
		struct zone *zone = pgdat->node_zones + i;

		zone->temp_priority = DEF_PRIORITY;
	}

	for (priority = DEF_PRIORITY; priority; priority--) {
		int all_zones_ok = 1;
		int pages_scanned = 0;
		int end_zone = 0;	/* Inclusive.  0 = ZONE_DMA */


		if (nr_pages == 0) {
			/*
			 * Scan in the highmem->dma direction for the highest
			 * zone which needs scanning
			 */
			for (i = pgdat->nr_zones - 1; i >= 0; i--) {
				struct zone *zone = pgdat->node_zones + i;

				if (zone->all_unreclaimable &&
						priority != DEF_PRIORITY)
					continue;

				if (zone->free_pages <= zone->pages_high) {
					end_zone = i;
					goto scan;
				}
			}
			goto out;
		} else {
			end_zone = pgdat->nr_zones - 1;
		}
scan:
		/*
		 * Now scan the zone in the dma->highmem direction, stopping
		 * at the last zone which needs scanning.
		 *
		 * We do this because the page allocator works in the opposite
		 * direction.  This prevents the page allocator from allocating
		 * pages behind kswapd's direction of progress, which would
		 * cause too much scanning of the lower zones.
		 */
		for (i = 0; i <= end_zone; i++) {
			struct zone *zone = pgdat->node_zones + i;
			int total_scanned = 0;
			int max_scan;
			int reclaimed;

			if (zone->all_unreclaimable && priority != DEF_PRIORITY)
				continue;

			if (nr_pages == 0) {	/* Not software suspend */
				if (zone->free_pages <= zone->pages_high)
					all_zones_ok = 0;
			}
			zone->temp_priority = priority;
			max_scan = zone->nr_inactive >> priority;
			reclaimed = shrink_zone(zone, max_scan, GFP_KERNEL,
					&total_scanned, ps);
			total_scanned += pages_scanned;
			reclaim_state->reclaimed_slab = 0;
			shrink_slab(total_scanned, GFP_KERNEL);
			reclaimed += reclaim_state->reclaimed_slab;
			to_free -= reclaimed;
			if (zone->all_unreclaimable)
				continue;
			if (zone->pages_scanned > zone->present_pages * 2)
				zone->all_unreclaimable = 1;
		}
		if (nr_pages && to_free > 0)
			continue;	/* swsusp: need to do more work */
		if (all_zones_ok)
			break;		/* kswapd: all done */
		/*
		 * OK, kswapd is getting into trouble.  Take a nap, then take
		 * another pass across the zones.
		 */
		if (pages_scanned && priority < DEF_PRIORITY - 2)
			blk_congestion_wait(WRITE, HZ/10);
	}
out:
	for (i = 0; i < pgdat->nr_zones; i++) {
		struct zone *zone = pgdat->node_zones + i;

		zone->prev_priority = zone->temp_priority;
	}
	return nr_pages - to_free;
}

/*
 * The background pageout daemon, started as a kernel thread
 * from the init process. 
 *
 * This basically trickles out pages so that we have _some_
 * free memory available even if there is no other activity
 * that frees anything up. This is needed for things like routing
 * etc, where we otherwise might have all activity going on in
 * asynchronous contexts that cannot page things out.
 *
 * If there are applications that are active memory-allocators
 * (most normal use), this basically shouldn't matter.
 */
int kswapd(void *p)
{
	pg_data_t *pgdat = (pg_data_t*)p;
	struct task_struct *tsk = current;
	DEFINE_WAIT(wait);
	struct reclaim_state reclaim_state = {
		.reclaimed_slab = 0,
	};
	cpumask_t cpumask;

	daemonize("kswapd%d", pgdat->node_id);
	cpumask = node_to_cpumask(pgdat->node_id);
	if (!cpus_empty(cpumask))
		set_cpus_allowed(tsk, cpumask);
	current->reclaim_state = &reclaim_state;

	/*
	 * Tell the memory management that we're a "memory allocator",
	 * and that if we need more memory we should get access to it
	 * regardless (see "__alloc_pages()"). "kswapd" should
	 * never get caught in the normal page freeing logic.
	 *
	 * (Kswapd normally doesn't need memory anyway, but sometimes
	 * you need a small amount of memory in order to be able to
	 * page out something else, and this flag essentially protects
	 * us from recursively trying to free more memory as we're
	 * trying to free the first piece of memory in the first place).
	 */
	tsk->flags |= PF_MEMALLOC|PF_KSWAPD;

	for ( ; ; ) {
		struct page_state ps;

		if (current->flags & PF_FREEZE)
			refrigerator(PF_IOTHREAD);
		prepare_to_wait(&pgdat->kswapd_wait, &wait, TASK_INTERRUPTIBLE);
		schedule();
		finish_wait(&pgdat->kswapd_wait, &wait);
		get_page_state(&ps);
		balance_pgdat(pgdat, 0, &ps);
	}
}

/*
 * A zone is low on free memory, so wake its kswapd task to service it.
 */
void wakeup_kswapd(struct zone *zone)
{
	if (zone->free_pages > zone->pages_low)
		return;
	if (!waitqueue_active(&zone->zone_pgdat->kswapd_wait))
		return;
	wake_up_interruptible(&zone->zone_pgdat->kswapd_wait);
}

#ifdef CONFIG_PM
/*
 * Try to free `nr_pages' of memory, system-wide.  Returns the number of freed
 * pages.
 */
int shrink_all_memory(int nr_pages)
{
	pg_data_t *pgdat;
	int nr_to_free = nr_pages;
	int ret = 0;
	struct reclaim_state reclaim_state = {
		.reclaimed_slab = 0,
	};

	current->reclaim_state = &reclaim_state;
	for_each_pgdat(pgdat) {
		int freed;
		struct page_state ps;

		get_page_state(&ps);
		freed = balance_pgdat(pgdat, nr_to_free, &ps);
		ret += freed;
		nr_to_free -= freed;
		if (nr_to_free <= 0)
			break;
	}
	current->reclaim_state = NULL;
	return ret;
}
#endif

static int __init kswapd_init(void)
{
	pg_data_t *pgdat;
	swap_setup();
	for_each_pgdat(pgdat)
		kernel_thread(kswapd, pgdat, CLONE_KERNEL);
	total_memory = nr_free_pagecache_pages();
	return 0;
}

module_init(kswapd_init)
