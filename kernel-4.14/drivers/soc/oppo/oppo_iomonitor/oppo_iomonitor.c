#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/kernel_stat.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/irqnr.h>
#include <linux/tick.h>
#include <linux/mm.h>
#include <linux/workqueue.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/list.h>
#include <linux/fcntl.h>
#include <linux/syscalls.h>
#include <linux/delay.h>
#include <linux/rtc.h>
#include <soc/oppo/oppo_iomonitor.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/ktime.h>
#include <linux/rtmutex.h>
#include <linux/profile.h>
#include <linux/vmstat.h>
#include <linux/magic.h>
#include "../../../../fs/mount.h"


#define TMP_BUF_LEN 64
#define LOG_FILE_SIZE (5 * 1024 * 1024)
#define ABNORMAL_LOG_NAME "/data/oppo_log/io_abnormal.log"
#define ENTRY_END_TAG  "-------STR end-----\n"

#define DATE_STR_LEN 128
#define IOM_REASON_LENGTH 32

extern unsigned dump_log(char *buf, unsigned len);

static LIST_HEAD(daily_list);
static LIST_HEAD(abnormal_list);
static LIST_HEAD(flow_list);

#define MAX_IOMEVENT_PARAM 3
static struct kobject *iomonitor_kobj = NULL;
static char *io_monitor_env[MAX_IOMEVENT_PARAM] = { NULL };
char iom_reason[IOM_REASON_LENGTH];

unsigned int io_abl_interval_s = 60 * 1000; /* secs */

static unsigned long last_abnormal_jiffies;
static atomic_t abnormal_handle_ref;

struct proc_dir_entry *IoMonitor_dir = NULL;
struct fs_status_info fs_status;
struct abnormal_info io_abnormal_info = {0,0};

static void io_abnormal(struct work_struct *work);
static DECLARE_WAIT_QUEUE_HEAD(io_abnormal_wait);
static struct workqueue_struct *io_abnormal_queue = NULL;
static DECLARE_DELAYED_WORK(io_abnormal_work, io_abnormal);

static DEFINE_PER_CPU(struct cal_data_info, rw_data_info) = {0,0,0,0,0};
static DEFINE_PER_CPU(struct cal_page_info, pgpg_info) = {0,0};
static struct disk_info disk_status;
static int task_status_init(void);
static struct task_io_info *submit_taskio = NULL;
static struct task_io_info *submit_taskio_bak = NULL;
static struct task_io_info *flow_taskio_bak = NULL;

static char * freq_buffer = NULL;
static char * flow_freq_buffer = NULL;

static int block_delay_range[][10] = {
	/* unit of measurement is 'ms' */
	[IO_MONITOR_DELAY_SYNC] = {0, 1, 2, 4, 8, 16, 32, 64, 128, 256},
	[IO_MONITOR_DELAY_NSYNC] = {0, 1, 2, 4, 8, 16, 32, 64, 128, 256},
};

static int device_delay_range[][5] = {
	/* unit of measurement is 'ms' */
	[IO_MONITOR_DELAY_4K] = {0, 1, 10, 50, 100},
	[IO_MONITOR_DELAY_512K] = {1, 10, 50, 100, 500},
};

struct req_delay_data req_para;

/*
* descripte one item
* @name: string for this entry
* @type: see above
* @get_value: call back used to get this entry's value
* @list: list to abnormal_list or daily_list
*
* get_value in case of type == NUM_ENTRY_TYPE
* return  entry's value;  -1 means failed,
* @data is ignored
*
* get_value in case of type == STR_ENTRY_TYPE
* return value represent length of @data ; -1 means get_value failed
* @data point to entry buf
*
* get_value in case of type == STR_FREE_ENTRY_TYPE
* return value represent length of @data ; -1  means get_value failed
* @data point to entry buf
* need free @data after exec get_value
*/
struct io_info_entry {
	char *name;
	int type;
	int (*get_value)(char **data);
	struct list_head list;
};

/*struct seq_operations iomonitor_op = {
	.start = io_monitor_start,
	.next = io_monitor_next,
	.stop = io_monitor_stop,
	.show = io_monitor_show

};*/

#ifdef EXAMPLE_TEST
/* =======================example================= */
static int demo_get_free_mem(char **data)
{
	int free_mem = 5;
	return free_mem;
}

static int demo_get_dump_stack(char **data)
{
	char *dump_buff;
	char *str_value = "this is demo for string type\n";

	dump_buff = kzalloc(128, GFP_KERNEL);
	strcpy(dump_buff,str_value);
	*data = dump_buff;
	return strlen(dump_buff);
}

struct io_info_entry demo_free_mem_entry = {
	.name = "demo_free_mem" ,
	.type = NUM_ENTRY_TYPE,
	.get_value = demo_get_free_mem,
};
struct io_info_entry demo_dump_stack_entry = {
	.name = "demo_dump_stack" ,
	.type = STR_FREE_ENTRY_TYPE,
	.get_value = demo_get_dump_stack,
};

/* =======================example end============================ */
#endif

static int abnormal_interval_seq_show(struct seq_file *seq, void *p)
{
	seq_printf(seq, "%d\n", (io_abl_interval_s/1000));
	return 0;
}

static ssize_t abnormal_interval_write(struct file *filp, const char __user *ubuf,
	size_t cnt, loff_t *ppos)
{
	char buf[8] = { 0 };
	ssize_t buf_size;
	unsigned int v;
	int ret;


	if (!cnt) {
		return 0;
	}

	buf_size = min(cnt, (size_t)(sizeof(buf)-1));
	if (copy_from_user(buf, ubuf, buf_size)) {
			return -EFAULT;
	}
	buf[buf_size-1] = '\0';

	ret = kstrtouint(buf, 10, &v);
	if (ret)
		return ret;

	io_abl_interval_s = v*1000;

	printk("my debug %s %d:int buf=%d\n", __func__, __LINE__, io_abl_interval_s);

	return cnt;
}

static int abnormal_interval_open(struct inode *inode, struct file *file)
{
	return single_open(file, abnormal_interval_seq_show, PDE_DATA(inode));
}

static const struct file_operations seq_abnormal_interval_fops = {
	.open = abnormal_interval_open,
	.read = seq_read,
	.write = abnormal_interval_write,
	.llseek = seq_lseek,
	.release = single_release,
};


static inline bool is_data_fs(struct file *file){
	struct super_block *sb;
	sb = file->f_inode->i_sb;
	if(!sb)
		return false;
	if ((!strncmp(sb->s_type->name, "f2fs", 4))||(!strncmp(sb->s_type->name, "ext4", 4)))
		return true;
	return false;
}

