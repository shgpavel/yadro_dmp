/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (C) 2025 Pavel Shago <pavel@shago.dev>
 */

/* FOR DEMO USE ONLY */

#include <linux/device-mapper.h>

#include <linux/bio.h>
#include <linux/blk_types.h>
#include <linux/blkdev.h>
#include <linux/gfp_types.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/container_of.h>

#define DM_MSG_PREFIX "dmp"

struct proxy_data {
	unsigned int read_calls;
	unsigned int write_calls;
	u64          read_bytes;
	u64          write_bytes;
};

struct proxier {
	struct dm_dev    *dev;
	spinlock_t        stats_lock;
	struct kobject    stats_kobj;
	struct proxy_data stats;
};

static ssize_t
volumes_show(struct kobject *kobj, struct kobj_attribute *attr, char *buffer)
{
	struct proxier   *meta = container_of(kobj, struct proxier, stats_kobj);
	struct proxy_data snap;
	unsigned long     flags;
	u64               read_avg, write_avg, total_calls, total_avg;

	spin_lock_irqsave(&meta->stats_lock, flags);
	snap = meta->stats;
	spin_unlock_irqrestore(&meta->stats_lock, flags);

	read_avg    = snap.read_calls ? snap.read_bytes / snap.read_calls : 0;
	write_avg   = snap.write_calls ? snap.write_bytes / snap.write_calls : 0;
	total_calls = snap.read_calls + snap.write_calls;
	total_avg   = total_calls ?
	                      (snap.read_bytes + snap.write_bytes) / total_calls :
	                      0;

	return sysfs_emit(buffer,
	                  "read:\nreqs: %u\navg size: %llu\n"
	                  "write:\nreqs: %u\navg size: %llu\n"
	                  "total:\nreqs: %llu\navg size: %llu\n",
	                  snap.read_calls, read_avg, snap.write_calls, write_avg,
	                  total_calls, total_avg);
}

static struct kobj_attribute stats_kobj_attribute = __ATTR_RO(volumes);

static void
dmp_kobj_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct proxier, stats_kobj));
}

static struct kobj_type ktype_default = {
	.release   = dmp_kobj_release,
	.sysfs_ops = &kobj_sysfs_ops,
};

static int
proxy_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct proxier *meta;
	int             ret;

	if (argc != 1) {
		ti->error = "Invalid argument count (expected 1)";
		return -EINVAL;
	}

	meta = kzalloc(sizeof(*meta), GFP_KERNEL);
	if (!meta) {
		ti->error = "Failed to allocate memory (for meta info)";
		return -ENOMEM;
	}

	spin_lock_init(&meta->stats_lock);

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &meta->dev);
	if (ret) {
		ti->error = "Failed to get the device";
		kfree(meta);
		return ret;
	}

	ret = kobject_init_and_add(&meta->stats_kobj, &ktype_default,
	                           &THIS_MODULE->mkobj.kobj, "stat_%s",
	                           dm_device_name(dm_table_get_md(ti->table)));
	if (ret) {
		ti->error = "Failed to create statistical kobj";
		goto err_kobj;
	}

	ret = sysfs_create_file(&meta->stats_kobj, &stats_kobj_attribute.attr);
	if (ret) {
		ti->error = "Failed to create sysfs file";
		kobject_del(&meta->stats_kobj);
		goto err_kobj;
	}

	ti->private = meta;
	return 0;

err_kobj:
	dm_put_device(ti, meta->dev);
	kobject_put(&meta->stats_kobj);
	return ret;
}

static void
proxy_dtr(struct dm_target *ti)
{
	struct proxier *meta = ti->private;

	sysfs_remove_file(&meta->stats_kobj, &stats_kobj_attribute.attr);
	dm_put_device(ti, meta->dev);
	kobject_del(&meta->stats_kobj);
	kobject_put(&meta->stats_kobj);
}

static int
proxy_map(struct dm_target *ti, struct bio *bio)
{
	struct proxier *meta = ti->private;
	unsigned long   flags;

	spin_lock_irqsave(&meta->stats_lock, flags);
	if (bio_op(bio) == REQ_OP_READ) {
		meta->stats.read_calls++;
		meta->stats.read_bytes += bio->bi_iter.bi_size;
	} else if (bio_op(bio) == REQ_OP_WRITE) {
		meta->stats.write_calls++;
		meta->stats.write_bytes += bio->bi_iter.bi_size;
	}
	spin_unlock_irqrestore(&meta->stats_lock, flags);

	bio_set_dev(bio, meta->dev->bdev);
	bio->bi_iter.bi_sector = dm_target_offset(ti, bio->bi_iter.bi_sector);

	submit_bio_noacct(bio);
	return DM_MAPIO_SUBMITTED;
}

static struct target_type proxy_target = {
	.name    = "dmp",
	.version = { 1, 2, 0 },
	.module  = THIS_MODULE,
	.ctr     = proxy_ctr,
	.dtr     = proxy_dtr,
	.map     = proxy_map,
};

static int __init
dm_proxy_init(void)
{
	int r = dm_register_target(&proxy_target);

	if (r)
		DMERR("failed to register target: %d", r);
	else
		DMINFO("loaded");
	return r;
}

static void __exit
dm_proxy_exit(void)
{
	dm_unregister_target(&proxy_target);
	DMINFO("unloaded");
}

module_init(dm_proxy_init);
module_exit(dm_proxy_exit);

MODULE_AUTHOR("Pavel Shago <pavel@shago.dev>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DM_NAME " statistics proxy");
MODULE_SOFTDEP("pre: dm-mod");
