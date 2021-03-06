#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/sysfs.h>
#include <linux/crc32.h>
#include <linux/miscdevice.h>
#include "fuse_i.h"
#include "pxd.h"

#define CREATE_TRACE_POINTS
#include "pxd_trace.h"
#undef CREATE_TRACE_POINTS
#include <linux/timekeeping.h>

/** enables time tracing */
//#define GD_TIME_LOG

#define pxd_printk(args...)
//#define pxd_printk(args...) printk(KERN_ERR args)

#define SECTOR_SIZE 512
#define SEGMENT_SIZE (1024 * 1024)

static dev_t pxd_major;
static DEFINE_IDA(pxd_minor_ida);

struct pxd_context {
	spinlock_t lock;
	struct list_head list;
	size_t num_devices;
	struct fuse_conn fc;
	struct file_operations fops;
	char name[256];
	int id;
	struct miscdevice miscdev;
	struct list_head pending_requests;
	bool init_sent;
};

struct pxd_context *pxd_contexts;
uint32_t pxd_num_contexts = 10;

module_param(pxd_num_contexts, uint, 0644);

struct pxd_device {
	uint64_t		dev_id;

	int			major;
	int			minor;
	struct gendisk		*disk;
	struct device 		dev;

	size_t 			size;

	spinlock_t		lock;

	struct list_head	node;

	int			open_count;

	bool			removing;

	struct pxd_context	*ctx;
};

static int pxd_bus_add_dev(struct pxd_device *pxd_dev);

static int pxd_open(struct block_device *bdev, fmode_t mode)
{
	struct pxd_device *pxd_dev = bdev->bd_disk->private_data;
	int err = 0;

	spin_lock(&pxd_dev->lock);
	if (pxd_dev->removing)
		err = -EBUSY;
	else
		pxd_dev->open_count++;
	spin_unlock(&pxd_dev->lock);

	if (!err)
		(void)get_device(&pxd_dev->dev);

	return err;
}

static void pxd_release(struct gendisk *disk, fmode_t mode)
{
	struct pxd_device *pxd_dev = disk->private_data;

	spin_lock(&pxd_dev->lock);
	pxd_dev->open_count--;
	spin_unlock(&pxd_dev->lock);

	put_device(&pxd_dev->dev);
}

static int pxd_ioctl(struct block_device *bdev, fmode_t mode,
			unsigned int cmd, unsigned long arg)
{
	return -ENOTTY;
}

static const struct block_device_operations pxd_bd_ops = {
	.owner			= THIS_MODULE,
	.open			= pxd_open,
	.release		= pxd_release,
	.ioctl			= pxd_ioctl,
};

static void pxd_update_stats(struct fuse_req *req, int rw)
{
	struct pxd_device *pxd_dev = req->queue->queuedata;
	int cpu = part_stat_lock();
	part_stat_inc(cpu, &pxd_dev->disk->part0, ios[rw]);
	part_stat_add(cpu, &pxd_dev->disk->part0, sectors[rw],
			req->bio->bi_iter.bi_size / SECTOR_SIZE);
	part_stat_unlock();
}

static void pxd_process_read_reply(struct fuse_conn *fc, struct fuse_req *req)
{
	struct timespec end;
	pxd_printk("%s: receive reply to %p(%lld) at %lld err %d\n", __func__, req,
			req->in.h.unique, req->misc.pxd_rdwr_in.offset, req->out.h.error);

	pxd_update_stats(req, 0);

	bio_endio(req->bio, req->out.h.error);
#ifdef GD_TIME_LOG
	ktime_get_ts(&end);
#endif
	trace_make_request_lat(READ, fc->reqctr, req->in.h.unique,
		fc->num_background, &req->start, &end);
}

static void pxd_process_write_reply(struct fuse_conn *fc, struct fuse_req *req)
{
	struct timespec end;
	pxd_printk("%s: receive reply to %p(%lld) err %d\n", __func__, req,
			req->in.h.unique, req->out.h.error);

	pxd_update_stats(req, 1);

	bio_endio(req->bio, req->out.h.error);
#ifdef GD_TIME_LOG
	ktime_get_ts(&end);
#endif
	trace_make_request_lat(WRITE, fc->reqctr, req->in.h.unique,
		fc->num_background, &req->start, &end);
}

