/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (C) 2025 Pavel Shago <pavel@shago.dev>
 */

/* FOR DEMO USE ONLY */

#include <linux/device-mapper.h>

#include <linux/bio.h>
#include <linux/blk_types.h>
#include <linux/blkdev.h>
#include <linux/bvec.h>
#include <linux/gfp_types.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/container_of.h>

#define DM_MSG_PREFIX "dmp"
#define DMP_ERR_PREFIX "dmp: "
// more convenient naming is dm-proxy and proxy
// but as well as in sysfs paths doing task's declared naming

struct proxy_data {
	unsigned int read_calls;
	unsigned int write_calls;
	unsigned int read_bytes;
	unsigned int write_bytes;
};

struct proxier {
	struct dm_dev *dev;
	sector_t start;
	struct kobject stats_kobj;
	struct proxy_data stats;
};

static ssize_t volumes_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buffer)
{
	struct proxier *meta = container_of(kobj, struct proxier, stats_kobj);
	struct proxy_data *info = &meta->stats;

	unsigned int read_avg =
		info->read_calls ? info->read_bytes / info->read_calls : 0;
	unsigned int write_avg =
		info->write_calls ? info->write_bytes / info->write_calls : 0;
	unsigned int total_calls = info->read_calls + info->write_calls;
	unsigned int total_avg =
		total_calls ?
		(info->read_bytes + info->write_bytes) / total_calls : 0;

	return sysfs_emit(buffer,
			  "read:\nreqs: %u\navg size: %u\n"
			  "write:\nreqs: %u\navg size: %u\n"
			  "total:\nreqs: %u\navg size: %u\n",
			  info->read_calls, read_avg, info->write_calls,
			  write_avg, total_calls, total_avg);
}
static struct kobj_attribute stats_kobj_attribute = __ATTR_RO(volumes);

static void dmp_kobj_release(struct kobject *kobj)
{
}

static struct kobj_type ktype_default = {
	.release = dmp_kobj_release,
	.sysfs_ops = &kobj_sysfs_ops,
};

static int proxy_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct proxier *meta;
	int ret;

	if (argc != 1) {
		ti->error = "Invalid argument count (expected 1)";
		DMERR(DMP_ERR_PREFIX "%s", ti->error);
		return -EINVAL;
	}

	meta = kmalloc(sizeof(*meta), GFP_KERNEL);
	if (!meta) {
		ti->error = "Failed to allocate memory (for meta info)";
		DMERR(DMP_ERR_PREFIX "%s", ti->error);
		return -ENOMEM;
	}

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
			    &meta->dev);
	if (ret) {
		ti->error = "Failed to get the device";
		DMERR(DMP_ERR_PREFIX "%s", ti->error);
		kfree(meta);
		return ret;
	}

	ret = kobject_init_and_add(&meta->stats_kobj, &ktype_default,
				   &THIS_MODULE->mkobj.kobj, "stat_%s",
				   dm_device_name(dm_table_get_md(ti->table)));
	if (ret) {
		ti->error = "Failed to create statistical kobj";
		DMERR(DMP_ERR_PREFIX "%s", ti->error);
		dm_put_device(ti, meta->dev);
		kfree(meta);
		return -ENOMEM;
	}

	ret = sysfs_create_file(&meta->stats_kobj, &stats_kobj_attribute.attr);
	if (ret) {
		ti->error = "Failed to create sysfs file";
		DMERR(DMP_ERR_PREFIX "%s", ti->error);
		kobject_put(&meta->stats_kobj);
		dm_put_device(ti, meta->dev);
		kfree(meta);
		return ret;
	}

	ti->private = meta;
	return 0;
}

static void proxy_dtr(struct dm_target *ti)
{
	struct proxier *meta = ti->private;

	sysfs_remove_file(&meta->stats_kobj, &stats_kobj_attribute.attr);
	dm_put_device(ti, meta->dev);
	kobject_put(&meta->stats_kobj);
	kfree(meta);
}

static int proxy_map(struct dm_target *ti, struct bio *bio)
{
	struct proxier *meta = ti->private;
	unsigned int block_size = bio->bi_iter.bi_size;

	/*
	this debug print is showing that 22 extra
	read queries are from udev worker

	pr_info("dmp: bio op=%d size=%u sector=%llu task=%s\n",
	bio_op(bio), bio->bi_iter.bi_size,
	(unsigned long long)bio->bi_iter.bi_sector, current->comm);
	*/

	if (bio_op(bio) == REQ_OP_READ) {
		meta->stats.read_calls++;
		meta->stats.read_bytes += block_size;
	} else if (bio_op(bio) == REQ_OP_WRITE) {
		meta->stats.write_calls++;
		meta->stats.write_bytes += block_size;
	}

	bio_set_dev(bio, meta->dev->bdev);
	bio->bi_iter.bi_sector += meta->start;

	submit_bio_noacct(bio);
	return DM_MAPIO_SUBMITTED;
}

static struct target_type proxy_target = {
	.name = "dmp",
	.version = { 1, 1, 0 },
	.module = THIS_MODULE,
	.ctr = proxy_ctr,
	.dtr = proxy_dtr,
	.map = proxy_map,
};
module_dm(proxy);

MODULE_AUTHOR("Pavel Shago <pavel@shago.dev>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DM_NAME " drives statistics target");