void put_rw_bytes(enum daily_data_type type, struct file *file, ssize_t ret)
{
	switch (type){
		case USER_READ:
			if(is_data_fs(file))
				this_cpu_add(rw_data_info.user_read_data, ret);
			break;
		case USER_WRITE:
			if(is_data_fs(file))
				this_cpu_add(rw_data_info.user_write_data, ret);
			break;
		case KERNEL_READ:
			if(is_data_fs(file))
				this_cpu_add(rw_data_info.kernel_read_data, ret);
			break;
		case KERNEL_WRITE:
			if(is_data_fs(file))
				this_cpu_add(rw_data_info.kernel_write_data, ret);
			break;
		case DIO_WRITE:
			this_cpu_add(rw_data_info.dio_write_data, ret);
			break;
	}
}
EXPORT_SYMBOL(put_rw_bytes);

void pgpg_put_value(enum vm_event_item type, u64 delta)
{
	if(type == PGPGIN)
		this_cpu_add(pgpg_info.page_in, delta);
	else if(type == PGPGOUT)
		this_cpu_add(pgpg_info.page_out, delta);
}
EXPORT_SYMBOL(pgpg_put_value);

void add_fs_type_count(int type)
{
	if(type == FS_GC_OPT)
		fs_status.gc++;
	if(type == FS_DISCARD_OPT)
		fs_status.discard++;
}
EXPORT_SYMBOL(add_fs_type_count);

static void get_rw_bytes(u64* uread, u64* uwrite, u64*kread,u64* kwrite, u64*dwrite)
{
	int i;
	for_each_possible_cpu(i){
		struct cal_data_info *s_data_info = per_cpu_ptr(&rw_data_info, i);
		*uread += s_data_info->user_read_data;
		*uwrite += s_data_info->user_write_data;
		*kread += s_data_info->kernel_read_data;
		*kwrite += s_data_info->kernel_write_data;
		*dwrite += s_data_info->dio_write_data;

		s_data_info->user_read_data = 0;
		s_data_info->user_write_data = 0;
		s_data_info->kernel_read_data = 0;
		s_data_info->kernel_write_data = 0;
		s_data_info->dio_write_data = 0;
	}
}
static int fs_status_get_value(char **data)
{
	char *tmp = kzalloc(128, GFP_KERNEL);
	ssize_t len;
	u64 major_fpage = atomic64_read(&fs_status.major_fpage);
	u64 nfsync = atomic64_read(&fs_status.nfsync);
	u64 dirty_page = atomic64_read(&fs_status.dirty_page);

	len = snprintf(tmp, 128, "%llu %llu %llu %llu %llu %llu %llu %llu\n",
		major_fpage, nfsync,fs_status.discard,fs_status.gc, fs_status.task_rw_data, fs_status.high_iowait,fs_status.low_iowait, dirty_page);
	*data = tmp;
	return len;
}

struct io_info_entry fs_status_entry = {
	 .name = "fs_status" ,
	 .type = STR_FREE_ENTRY_TYPE,
	 .get_value = fs_status_get_value,
	 .list = LIST_HEAD_INIT(fs_status_entry.list),
 };

extern int write_log_kernel(char **data);
struct io_info_entry io_trace_entry = {
    .name = "io_trace" ,
    .type = PRIVATE_ENTRY_TYPE,
    .get_value = write_log_kernel,
};

static int pgpg_status_get_value(char **data)
{
	char *tmp = kzalloc(128, GFP_KERNEL);
	ssize_t len;
	int i;
	unsigned long pgpgin = 0,pgpgout = 0;
	u64 uread = 0,uwrite = 0,kread = 0,kwrite = 0,dwrite = 0;

	for_each_possible_cpu(i){
		struct cal_page_info *s_page_info = per_cpu_ptr(&pgpg_info, i);
		pgpgin += s_page_info->page_in;
		pgpgout += s_page_info->page_out;
		s_page_info->page_in = 0;
		s_page_info->page_out = 0;
	}

	pgpgin /= 2;
	pgpgout /= 2;
	get_rw_bytes(&uread, &uwrite, &kread, &kwrite, &dwrite);
	/*userread, userwrite, kernelread,kernelwrite,directwrite,pgpgin,pgpgout*/
	len = snprintf(tmp, 128, "%llu %llu %llu %llu %llu %lu %lu\n",uread,uwrite,
				kread,kwrite,dwrite,pgpgin,pgpgout);
	*data = tmp;
	return len;
}

struct io_info_entry pgpg_status_entry = {
	 .name = "pgpg_status" ,
	 .type = STR_FREE_ENTRY_TYPE,
	 .get_value = pgpg_status_get_value,
	 .list = LIST_HEAD_INIT(pgpg_status_entry.list),
 };

/* chenweijian@TECH.Storage.IOMonitor, add for calculate free memory, 2020/02/18 */
static int get_free_mem(char **data)
{
	int free_mem = -1;
	/* Free memory = xxx MB. */
	free_mem = (int)(global_zone_page_state(NR_FREE_PAGES) >> (20 - PAGE_SHIFT));
	return free_mem;
}

struct io_info_entry free_mem_entry = {
	.name = "free_mem",
	.type = NUM_ENTRY_TYPE,
	.get_value = get_free_mem
};

static int f2fs_get_free_disk(struct f2fs_sb_info *sbi)
{
	u64 free_size = 0;

	if (!sbi->ckpt || !sbi->sm_info)
		return -1;
	if (!sbi->sm_info->dcc_info)
		return -1;

	free_size = (sbi->user_block_count - sbi->total_valid_block_count);
	free_size >>= (20 - (int)sbi->log_blocksize);
	return ((int)free_size < 0 ? 0 : (int)free_size);
}

static void __dump_disk_info(struct inter_disk_data *disk_data)
{
	int ret = 0;
	char *disk_buf;
	int i;
	char *p;

	disk_data->len = -1;

	disk_buf = kzalloc(512, GFP_KERNEL);
	if (!disk_buf)
		return;

	p = disk_buf;
	ret = sprintf(p, "%d ", disk_status.score);
	p = p + ret;
	disk_data->len += ret;

	ret = sprintf(p, "%d ", disk_status.free);
	p = p + ret;
	disk_data->len += ret;

	for (i = 0; i < ARRAY_SIZE(disk_status.blocks); i++) {
		if (!disk_status.blocks[i]){
			continue;
		}
		if (i == ARRAY_SIZE(disk_status.blocks) - 1) {
			ret = sprintf(p, "%d\n", disk_status.blocks[i]);
			p = p + ret;
			disk_data->len += ret;
		}
		else{
			ret = sprintf(p, "%d ", disk_status.blocks[i]);
			p = p + ret;
			disk_data->len += ret;
		}
	}

	disk_data->buf= disk_buf;

	return;
}