static void pxd_make_request(struct request_queue *q, struct bio *bio)
{
	struct pxd_device *pxd_dev = q->queuedata;
	struct fuse_req *req;
	struct bio_vec bvec;
	struct bvec_iter iter;
	int i;
	struct timespec start, end;
	uint32_t crc = 0; 

	pxd_printk("%s: dev m %d g %lld %s at %ld len %d bytes %d pages flags %lx\n", __func__,
			pxd_dev->minor, pxd_dev->dev_id,
			bio_data_dir(bio) == WRITE ? "wr" : "rd",
			bio->bi_iter.bi_sector * SECTOR_SIZE, bio->bi_iter.bi_size,
			bio->bi_vcnt, bio->bi_rw);

	i = 0;
#ifdef GD_TIME_LOG
	ktime_get_ts(&start);
#endif
	do {
		req = fuse_get_req_for_background(&pxd_dev->ctx->fc, bio->bi_vcnt);
		if (!IS_ERR(req) || PTR_ERR(req) != -EINTR)
			break;
		++i;
	} while (1);
	if (i > 0) {
		printk(KERN_INFO "%s: request alloc (%d pages) EINTR retries %d\n",
			__func__, bio->bi_vcnt, i);
	}
	if (IS_ERR(req)) { // no NULL returned, not EINTR
		printk(KERN_ERR "%s: request alloc (%d pages) failed: %ld retries %d\n",
			__func__, bio->bi_vcnt, PTR_ERR(req), i);
		bio_io_error(bio);
		return;
	}

#ifdef GD_TIME_LOG
	ktime_get_ts(&end);
#endif
	trace_make_request_wait(bio_data_dir(bio), pxd_dev->ctx->fc.reqctr, i,
		req->in.h.unique, &start, &end);

	switch (bio->bi_rw & (REQ_WRITE | REQ_DISCARD)) {
	case REQ_WRITE:
#ifdef GD_TIME_LOG
		ktime_get_ts(&req->start);
#endif
		req->in.h.opcode = PXD_WRITE;
		req->in.numargs = 2;
		req->in.argpages = 1;
		req->in.args[0].size = sizeof(struct pxd_rdwr_in);
		req->in.args[0].value = &req->misc.pxd_rdwr_in;
		req->in.args[1].size = bio->bi_iter.bi_size;
		req->in.args[1].value = NULL;
		req->out.numargs = 0;
		req->end = pxd_process_write_reply;
		break;
	case 0:
#ifdef GD_TIME_LOG
		ktime_get_ts(&req->start);
#endif
		req->in.h.opcode = PXD_READ;
		req->in.numargs = 1;
		req->in.argpages = 0;
		req->in.args[0].size = sizeof(struct pxd_rdwr_in);
		req->in.args[0].value = &req->misc.pxd_rdwr_in;
		req->out.numargs = 1;
		req->out.argpages = 1;
		req->out.args[0].size = bio->bi_iter.bi_size;
		req->out.args[0].value = NULL;
		req->end = pxd_process_read_reply;
		break;
	case REQ_DISCARD:
	case REQ_WRITE | REQ_DISCARD:
		req->in.h.opcode = PXD_DISCARD;
		req->in.numargs = 1;
		req->in.args[0].size = sizeof(struct pxd_rdwr_in);
		req->in.args[0].value = &req->misc.pxd_rdwr_in;
		req->in.argpages = 0;
		req->out.numargs = 0;
		req->end = pxd_process_write_reply;
		break;
	}

	req->in.h.pid = current->pid;
        req->misc.pxd_rdwr_in.flags =
                ((bio->bi_rw & REQ_FLUSH) ? PXD_FLAGS_FLUSH : 0) |
                ((bio->bi_rw & REQ_FUA) ? PXD_FLAGS_FUA : 0) |
                ((bio->bi_rw & REQ_META) ? PXD_FLAGS_META : 0);
	req->misc.pxd_rdwr_in.minor = pxd_dev->minor;
	req->misc.pxd_rdwr_in.offset = bio->bi_iter.bi_sector * SECTOR_SIZE;
	req->misc.pxd_rdwr_in.size = bio->bi_iter.bi_size;
	req->num_pages = bio->bi_vcnt;
	if (bio->bi_vcnt) {
		i = 0;
		bio_for_each_segment(bvec, bio, iter) {
			BUG_ON(i >= req->max_pages);
			req->pages[i] = bvec.bv_page;
			req->page_descs[i].offset = bvec.bv_offset;
			req->page_descs[i].length = bvec.bv_len;
			++i;
		}
	}
	req->misc.pxd_rdwr_in.chksum = crc;
	req->bio = bio;
	req->queue = q;

	fuse_request_send_background(&pxd_dev->ctx->fc, req);
}

