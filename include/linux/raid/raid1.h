#ifndef _RAID1_H
#define _RAID1_H

#include <linux/raid/md.h>

typedef struct mirror_info mirror_info_t;

struct mirror_info {
	int		number;
	int		raid_disk;
	kdev_t		dev;
	int		sect_limit;
	int		head_position;

	/*
	 * State bits:
	 */
	int		operational;
	int		write_only;
	int		spare;

	int		used_slot;
};

typedef struct r1bio_s r1bio_t;

struct r1_private_data_s {
	mddev_t			*mddev;
	mirror_info_t		mirrors[MD_SB_DISKS];
	int			nr_disks;
	int			raid_disks;
	int			working_disks;
	int			last_used;
	sector_t		next_sect;
	int			sect_count;
	mdk_thread_t		*thread, *resync_thread;
	int			resync_mirrors;
	mirror_info_t		*spare;
	spinlock_t		device_lock;

	/* for use when syncing mirrors: */
	unsigned long	start_active, start_ready,
		start_pending, start_future;
	int	cnt_done, cnt_active, cnt_ready,
		cnt_pending, cnt_future;
	int	phase;
	int	window;
	wait_queue_head_t	wait_done;
	wait_queue_head_t	wait_ready;
	spinlock_t		segment_lock;

	mempool_t *r1bio_pool;
	mempool_t *r1buf_pool;
};

typedef struct r1_private_data_s conf_t;

/*
 * this is the only point in the RAID code where we violate
 * C type safety. mddev->private is an 'opaque' pointer.
 */
#define mddev_to_conf(mddev) ((conf_t *) mddev->private)

/*
 * this is our 'private' 'collective' RAID1 buffer head.
 * it contains information about what kind of IO operations were started
 * for this RAID1 operation, and about their status:
 */

struct r1bio_s {
	atomic_t		remaining; /* 'have we finished' count,
					    * used from IRQ handlers
					    */
	int			cmd;
	sector_t		sector;
	unsigned long		state;
	mddev_t			*mddev;
	/*
	 * original bio going to /dev/mdx
	 */
	struct bio		*master_bio;
	/*
	 * if the IO is in READ direction, then this bio is used:
	 */
	struct bio		*read_bio;
	/*
	 * if the IO is in WRITE direction, then multiple bios are used:
	 */
	struct bio		*write_bios[MD_SB_DISKS];

	r1bio_t			*next_r1; /* next for retry or in free list */
	struct list_head	retry_list;
};

/* bits for r1bio.state */
#define	R1BIO_Uptodate	1
#define	R1BIO_SyncPhase	2
#endif