static void __get_disk_info(struct super_block *sb, void *arg)
{
	unsigned int i;
	bool valid_patition = false;
	struct mount *mnt;
	struct f2fs_sb_info *sbi;
	unsigned int total_segs;
	block_t total_blocks = 0;
	struct inter_disk_data * disk_data = (struct inter_disk_data *)arg;

	if (disk_data->len != -1) {
		return;
	}

	list_for_each_entry(mnt, &sb->s_mounts, mnt_instance) {
		if (strstr (mnt->mnt_devname, "userdata") && strstr(mnt->mnt_mp->m_dentry->d_name.name, "data")) {
			if (sb->s_magic == F2FS_SUPER_MAGIC) {
				valid_patition = true;
				break;
			}
		}
	}

	if (valid_patition) {
		sbi = F2FS_SB(sb);
		total_segs = le32_to_cpu(sbi->raw_super->segment_count_main);
		memset(&disk_status, 0, sizeof(struct disk_info));
		for (i = 0; i < total_segs; i++) {
			total_blocks += of2fs_seg_freefrag(sbi, i,
					disk_status.blocks, ARRAY_SIZE(disk_status.blocks));
			cond_resched();
		}
		disk_status.score = total_blocks ? (disk_status.blocks[0] + disk_status.blocks[1]) * 100ULL / total_blocks : 0;
		disk_status.free = f2fs_get_free_disk(sbi);

		__dump_disk_info(disk_data);

	}

	return;
}
static int get_disk_info(char **data)
{
	struct inter_disk_data disk_data;

	disk_data.len = -1;
	disk_data.buf= NULL;

	iterate_supers(__get_disk_info, &disk_data);

	*data = disk_data.buf;
	return disk_data.len;
}

struct io_info_entry disk_info_entry = {
	 .name = "disk" ,
	 .type = STR_FREE_ENTRY_TYPE,
	 .get_value = get_disk_info,
 };

/*static int get_io_history(char **data)
{
	char *p, *buf;
	int i, ret, len;

	buf = kzalloc(2048, GFP_KERNEL);
	if (!buf) {
		return -1;
	}
	p = buf;

	strcpy(p, "iotype:");
	len = strlen("iotype:");
	p += len;
	for (i=0; i<IO_HISTORY_DEPTH; i++) {
		ret = sprintf(p, "%u ", io_queue_history[i].cmd_flags);
		p += ret;
		len += ret;
	}

	strcpy(p, "\niojiffies:");
	ret = strlen("\niojiffies:");
	p += ret;
	len += ret;
	for (i=0; i<IO_HISTORY_DEPTH; i++) {
		ret = sprintf(p, "%lu ", io_queue_history[i].jiffies);
		p += ret;
		len += ret;
	}

	*p = '\n';
	len += 1;

	*data = buf;

	return len;
}

struct io_info_entry io_queue_history_entry = {
	.name = "io history",
	.type = STR_FREE_ENTRY_TYPE,
	.get_value = get_io_history
};*/

/* chenweijian@TECH.Storage.IOMonitor, add for statistical IO time-consuming distribution, 2020/02/18 */
static inline bool rq_is_ux(struct request *rq)
{
	return (rq->cmd_flags & REQ_UX);
}

void reqstats_init()
{
	memset(&req_para, 0, sizeof(struct req_delay_data));
	return;
}

void reqstats_record(struct request *req, unsigned int nr_bytes, int fg)
{
	u64 tg2d = 0, td2c = 0;
	int index, rw, sync, isux;
	if (!req)
		return;

	/* BLOCK */
	tg2d = ktime_us_delta(req->req_td, req->req_tg) / 1000;
	sync = rq_is_sync(req) ? 0 : 1;
	index = sync;
	isux = rq_is_ux(req);
	/* UX req */
	if (!sync && isux) {
		req_para.uxreq_block_para.cnt++;
		req_para.uxreq_block_para.total_delay += tg2d;
		if (tg2d >= block_delay_range[index][9])
			req_para.uxreq_block_para.stage_ten++;
		else if (tg2d >= block_delay_range[index][8])
			req_para.uxreq_block_para.stage_nin++;
		else if (tg2d >= block_delay_range[index][7])
			req_para.uxreq_block_para.stage_eig++;
		else if (tg2d >= block_delay_range[index][6])
			req_para.uxreq_block_para.stage_sev++;
		else if (tg2d >= block_delay_range[index][5])
			req_para.uxreq_block_para.stage_six++;
		else if (tg2d >= block_delay_range[index][4])
			req_para.uxreq_block_para.stage_fiv++;
		else if (tg2d >= block_delay_range[index][3])
			req_para.uxreq_block_para.stage_fou++;
		else if (tg2d >= block_delay_range[index][2])
			req_para.uxreq_block_para.stage_thr++;
		else if (tg2d >= block_delay_range[index][1])
			req_para.uxreq_block_para.stage_two++;
		else if (tg2d >= block_delay_range[index][0])
			req_para.uxreq_block_para.stage_one++;
		req_para.uxreq_block_para.max_delay = (req_para.uxreq_block_para.max_delay >= tg2d) ? req_para.uxreq_block_para.max_delay : tg2d;
	}
	/* ALL req */
	req_para.req_block_para.cnt[sync]++;
	req_para.req_block_para.total_delay[sync] += tg2d;
	if (tg2d >= block_delay_range[index][9])
		req_para.req_block_para.stage_ten[sync]++;
	else if (tg2d >= block_delay_range[index][8])
		req_para.req_block_para.stage_nin[sync]++;
	else if (tg2d >= block_delay_range[index][7])
		req_para.req_block_para.stage_eig[sync]++;
	else if (tg2d >= block_delay_range[index][6])
		req_para.req_block_para.stage_sev[sync]++;
	else if (tg2d >= block_delay_range[index][5])
		req_para.req_block_para.stage_six[sync]++;
	else if (tg2d >= block_delay_range[index][4])
		req_para.req_block_para.stage_fiv[sync]++;
	else if (tg2d >= block_delay_range[index][3])
		req_para.req_block_para.stage_fou[sync]++;
	else if (tg2d >= block_delay_range[index][2])
		req_para.req_block_para.stage_thr[sync]++;
	else if (tg2d >= block_delay_range[index][1])
		req_para.req_block_para.stage_two[sync]++;
	else if (tg2d >= block_delay_range[index][0])
		req_para.req_block_para.stage_one[sync]++;
	req_para.req_block_para.max_delay[sync] = (req_para.req_block_para.max_delay[sync] >= tg2d) ? req_para.req_block_para.max_delay[sync] : tg2d;

	/* DEVICE */
	td2c = ktime_us_delta(req->req_tc, req->req_td) / 1000;
	if (nr_bytes == 4 * 1024)
		index = IO_MONITOR_DELAY_4K;
	else if (nr_bytes == 512 * 1024)
		index = IO_MONITOR_DELAY_512K;
	else {
		index = IO_MONITOR_DELAY_DEVICE_NUM;
		return;
	}
	/* size = 512K && time < 1ms, do not record */
	if (td2c < 1 && index == IO_MONITOR_DELAY_512K)
		return;
	rw = rq_data_dir(req);
	req_para.req_device_para[index].cnt[rw]++;
	req_para.req_device_para[index].total_delay[rw] += td2c;
	if (td2c >= device_delay_range[index][4])
		req_para.req_device_para[index].stage_fiv[rw]++;
	else if (td2c >= device_delay_range[index][3])
		req_para.req_device_para[index].stage_fou[rw]++;
	else if (td2c >= device_delay_range[index][2])
		req_para.req_device_para[index].stage_thr[rw]++;
	else if (td2c >= device_delay_range[index][1])
		req_para.req_device_para[index].stage_two[rw]++;
	else if (td2c >= device_delay_range[index][0])
		req_para.req_device_para[index].stage_one[rw]++;
	req_para.req_device_para[index].max_delay[rw] = (req_para.req_device_para[index].max_delay[rw] >= td2c) ? req_para.req_device_para[index].max_delay[rw] : td2c;

	return;
}