static int pxd_init_disk(struct pxd_device *pxd_dev, struct pxd_add_out *add)
{
	struct gendisk *disk;
	struct request_queue *q;

	/* create gendisk info */
	disk = alloc_disk(1);
	if (!disk)
		return -ENOMEM;

	snprintf(disk->disk_name, sizeof(disk->disk_name), "pxd%d",
		 pxd_dev->minor);
	disk->major = pxd_dev->major;
	disk->first_minor = pxd_dev->minor;
	disk->flags |= GENHD_FL_EXT_DEVT | GENHD_FL_NO_PART_SCAN;
	disk->fops = &pxd_bd_ops;
	disk->private_data = pxd_dev;

	q = blk_alloc_queue(GFP_KERNEL);
	if (!q)
		goto out_disk;

	blk_queue_make_request(q, pxd_make_request);

	blk_queue_max_hw_sectors(q, SEGMENT_SIZE / SECTOR_SIZE);
	blk_queue_max_segment_size(q, SEGMENT_SIZE);
	blk_queue_io_min(q, PXD_LBS);
	blk_queue_io_opt(q, PXD_LBS);
	blk_queue_logical_block_size(q, PXD_LBS);

	set_capacity(disk, add->size / SECTOR_SIZE);

	/* enable the discard support */
	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, q);
	q->limits.discard_granularity = PXD_LBS;
	q->limits.discard_alignment = PXD_LBS;
	q->limits.max_discard_sectors = SEGMENT_SIZE / SECTOR_SIZE;
	q->limits.discard_zeroes_data = 1;

	/* enable flush support */
	blk_queue_flush(q, REQ_FLUSH | REQ_FUA);

	disk->queue = q;

	q->queuedata = pxd_dev;

	pxd_dev->disk = disk;

	return 0;
out_disk:
	put_disk(disk);

	return -ENOMEM;
}

static void pxd_free_disk(struct pxd_device *pxd_dev)
{
	struct gendisk *disk = pxd_dev->disk;

	if (!disk)
		return;

	pxd_dev->disk = NULL;
	if (disk->flags & GENHD_FL_UP) {
		del_gendisk(disk);
		if (disk->queue)
			blk_cleanup_queue(disk->queue);
	}
	put_disk(disk);
}

ssize_t pxd_add(struct fuse_conn *fc, struct pxd_add_out *add)
{
	struct pxd_context *ctx = container_of(fc, struct pxd_context, fc);
	struct pxd_device *pxd_dev;
	struct pxd_device *pxd_dev_itr;
	int new_minor;
	int err;

	err = -ENODEV;
	if (!try_module_get(THIS_MODULE))
		goto out;

	err = -ENOMEM;
	pxd_dev = kzalloc(sizeof(*pxd_dev), GFP_KERNEL);
	if (!pxd_dev)
		goto out_module;

	spin_lock_init(&pxd_dev->lock);

	new_minor = ida_simple_get(&pxd_minor_ida,
				    1, 1 << MINORBITS,
				    GFP_KERNEL);
	if (new_minor < 0) {
		err = new_minor;
		goto out;
	}

	pxd_dev->dev_id = add->dev_id;
	pxd_dev->major = pxd_major;
	pxd_dev->minor = new_minor;
	pxd_dev->ctx = ctx;

	err = pxd_init_disk(pxd_dev, add);
	if (err)
		goto out_id;

	err = pxd_bus_add_dev(pxd_dev);
	if (err)
		goto out_disk;

	spin_lock(&ctx->lock);
	list_for_each_entry(pxd_dev_itr, &ctx->list, node) {
		if (pxd_dev_itr->dev_id == add->dev_id) {
			err = -EEXIST;
			break;
		}
	}
	if (!err) {
		list_add(&pxd_dev->node, &ctx->list);
		++ctx->num_devices;
		spin_unlock(&ctx->lock);
	} else {
		spin_unlock(&ctx->lock);
		goto out_id;
	}

	add_disk(pxd_dev->disk);

	return pxd_dev->minor;

out_disk:
	pxd_free_disk(pxd_dev);
out_id:
	ida_simple_remove(&pxd_minor_ida, new_minor);
out_module:
	module_put(THIS_MODULE);
out:
	return err;
}