static int get_blk_ux_io_distribution(char **data)
{
	int ret = 0;
	char *uxio_buff;
	uxio_buff = kzalloc(1024, GFP_KERNEL);
	if (!uxio_buff)
		return ret;
	ret = sprintf(uxio_buff,
		"%lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld\n",
		req_para.uxreq_block_para.cnt,
		req_para.uxreq_block_para.total_delay,
		req_para.uxreq_block_para.max_delay,
		req_para.uxreq_block_para.stage_one,
		req_para.uxreq_block_para.stage_two,
		req_para.uxreq_block_para.stage_thr,
		req_para.uxreq_block_para.stage_fou,
		req_para.uxreq_block_para.stage_fiv,
		req_para.uxreq_block_para.stage_six,
		req_para.uxreq_block_para.stage_sev,
		req_para.uxreq_block_para.stage_eig,
		req_para.uxreq_block_para.stage_nin,
		req_para.uxreq_block_para.stage_ten);

	*data = uxio_buff;
	return ret;
}


struct io_info_entry block_ux_io_entry = {
	.name = "block_ux_io",
	.type = STR_FREE_ENTRY_TYPE,
	.get_value = get_blk_ux_io_distribution
};

static int get_blk_sync_io_distribution(char **data)
{
	int ret = 0;
	char *syncio_buff;
	syncio_buff = kzalloc(1024, GFP_KERNEL);
	if (!syncio_buff)
		return ret;
	ret = sprintf(syncio_buff,
		"%lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld\n",
		req_para.req_block_para.cnt[IO_MONITOR_DELAY_SYNC],
		req_para.req_block_para.total_delay[IO_MONITOR_DELAY_SYNC],
		req_para.req_block_para.max_delay[IO_MONITOR_DELAY_SYNC],
		req_para.req_block_para.stage_one[IO_MONITOR_DELAY_SYNC],
		req_para.req_block_para.stage_two[IO_MONITOR_DELAY_SYNC],
		req_para.req_block_para.stage_thr[IO_MONITOR_DELAY_SYNC],
		req_para.req_block_para.stage_fou[IO_MONITOR_DELAY_SYNC],
		req_para.req_block_para.stage_fiv[IO_MONITOR_DELAY_SYNC],
		req_para.req_block_para.stage_six[IO_MONITOR_DELAY_SYNC],
		req_para.req_block_para.stage_sev[IO_MONITOR_DELAY_SYNC],
		req_para.req_block_para.stage_eig[IO_MONITOR_DELAY_SYNC],
		req_para.req_block_para.stage_nin[IO_MONITOR_DELAY_SYNC],
		req_para.req_block_para.stage_ten[IO_MONITOR_DELAY_SYNC]);

	*data = syncio_buff;
	return ret;
}


struct io_info_entry block_sync_io_entry = {
	.name = "block_sync_io",
	.type = STR_FREE_ENTRY_TYPE,
	.get_value = get_blk_sync_io_distribution
};

static int get_blk_nsync_io_distribution(char **data)
{
	int ret = 0;
	char *nsyncio_buff;
	nsyncio_buff = kzalloc(1024, GFP_KERNEL);
	if (!nsyncio_buff)
		return ret;
	ret = sprintf(nsyncio_buff,
		"%lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld\n",
		req_para.req_block_para.cnt[IO_MONITOR_DELAY_NSYNC],
		req_para.req_block_para.total_delay[IO_MONITOR_DELAY_NSYNC],
		req_para.req_block_para.max_delay[IO_MONITOR_DELAY_NSYNC],
		req_para.req_block_para.stage_one[IO_MONITOR_DELAY_NSYNC],
		req_para.req_block_para.stage_two[IO_MONITOR_DELAY_NSYNC],
		req_para.req_block_para.stage_thr[IO_MONITOR_DELAY_NSYNC],
		req_para.req_block_para.stage_fou[IO_MONITOR_DELAY_NSYNC],
		req_para.req_block_para.stage_fiv[IO_MONITOR_DELAY_NSYNC],
		req_para.req_block_para.stage_six[IO_MONITOR_DELAY_NSYNC],
		req_para.req_block_para.stage_sev[IO_MONITOR_DELAY_NSYNC],
		req_para.req_block_para.stage_eig[IO_MONITOR_DELAY_NSYNC],
		req_para.req_block_para.stage_nin[IO_MONITOR_DELAY_NSYNC],
		req_para.req_block_para.stage_ten[IO_MONITOR_DELAY_NSYNC]);

	*data = nsyncio_buff;
	return ret;
}