static int match_minor(struct fuse_conn *conn, struct fuse_req *req, void *arg)
{
	/* device id is always first field in the argument */
	return req->in.h.opcode != PXD_INIT &&
		*(uint32_t *)req->in.args[0].value == (uint32_t)(uintptr_t)arg;
}

ssize_t pxd_remove(struct fuse_conn *fc, struct pxd_remove_out *remove)
{
	struct pxd_context *ctx = container_of(fc, struct pxd_context, fc);
	int found = false;
	int err;
	struct pxd_device *pxd_dev;
	int minor;

	spin_lock(&ctx->lock);
	list_for_each_entry(pxd_dev, &ctx->list, node) {
		if (pxd_dev->dev_id == remove->dev_id) {
			spin_lock(&pxd_dev->lock);
			if (!pxd_dev->open_count) {
				list_del(&pxd_dev->node);
				--ctx->num_devices;
			}
			found = true;
			break;
		}
	}
	spin_unlock(&ctx->lock);

	if (!found) {
		err = -ENOENT;
		goto out;
	}

	if (pxd_dev->open_count) {
		err = -EBUSY;
		spin_unlock(&pxd_dev->lock);
		goto out;
	}

	pxd_dev->removing = true;

	minor = pxd_dev->minor;

	spin_unlock(&pxd_dev->lock);

	device_unregister(&pxd_dev->dev);
	pxd_free_disk(pxd_dev);

	fuse_end_matching_requests(fc, match_minor, (void *)(uintptr_t)minor);

	ida_simple_remove(&pxd_minor_ida, minor);

	module_put(THIS_MODULE);

	return 0;
out:
	return err;
}

static struct bus_type pxd_bus_type = {
	.name		= "pxd",
};

static void pxd_root_dev_release(struct device *dev)
{
}

static struct device pxd_root_dev = {
	.init_name =    "pxd",
	.release =      pxd_root_dev_release,
};

static struct pxd_device *dev_to_pxd_dev(struct device *dev)
{
	return container_of(dev, struct pxd_device, dev);
}

static ssize_t pxd_size_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);

	return sprintf(buf, "%llu\n",
		(unsigned long long)pxd_dev->size);
}

static ssize_t pxd_major_show(struct device *dev,
		     struct device_attribute *attr, char *buf)
{
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);

	return sprintf(buf, "%llu\n",
			(unsigned long long)pxd_dev->major);
}

static ssize_t pxd_minor_show(struct device *dev,
		     struct device_attribute *attr, char *buf)
{
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);

	return sprintf(buf, "%llu\n",
			(unsigned long long)pxd_dev->minor);
}

static DEVICE_ATTR(size, S_IRUGO, pxd_size_show, NULL);
static DEVICE_ATTR(major, S_IRUGO, pxd_major_show, NULL);
static DEVICE_ATTR(minor, S_IRUGO, pxd_minor_show, NULL);

static struct attribute *pxd_attrs[] = {
	&dev_attr_size.attr,
	&dev_attr_major.attr,
	&dev_attr_minor.attr,
	NULL
};

static struct attribute_group pxd_attr_group = {
	.attrs = pxd_attrs,
};

static const struct attribute_group *pxd_attr_groups[] = {
	&pxd_attr_group,
	NULL
};

static void pxd_sysfs_dev_release(struct device *dev)
{
}

static struct device_type pxd_device_type = {
	.name		= "pxd",
	.groups		= pxd_attr_groups,
	.release	= pxd_sysfs_dev_release,
};

static void pxd_dev_device_release(struct device *dev)
{
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);

	pxd_free_disk(pxd_dev);
}

static int pxd_bus_add_dev(struct pxd_device *pxd_dev)
{
	struct device *dev;
	int ret;

	dev = &pxd_dev->dev;
	dev->bus = &pxd_bus_type;
	dev->type = &pxd_device_type;
	dev->parent = &pxd_root_dev;
	dev->release = pxd_dev_device_release;
	dev_set_name(dev, "%d", pxd_dev->minor);
	ret = device_register(dev);

	return ret;
}

static int pxd_sysfs_init(void)
{
	int err;

	err = device_register(&pxd_root_dev);
	if (err < 0)
		return err;

	err = bus_register(&pxd_bus_type);
	if (err < 0)
		device_unregister(&pxd_root_dev);

	return err;
}