struct io_info_entry block_nsync_io_entry = {
	.name = "block_nsync_io",
	.type = STR_FREE_ENTRY_TYPE,
	.get_value = get_blk_nsync_io_distribution
};

static int get_dev_4k_rw_distribution(char **data)
{
	int ret = 0;
	char *rw4k_buff;
	rw4k_buff = kzalloc(1024, GFP_KERNEL);
	if (!rw4k_buff)
		return ret;
	ret = sprintf(rw4k_buff,
		"%lld %lld %lld %lld %lld %lld %lld %lld "
		"%lld %lld %lld %lld %lld %lld %lld %lld\n",
		req_para.req_device_para[IO_MONITOR_DELAY_4K].cnt[IO_MONITOR_DELAY_READ],
		req_para.req_device_para[IO_MONITOR_DELAY_4K].total_delay[IO_MONITOR_DELAY_READ],
		req_para.req_device_para[IO_MONITOR_DELAY_4K].max_delay[IO_MONITOR_DELAY_READ],
		req_para.req_device_para[IO_MONITOR_DELAY_4K].stage_one[IO_MONITOR_DELAY_READ],
		req_para.req_device_para[IO_MONITOR_DELAY_4K].stage_two[IO_MONITOR_DELAY_READ],
		req_para.req_device_para[IO_MONITOR_DELAY_4K].stage_thr[IO_MONITOR_DELAY_READ],
		req_para.req_device_para[IO_MONITOR_DELAY_4K].stage_fou[IO_MONITOR_DELAY_READ],
		req_para.req_device_para[IO_MONITOR_DELAY_4K].stage_fiv[IO_MONITOR_DELAY_READ],
		req_para.req_device_para[IO_MONITOR_DELAY_4K].cnt[IO_MONITOR_DELAY_WRITE],
		req_para.req_device_para[IO_MONITOR_DELAY_4K].total_delay[IO_MONITOR_DELAY_WRITE],
		req_para.req_device_para[IO_MONITOR_DELAY_4K].max_delay[IO_MONITOR_DELAY_WRITE],
		req_para.req_device_para[IO_MONITOR_DELAY_4K].stage_one[IO_MONITOR_DELAY_WRITE],
		req_para.req_device_para[IO_MONITOR_DELAY_4K].stage_two[IO_MONITOR_DELAY_WRITE],
		req_para.req_device_para[IO_MONITOR_DELAY_4K].stage_thr[IO_MONITOR_DELAY_WRITE],
		req_para.req_device_para[IO_MONITOR_DELAY_4K].stage_fou[IO_MONITOR_DELAY_WRITE],
		req_para.req_device_para[IO_MONITOR_DELAY_4K].stage_fiv[IO_MONITOR_DELAY_WRITE]);

	*data = rw4k_buff;
	return ret;
}

struct io_info_entry device_4k_rw_entry = {
	.name = "device_4k_rw",
	.type = STR_FREE_ENTRY_TYPE,
	.get_value = get_dev_4k_rw_distribution
};

static int get_dev_512k_rw_distribution(char **data)
{
	int ret = 0;
	char *rw512k_buff;
	rw512k_buff = kzalloc(1024, GFP_KERNEL);
	if (!rw512k_buff)
		return ret;
	ret = sprintf(rw512k_buff,
		"%lld %lld %lld %lld %lld %lld %lld %lld "
		"%lld %lld %lld %lld %lld %lld %lld %lld\n",
		req_para.req_device_para[IO_MONITOR_DELAY_512K].cnt[IO_MONITOR_DELAY_READ],
		req_para.req_device_para[IO_MONITOR_DELAY_512K].total_delay[IO_MONITOR_DELAY_READ],
		req_para.req_device_para[IO_MONITOR_DELAY_512K].max_delay[IO_MONITOR_DELAY_READ],
		req_para.req_device_para[IO_MONITOR_DELAY_512K].stage_one[IO_MONITOR_DELAY_READ],
		req_para.req_device_para[IO_MONITOR_DELAY_512K].stage_two[IO_MONITOR_DELAY_READ],
		req_para.req_device_para[IO_MONITOR_DELAY_512K].stage_thr[IO_MONITOR_DELAY_READ],
		req_para.req_device_para[IO_MONITOR_DELAY_512K].stage_fou[IO_MONITOR_DELAY_READ],
		req_para.req_device_para[IO_MONITOR_DELAY_512K].stage_fiv[IO_MONITOR_DELAY_READ],
		req_para.req_device_para[IO_MONITOR_DELAY_512K].cnt[IO_MONITOR_DELAY_WRITE],
		req_para.req_device_para[IO_MONITOR_DELAY_512K].total_delay[IO_MONITOR_DELAY_WRITE],
		req_para.req_device_para[IO_MONITOR_DELAY_512K].max_delay[IO_MONITOR_DELAY_WRITE],
		req_para.req_device_para[IO_MONITOR_DELAY_512K].stage_one[IO_MONITOR_DELAY_WRITE],
		req_para.req_device_para[IO_MONITOR_DELAY_512K].stage_two[IO_MONITOR_DELAY_WRITE],
		req_para.req_device_para[IO_MONITOR_DELAY_512K].stage_thr[IO_MONITOR_DELAY_WRITE],
		req_para.req_device_para[IO_MONITOR_DELAY_512K].stage_fou[IO_MONITOR_DELAY_WRITE],
		req_para.req_device_para[IO_MONITOR_DELAY_512K].stage_fiv[IO_MONITOR_DELAY_WRITE]);

	*data = rw512k_buff;
	return ret;
}

struct io_info_entry device_512k_rw_entry = {
	.name = "device_512k_rw",
	.type = STR_FREE_ENTRY_TYPE,
	.get_value = get_dev_512k_rw_distribution
};

static int get_dump_log(char **data)
{
	int ret = 0;
	char *dmesg_buff;
	dmesg_buff = kzalloc(4096, GFP_KERNEL);
	if (!dmesg_buff)
		return ret;
	ret = dump_log(dmesg_buff, 4096);
	*data = dmesg_buff;
	return ret;
}

struct io_info_entry ligntweight_dmesg_entry = {
	.name = "dmesg",
	.type = STR_FREE_ENTRY_TYPE,
	.get_value = get_dump_log
};


static int exit_task_notifier(struct notifier_block *self,
			unsigned long cmd, void *v)
{
	struct task_struct *task = v;

	if (!task->ioac.read_bytes && !task->ioac.write_bytes)
		return NOTIFY_OK;

	exit_uid_find_or_register(task);
	return NOTIFY_OK;
}

static struct notifier_block exit_task_notifier_block = {
	.notifier_call	= exit_task_notifier,
};

static u64 task_timestamp;
void add_pid_to_list(struct task_struct *task, size_t bytes, bool opt)
{
	int i;
	struct task_io_info *taskio = NULL;
	int pos = 0;
	for(i=0; i < PID_LENGTH; i++){
		taskio = submit_taskio + i;
		if(taskio->pid == task->pid){
			taskio->state = task->state;
			taskio->all_read_bytes = task->ioac.read_bytes;
			taskio->all_write_bytes = task->ioac.write_bytes;
			taskio->fsync = task->ioac.syscfs;
			if (opt)
				taskio->write_bytes += bytes;
			else
				taskio->read_bytes += bytes;
			if (strncmp(taskio->comm, task->comm, strlen(task->comm)))
				strncpy(taskio->comm, task->comm, TASK_COMM_LEN);
			taskio->time = jiffies;
			return;
		}

		if(taskio->time <= task_timestamp){
			task_timestamp = taskio->time;
			pos = i;
		}
	}
	taskio = submit_taskio + pos;
	taskio->pid = task->pid;
	taskio->tgid = task->tgid;
	taskio->uid = from_kuid_munged(current_user_ns(), task_uid(task));
	taskio->state = task->state;
	taskio->all_read_bytes = task->ioac.read_bytes;
	taskio->all_write_bytes = task->ioac.write_bytes;
	taskio->fsync = task->ioac.syscfs;
	taskio->time = jiffies;
	if (opt)
		taskio->write_bytes = bytes;
	else
		taskio->read_bytes = bytes;
	strncpy(taskio->comm, task->comm, TASK_COMM_LEN);
	task_timestamp = taskio->time;
}

static int task_status_get_value(char **data)
{
	int len = 0,i;
	struct task_io_info *taskio = NULL;
	memset(flow_freq_buffer, 0, TASK_STATUS_LENGTH);
	memset(flow_taskio_bak, 0, sizeof(struct task_io_info) * PID_LENGTH);
	memcpy(flow_taskio_bak,submit_taskio,sizeof(struct task_io_info) * PID_LENGTH);
	for(i=0; i< PID_LENGTH; i++){
		taskio = flow_taskio_bak + i;
		if(taskio->pid)
			len += snprintf(flow_freq_buffer + len, TASK_STATUS_LENGTH - len, "\n%d %d %d %d %llu %llu %llu",taskio->pid,taskio->tgid,taskio->uid,taskio->state,taskio->all_read_bytes,taskio->all_write_bytes,taskio->fsync);
	}
	*data = flow_freq_buffer;
	return len;
}

struct io_info_entry task_status_entry = {
	 .name = "task_status" ,
	 .type = STR_ENTRY_TYPE,
	 .get_value = task_status_get_value,
	 .list = LIST_HEAD_INIT(task_status_entry.list),
};

static int get_task_status(char **data)
{
	int len = 0,i;
	struct task_io_info *taskio = NULL;
	memset(freq_buffer, 0, TASK_STATUS_LENGTH);
	memset(submit_taskio_bak, 0, sizeof(struct task_io_info) * PID_LENGTH);
	memcpy(submit_taskio_bak,submit_taskio,sizeof(struct task_io_info) * PID_LENGTH);
	for(i=0; i< PID_LENGTH; i++){
		taskio = submit_taskio_bak + i;
		if(taskio->pid)
			len += snprintf(freq_buffer + len, TASK_STATUS_LENGTH - len, "%d %s %d %d %llu %llu %llu\n",taskio->pid,taskio->comm,taskio->tgid,taskio->uid,taskio->read_bytes,taskio->write_bytes,taskio->fsync);
	}
	*data = freq_buffer;
	return len;
}

static inline void add_daily_entry(struct io_info_entry *entry)
{
	list_add_tail(&entry->list, &daily_list);
}

static inline void add_monitor_entry(struct io_info_entry *entry)
{
	list_add_tail(&entry->list, &abnormal_list);
}

static inline void add_flow_entry(struct io_info_entry *entry)
{
	list_add_tail(&entry->list, &flow_list);
}

static int io_monitor_show(struct seq_file *m, void *v)
{
	struct list_head *p;
	struct io_info_entry *entry;
	int ret;
	char *pbuf;

	p = daily_list.next;
	while (p != &daily_list) {
		entry = list_entry(p, struct io_info_entry, list);
		p = p->next;

		if (NUM_ENTRY_TYPE == entry->type) {
			ret = entry->get_value(NULL);
			if (-1 != ret) {
				seq_printf(m, "%s %d\n", entry->name, ret);
			}
		} else if (STR_ENTRY_TYPE == entry->type || STR_FREE_ENTRY_TYPE == entry->type) {
			ret = entry->get_value(&pbuf);
			if ( -1 != ret ) {
				seq_printf(m, "%s %s", entry->name, pbuf);
				if (STR_FREE_ENTRY_TYPE == entry->type) {
					kfree(pbuf);
				}
			}
		}

	}
	memset(&fs_status, 0, sizeof(struct fs_status_info));
	//memset(&disk_status, 0, sizeof(struct disk_info));
	memset(&req_para, 0, sizeof(struct req_delay_data));
	return 0;
}

static int iomonitor_open(struct inode *inode, struct file *file)
{
    return single_open(file, io_monitor_show, inode->i_private);
}

static const struct file_operations proc_dailyio_operations =
{
    .open       = iomonitor_open,
    .read       = seq_read,
    .llseek     = seq_lseek,
    .release    = single_release,
};

static int iomonitor_dataflow_show(struct seq_file *m, void *v)
{
	struct list_head *p;
	struct io_info_entry *entry;
	int ret;
	char *pbuf;

	p = flow_list.next;
	while (p != &flow_list) {
		entry = list_entry(p, struct io_info_entry, list);
		p = p->next;

		if (NUM_ENTRY_TYPE == entry->type) {
			ret = entry->get_value(NULL);
			if (-1 != ret) {
				seq_printf(m, "%s:%d\n", entry->name, ret);
			}
		} else if (STR_ENTRY_TYPE == entry->type || STR_FREE_ENTRY_TYPE == entry->type) {
			ret = entry->get_value(&pbuf);
			if ( -1 != ret ) {
				seq_printf(m, "%s:%s\n", entry->name, pbuf);
			}
			if (STR_FREE_ENTRY_TYPE == entry->type) {
				kfree(pbuf);
			}
		}

	}
	return 0;
}
static int iomonitor_dataflow_open(struct inode *inode, struct file *file)
{
    return single_open(file, iomonitor_dataflow_show, inode->i_private);
}