static void pxd_sysfs_exit(void)
{
	bus_unregister(&pxd_bus_type);
	device_unregister(&pxd_root_dev);
}

static void pxd_fill_init_desc(struct fuse_page_desc *desc, int num_ids)
{
	desc->length = num_ids * sizeof(struct pxd_dev_id);
	desc->offset = 0;
}

static void pxd_fill_init(struct fuse_conn *fc, struct fuse_req *req,
	struct pxd_init_in *in)
{
	struct pxd_context *ctx = container_of(fc, struct pxd_context, fc);
	int i = 0, j = 0;
	int num_per_page = PAGE_SIZE / sizeof(struct pxd_dev_id);
	struct pxd_device *pxd_dev;
	struct pxd_dev_id *ids = NULL;

	in->version = PXD_VERSION;

	if (!req->num_pages)
		return;

	spin_lock(&ctx->lock);
	list_for_each_entry(pxd_dev, &ctx->list, node) {
		if (i == 0)
			ids = kmap_atomic(req->pages[j]);
		ids[i].dev_id = pxd_dev->dev_id;
		ids[i].local_minor = pxd_dev->minor;
		++i;
		if (i == num_per_page) {
			pxd_fill_init_desc(&req->page_descs[j], i);
			kunmap_atomic(ids);
			++j;
		}
	}
	in->num_devices = ctx->num_devices;
	spin_unlock(&ctx->lock);

	if (i < num_per_page) {
		pxd_fill_init_desc(&req->page_descs[j], i);
		kunmap_atomic(ids);
	}
}

static void pxd_process_init_reply(struct fuse_conn *fc,
		struct fuse_req *req)
{
	pxd_printk("%s: req %p err %d len %d un %lld\n", __func__, req, req->out.h.error,
		req->out.h.len, req->out.h.unique);

	BUG_ON(fc->pend_open != 1);
	if (req->out.h.error != 0)
		fc->connected = 0;
	fc->pend_open = 0;
	fuse_put_request(fc, req);
}

static int pxd_send_init(struct fuse_conn *fc)
{
	struct pxd_context *ctx = container_of(fc, struct pxd_context, fc);
	int rc;
	struct fuse_req *req;
	struct pxd_init_in *arg;
	void *outarg;
	int i;
	int num_pages = (sizeof(struct pxd_dev_id) * ctx->num_devices +
				PAGE_SIZE - 1) / PAGE_SIZE;

	req = fuse_get_req(fc, num_pages);
	if (IS_ERR(req)) {
		rc = PTR_ERR(req);
		printk(KERN_ERR "%s: get req error %d\n", __func__, rc);
		goto err;
	}

	req->num_pages = num_pages;

	rc = -ENOMEM;
	for (i = 0; i < req->num_pages; ++i) {
		req->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!req->pages[i])
			goto err_free_pages;
	}

	arg = &req->misc.pxd_init_in;
	pxd_fill_init(fc, req, arg);

	outarg = kzalloc(sizeof(struct pxd_init_out), GFP_KERNEL);
	if (!outarg)
		goto err_free_pages;

	req->in.h.opcode = PXD_INIT;
	req->in.numargs = 2;
	req->in.args[0].size = sizeof(struct pxd_init_in);
	req->in.args[0].value = arg;
	req->in.args[1].size = sizeof(struct pxd_dev_id) * ctx->num_devices;
	req->in.args[1].value = NULL;
	req->in.argpages = 1;
	req->out.numargs = 0;
	req->end = pxd_process_init_reply;

	fuse_request_send_oob(fc, req);

	pxd_printk("%s: version %d num devices %ld(%d)\n", __func__, arg->version,
		ctx->num_devices, arg->num_devices);
	return 0;

err_free_pages:
	printk(KERN_ERR "%s: mem alloc\n", __func__);
	for (i = 0; i < req->num_pages; ++i) {
		if (req->pages[i])
			put_page(req->pages[i]);
	}
	fuse_put_request(fc, req);
err:
	return rc;
}

static int pxd_control_open(struct inode *inode, struct file *file)
{
	int rc;
	struct pxd_context *ctx;
	struct fuse_conn *fc;

	if (!((uintptr_t)pxd_contexts <= (uintptr_t)file->f_op &&
		(uintptr_t)file->f_op < (uintptr_t)(pxd_contexts + pxd_num_contexts))) {
		printk(KERN_ERR "%s: invalid fops struct\n", __func__);
		return -EINVAL;
	}

	ctx = container_of(file->f_op, struct pxd_context, fops);
	fc = &ctx->fc;
	if (fc->pend_open == 1) {
		printk(KERN_ERR "%s: too many outstanding opened\n", __func__);
		return -EINVAL;
	}
	fc->pend_open = 1;
	fc->connected = 1;
	fc->initialized = 1;
	file->private_data = fc;

	fuse_restart_requests(fc);

	rc = pxd_send_init(fc);
	if (rc)
		return rc;

	printk(KERN_INFO "%s: open OK\n", __func__);
	return 0;
}

/** note that "kill -9" and "systemctl stop px" won't call this */
static int pxd_control_release(struct inode *inode, struct file *file)
{
	struct pxd_context *ctx;
	ctx = container_of(file->f_op, struct pxd_context, fops);
	if (ctx->fc.connected == 0)
		pxd_printk("%s: not opened\n", __func__);
	else
		ctx->fc.connected = 0;
	return 0;
}

static struct miscdevice pxd_miscdev = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= "pxd-control",
};

MODULE_ALIAS("devname:pxd-control");

static void pxd_fuse_conn_release(struct fuse_conn *conn)
{
}

void pxd_context_init(struct pxd_context *ctx, int i)
{
	spin_lock_init(&ctx->lock);
	fuse_conn_init(&ctx->fc);
	ctx->id = i;
	ctx->fc.release = pxd_fuse_conn_release;
	ctx->fops = fuse_dev_operations;
	ctx->fops.owner = THIS_MODULE;
	ctx->fops.open = pxd_control_open;
	ctx->fops.release = pxd_control_release;
	INIT_LIST_HEAD(&ctx->list);
	sprintf(ctx->name, "pxd-control-%d", i);
	ctx->miscdev.minor = MISC_DYNAMIC_MINOR;
	ctx->miscdev.name = ctx->name;
	ctx->miscdev.fops = &ctx->fops;
	INIT_LIST_HEAD(&ctx->pending_requests);
}

int pxd_init(void)
{
	int err, i;

	err = fuse_dev_init();
	if (err)
		goto out;

	pxd_contexts = kzalloc(sizeof(pxd_contexts[0]) * pxd_num_contexts,
		GFP_KERNEL);
	err = -ENOMEM;
	if (!pxd_contexts)
		goto out_fuse_dev;

	for (i = 0; i < pxd_num_contexts; ++i) {
		struct pxd_context *ctx = &pxd_contexts[i];
		pxd_context_init(ctx, i);
		err = misc_register(&ctx->miscdev);
		if (err) {
			printk(KERN_ERR "%s: failed to register dev %s %d: %d\n", __func__,
				ctx->miscdev.name, i, -err);
			goto out_fuse;
		}
	}

	pxd_miscdev.fops = &pxd_contexts[0].fops;
	err = misc_register(&pxd_miscdev);
	if (err)
		goto out_fuse;

	pxd_major = register_blkdev(0, "pxd");
	if (pxd_major < 0) {
		err = pxd_major;
		goto out_misc;
	}

	err = pxd_sysfs_init();
	if (err)
		goto out_blkdev;

	printk(KERN_INFO "pxd driver loaded\n");

	return 0;

out_blkdev:
	unregister_blkdev(0, "pxd");
out_misc:
	misc_deregister(&pxd_miscdev);
out_fuse:
	for (i = 0; i < pxd_num_contexts; ++i)
		fuse_conn_put(&pxd_contexts[i].fc);
out_fuse_dev:
	fuse_dev_cleanup();
out:
	return err;
}

void pxd_exit(void)
{
	int i;

	pxd_sysfs_exit();
	unregister_blkdev(pxd_major, "pxd");
	misc_deregister(&pxd_miscdev);

	for (i = 0; i < pxd_num_contexts; ++i) {
		misc_deregister(&pxd_contexts[i].miscdev);
		fuse_abort_conn(&pxd_contexts[i].fc);
		fuse_conn_put(&pxd_contexts[i].fc);
	}

	fuse_dev_cleanup();

	kfree(pxd_contexts);

	printk(KERN_INFO "pxd driver unloaded\n");
}

module_init(pxd_init);
module_exit(pxd_exit);

MODULE_LICENSE("GPL");