static ssize_t trigger_abnormal_event(struct file *filp, const char __user *ubuf,
	size_t cnt, loff_t *ppos)
{
	abnormal_handle(USER_TRIGGER,current->pid);

	return cnt;
}

static const struct file_operations proc_dataflow_operations =
{
    .open       = iomonitor_dataflow_open,
    .read       = seq_read,
    .llseek     = seq_lseek,
    .release    = single_release,
};

static int trigger_abnormal_open(struct inode *inode, struct file *file)
{
    return single_open(file, abnormal_interval_seq_show, inode->i_private);
}


static const struct file_operations trigger_abnormal_open_operations =
{
    .open       = trigger_abnormal_open,
    .read       = seq_read,
    .write = trigger_abnormal_event,
    .llseek     = seq_lseek,
    .release    = single_release,
};

static int get_date(char *date)
{
	struct timespec64 ts;
	struct rtc_time tm;
	int len = 0;

	getnstimeofday64(&ts);
	ts.tv_sec -= sys_tz.tz_minuteswest *60;
	rtc_time_to_tm(ts.tv_sec, &tm);

	len += snprintf(date + len, DATE_STR_LEN - len,"%04d-%02d-%02d %02d:%02d:%02d %d %d\n",
		tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec,
		io_abnormal_info.reason,io_abnormal_info.pid);

	memset(&io_abnormal_info, 0, sizeof(struct abnormal_info));
	return len;
}

int write_abnormal_file(struct file *filp, struct io_info_entry *entry)
{
	char name_buf[TMP_BUF_LEN];
	int ret;
	loff_t file_pos = 0;
	char *pbuf = NULL;


	switch(entry->type) {
		case NUM_ENTRY_TYPE:
			ret = entry->get_value(NULL);
			if (-1 != ret) {
				memset(name_buf, 0, TMP_BUF_LEN);
				snprintf(name_buf, TMP_BUF_LEN, "%d\n", ret);
				kernel_write(filp, name_buf, strlen(name_buf), &file_pos);
			}
			break;
		case STR_ENTRY_TYPE:
		case STR_FREE_ENTRY_TYPE:
			ret = entry->get_value(&pbuf);
			if (-1 != ret) {
				memset(name_buf, 0, TMP_BUF_LEN);
				snprintf(name_buf,TMP_BUF_LEN, "%s %d\n", entry->name, ret);
				kernel_write(filp, name_buf, strlen(name_buf), &file_pos);
				kernel_write(filp, pbuf, ret, &file_pos);
				kernel_write(filp, ENTRY_END_TAG, strlen(ENTRY_END_TAG), &file_pos);

				if (STR_FREE_ENTRY_TYPE == entry->type) {
					if (pbuf) {
						kfree(pbuf);
					}
				}
			}
			break;
		case PRIVATE_ENTRY_TYPE:
			entry->get_value((char **)filp);
			break;
	}
	return 0;
}

static void io_abnormal(struct work_struct *work)
{
	struct list_head *p;
	struct file *filp_log;
	loff_t file_pos = 0;
	struct io_info_entry *io_entry;
	char *buffer = NULL;
	char date[DATE_STR_LEN];
	int flags = O_RDWR | O_CREAT | O_APPEND;

	if (time_before(jiffies, last_abnormal_jiffies + msecs_to_jiffies(io_abl_interval_s))) {
		atomic_dec(&abnormal_handle_ref);
		return;
	}

	memset(date, 0, DATE_STR_LEN);
	get_date(date);
	memset(io_monitor_env[0], 0, IOM_REASON_LENGTH);
	memset(io_monitor_env[1], 0, DATE_STR_LEN);
	snprintf(io_monitor_env[0],IOM_REASON_LENGTH, "REASON=%d", io_abnormal_info.reason);
	snprintf(io_monitor_env[1],DATE_STR_LEN, "DATE=%s", date);
	if (iomonitor_kobj) {
		kobject_uevent_env(iomonitor_kobj, KOBJ_CHANGE, io_monitor_env);
	}

	filp_log = filp_open(ABNORMAL_LOG_NAME, flags, 0666);
	if (IS_ERR(filp_log)) {
		atomic_dec(&abnormal_handle_ref);
		return;
	}

	if (filp_log->f_inode->i_size > LOG_FILE_SIZE) {
		atomic_dec(&abnormal_handle_ref);
		filp_close(filp_log, NULL);
		printk("my debug %s %d:wait for rm file\n", __func__, __LINE__);
		return;
	}

	/* write log file header */
	kernel_write(filp_log, date, strlen(date), &file_pos);

	get_task_status(&buffer);
	kernel_write(filp_log, buffer, strlen(buffer), &file_pos);

	/* write log file data */
	p = abnormal_list.next;
	while (p != &abnormal_list) {
		io_entry = list_entry(p, struct io_info_entry, list);
		p = p->next;

		write_abnormal_file(filp_log,io_entry);
	}

	filp_close(filp_log, NULL);
	last_abnormal_jiffies = jiffies;

	atomic_dec(&abnormal_handle_ref);

	return;
}


/*
* should be called at io abnormal point
* might sleep,can't used in atomic-context
*/
int abnormal_handle(enum iomointor_io_type reason, pid_t pid)
{
	if (atomic_inc_return(&abnormal_handle_ref) > 1) {
		atomic_dec(&abnormal_handle_ref);
		return 0;
	}

	//printk_deferred("[iomonitor] %s %d: queue_delayed_work\n", __func__, __LINE__);
	io_abnormal_info.reason = reason;
	io_abnormal_info.pid = pid;
	queue_delayed_work(io_abnormal_queue, &io_abnormal_work, msecs_to_jiffies(1));

	return 0;
}

static void fs_status_init()
{
	memset(&fs_status, 0, sizeof(struct fs_status_info));
}

static int task_status_init(void)
{
	freq_buffer = kzalloc(TASK_STATUS_LENGTH, GFP_KERNEL);
	if(!freq_buffer)
		return -1;
	flow_freq_buffer = kzalloc(TASK_STATUS_LENGTH, GFP_KERNEL);
	if(!flow_freq_buffer)
		return -1;
	submit_taskio = kzalloc(sizeof(struct task_io_info) * PID_LENGTH, GFP_KERNEL);
	if(!submit_taskio)
		return -1;
	submit_taskio_bak = kzalloc(sizeof(struct task_io_info) * PID_LENGTH, GFP_KERNEL);
	if(!submit_taskio_bak)
		return -1;
	flow_taskio_bak = kzalloc(sizeof(struct task_io_info) * PID_LENGTH, GFP_KERNEL);
	if(!flow_taskio_bak)
		return -1;

	profile_event_register(PROFILE_TASK_EXIT, &exit_task_notifier_block);
	return 0;
}

static int task_status_exit(void)
{
	if(freq_buffer )
		kfree(freq_buffer);
	if(flow_freq_buffer )
		kfree(flow_freq_buffer);
	if(submit_taskio)
		kfree(submit_taskio);
	if(submit_taskio_bak)
		kfree(submit_taskio_bak);
	if(flow_taskio_bak)
		kfree(flow_taskio_bak);

	profile_event_unregister(PROFILE_TASK_EXIT, &exit_task_notifier_block);
	return 0;
}

/*
* alloc meme for IoMonitor
* return 0 success,otherwith fail
*/
static int io_monitor_resource_init(void)
{
	fs_status_init();
	task_status_init();
	return 0;
}

static inline void remove_uevent_resourse(void)
{
	int i;

	for (i=0; i < MAX_IOMEVENT_PARAM -1; i++) {
		if (io_monitor_env[i]) {
			kfree(io_monitor_env[i]);
		}
	}

	return;
}

/*
* free meme for IoMonitor
* return 0 success,otherwith fail
*/
static int io_monitor_resource_exit(void)
{
	task_status_exit();
	free_all_uid_entry();
   return 0;
}

static void add_monitor_items(void)
{
#ifdef EXAMPLE_TEST
	add_monitor_entry(&demo_dump_stack_entry); /* example */
#endif
	add_monitor_entry(&free_mem_entry);
	//add_monitor_entry(&io_queue_history_entry);
	add_monitor_entry(&io_trace_entry);
	add_monitor_entry(&ligntweight_dmesg_entry);
}

static void add_daily_items(void)
{
#ifdef EXAMPLE_TEST
	add_daily_entry(&demo_free_mem_entry); /* example */
#endif
	add_daily_entry(&fs_status_entry);
	add_daily_entry(&block_ux_io_entry);
	add_daily_entry(&block_sync_io_entry);
	add_daily_entry(&block_nsync_io_entry);
	add_daily_entry(&device_4k_rw_entry);
	add_daily_entry(&device_512k_rw_entry);
	add_daily_entry(&pgpg_status_entry);
	add_daily_entry(&disk_info_entry);
}

static void add_flow_items(void)
{
	add_flow_entry(&task_status_entry);
}


static int iomonitor_uevent_init(void)
{
	int i;
	io_monitor_env[0] = &iom_reason[0];
	io_monitor_env[1] = kzalloc(DATE_STR_LEN, GFP_KERNEL);
	if (!io_monitor_env[1]) {
		printk("io_monitor:kzalloc io monitor uevent param failed\n");
		goto err;
	}

	iomonitor_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!iomonitor_kobj) {
		printk("io_monitor:find kobj %s failed.\n", KBUILD_MODNAME);
		goto err;
	}

	printk("io_monitor:find kobj %s success.\n", KBUILD_MODNAME);

	return 0;
err:
	for(i = 1; i < MAX_IOMEVENT_PARAM - 1; i++) {
		if (io_monitor_env[i]) {
			kfree(io_monitor_env[i]);
		}
	}

	return -1;
}

static int __init io_monitor_init(void)
{
	int ret = -1;
	struct proc_dir_entry *daily_info_entry = NULL;
	struct proc_dir_entry *data_flow_entry = NULL;
	struct proc_dir_entry *uid_status_entry = NULL;
	struct proc_dir_entry *trigger_event_entry = NULL;
	struct proc_dir_entry *interval_entry = NULL;

	ret = io_monitor_resource_init();
	if (0 != ret) {
		printk("io_monitor:resource init failed.\n");
		return -1;
	}

	ret = iomonitor_uevent_init();
	if (ret) {
		goto err;
	}

	IoMonitor_dir = proc_mkdir("IoMonitor", NULL);
	if (IoMonitor_dir) {
		daily_info_entry = proc_create("daily", S_IRUGO | S_IWUGO, IoMonitor_dir, &proc_dailyio_operations);
		if (!daily_info_entry) {
			printk("io_monitor:create daily_info failed.\n");
			ret = -1;
			goto err;
		}
		data_flow_entry = proc_create("flow", S_IRUGO | S_IWUGO, IoMonitor_dir, &proc_dataflow_operations);
		if (!data_flow_entry) {
			printk("io_monitor:create data_flow failed.\n");
			ret = -1;
			goto err;
		}

		trigger_event_entry = proc_create("event", S_IRUGO | S_IWUGO, IoMonitor_dir, &trigger_abnormal_open_operations);
		if (!data_flow_entry) {
			printk("io_monitor:create trigger_event failed.\n");
			ret = -1;
			goto err;
		}

		uid_status_entry = create_uid_proc(IoMonitor_dir);
		if(!uid_status_entry)
		{
			printk("io_monitor:create uid_proc failed.\n");
			ret = -1;
			goto err;
		}

		interval_entry = proc_create("interval", S_IRUGO | S_IWUGO, IoMonitor_dir, &seq_abnormal_interval_fops);
		if (!interval_entry) {
			printk("io_monitor:create interval failed.\n");
			ret = -1;
			goto err;
		}
	}

	reqstats_init();
	add_monitor_items();
	add_daily_items();
	add_flow_items();

	io_abnormal_queue = create_singlethread_workqueue("iomonitor-queue");
	if (io_abnormal_queue == NULL) {
		printk("%s: failed to create work queue iomonitor-queue\n", __func__);
		goto err;
	}

	printk("io_monitor:init module success\n");

   return 0;

 err:
	if (IoMonitor_dir){
		remove_proc_subtree("IoMonitor", NULL);
		IoMonitor_dir = NULL;
	}
	io_monitor_resource_exit();
	return ret;
}


static void __exit io_monitor_exit(void)
{
	if (io_abnormal_queue) {
		destroy_workqueue(io_abnormal_queue);
		io_abnormal_queue = NULL;
	}
	remove_proc_subtree("IoMonitor", NULL);
	IoMonitor_dir = NULL;

	remove_uevent_resourse();

	io_monitor_resource_exit();

	printk("io_monitor: module exit\n");
}

module_init(io_monitor_init);
module_exit(io_monitor_exit);

MODULE_AUTHOR("geshifei <geshifei@oppo.com>");
MODULE_DESCRIPTION("iomonitor");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");


