#include <linux/version.h>
#include <linux/autoconf.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>  /* printk() */
#include <linux/errno.h>   /* error codes */
#include <linux/types.h>   /* size_t */
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/wait.h>
#include <linux/file.h>

#include "spinlock.h"
#include "osprd.h"


//Set max number of locks
#define MAX_LOCKS 120

/* The size of an OSPRD sector. */
#define SECTOR_SIZE	512

/* This flag is added to an OSPRD file's f_flags to indicate that the file
 * is locked. */
#define F_OSPRD_LOCKED	0x80000

/* eprintk() prints messages to the console.
 * (If working on a real Linux machine, change KERN_NOTICE to KERN_ALERT or
 * KERN_EMERG so that you are sure to see the messages.  By default, the
 * kernel does not print all messages to the console.  Levels like KERN_ALERT
 * and KERN_EMERG will make sure that you will see messages.) */
#define eprintk(format, ...) printk(KERN_NOTICE format, ## __VA_ARGS__)

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("CS 111 RAM Disk");
// EXERCISE: Pass your names into the kernel as the module's authors.
MODULE_AUTHOR("Wai Lun Ho\nNabil Nathani");

#define OSPRD_MAJOR	222

/* This module parameter controls how big the disk will be.
 * You can specify module parameters when you load the module,
 * as an argument to insmod: "insmod osprd.ko nsectors=4096" */
static int nsectors = 32;
module_param(nsectors, int, 0);


/* The internal representation of our device. */
typedef struct osprd_info {
	uint8_t *data;                  // The data array. Its size is
	                                // (nsectors * SECTOR_SIZE) bytes.

	osp_spinlock_t mutex;           // Mutex for synchronizing access to
					// this block device

	unsigned ticket_head;		// Currently running ticket for
					// the device lock

	unsigned ticket_tail;		// Next available ticket for
					// the device lock

	wait_queue_head_t blockq;       // Wait queue for tasks blocked on
					// the device lock

	/* HINT: You may want to add additional fields to help
	         in detecting deadlock. */

	int r_locks;				//number of read locks;
	int w_locks;				//number of write locks
	pid_t r_hold[MAX_LOCKS];		//array of pids with a read lock
	pid_t r_wait[MAX_LOCKS];		//array of pids waiting for a read lock
	pid_t w_lock;				// write lock pid
	pid_t w_wait[MAX_LOCKS];		//array of pids waiting for a write lock


	// The following elements are used internally; you don't need
	// to understand them.
	struct request_queue *queue;    // The device request queue.
	spinlock_t qlock;		// Used internally for mutual
	                                //   exclusion in the 'queue'.
	struct gendisk *gd;             // The generic disk.
} osprd_info_t;

#define NOSPRD 4
static osprd_info_t osprds[NOSPRD];


// Declare useful helper functions
int osprd_ioctl(struct inode *inode, struct file *filp,
                unsigned int cmd, unsigned long arg);
/*
 * file2osprd(filp)
 *   Given an open file, check whether that file corresponds to an OSP ramdisk.
 *   If so, return a pointer to the ramdisk's osprd_info_t.
 *   If not, return NULL.
 */
static osprd_info_t *file2osprd(struct file *filp);

/*
 * for_each_open_file(task, callback, user_data)
 *   Given a task, call the function 'callback' once for each of 'task's open
 *   files.  'callback' is called as 'callback(filp, user_data)'; 'filp' is
 *   the open file, and 'user_data' is copied from for_each_open_file's third
 *   argument.
 */
static void for_each_open_file(struct task_struct *task,
			       void (*callback)(struct file *filp,
						osprd_info_t *user_data),
			       osprd_info_t *user_data);


/*
 * osprd_process_request(d, req)
 *   Called when the user reads or writes a sector.
 *   Should perform the read or write, as appropriate.
 */
static void osprd_process_request(osprd_info_t *d, struct request *req)
{
	int d_size = req->current_nr_sectors * SECTOR_SIZE;
	int offset = req->sector * SECTOR_SIZE;

	if (!blk_fs_request(req)) {
		end_request(req, 0);
		return;
	}

	// EXERCISE: Perform the read or write request by copying data between
	// our data array and the request's buffer.
	// Hint: The 'struct request' argument tells you what kind of request
	// this is, and which sectors are being read or written.
	// Read about 'struct request' in <linux/blkdev.h>.
	// Consider the 'req->sector', 'req->current_nr_sectors', and
	// 'req->buffer' members, and the rq_data_dir() function.

	// Your code here.
	// implement parameter check
	if ((req->current_nr_sectors + req->sector) > nsectors) {
		eprintk("Error");
		end_request(req, 0);

	}


	if(rq_data_dir(req) == READ)
		memcpy(req->buffer, d->data + offset, d_size);
	else if(rq_data_dir(req) == WRITE)
		memcpy(d->data + offset, req->buffer, d_size);
	else
	{
		end_request(req, 0);
		return;
	}
	end_request(req, 1);
}


// This function is called when a /dev/osprdX file is opened.
// You aren't likely to need to change this.
static int osprd_open(struct inode *inode, struct file *filp)
{
	// Always set the O_SYNC flag. That way, we will get writes immediately
	// instead of waiting for them to get through write-back caches.
	filp->f_flags |= O_SYNC;
	return 0;
}


// This function is called when a /dev/osprdX file is finally closed.
// (If the file descriptor was dup2ed, this function is called only when the
// last copy is closed.)
static int osprd_close_last(struct inode *inode, struct file *filp)
{
	if (filp) {
		osprd_info_t *d = file2osprd(filp);
		int filp_writable = filp->f_mode & FMODE_WRITE;

		// EXERCISE: If the user closes a ramdisk file that holds
		// a lock, release the lock.  Also wake up blocked processes
		// as appropriate.

		// Your code here.
		
		osprd_ioctl(inode, filp, OSPRDIOCRELEASE, 0);
		// This line avoids compiler warnings; you may remove it.
		(void) filp_writable, (void) d;

	}

	return 0;
}


static int osprd_deadlock_check_disk(int disk, int * dir, int dir_indx);
static int osprd_deadlock_check_process(pid_t p_node, int* dir, int dir_index);

static int osprd_deadlock_check_disk(int disk, int * dir, int dir_index)
{
	int x;
	pid_t p_node;			//process node

	//check if the disk on the travel path is deadlocked
	for( x=0; x < dir_index; x++)
	{
		if(dir[x] == disk)
		{
			return -1;
		}
	}

	//check every lock that is held by the disk
	for(x = -1; x < MAX_LOCKS; x++)
	{	//find held locks
		p_node = 0;
		//handle write locks
		if((x == -1) && osprds[disk].w_lock !=0) {
			p_node = osprds[disk].w_lock;
		}
		//handle read locks
		else if(osprds[disk].r_hold[x] != 0) {
			p_node = osprds[disk].r_hold[x];
		}
	}
	
	//find any held locks
	if(p_node !=0)
	{
		dir[dir_index] = disk;
		//if deadlocked
		if(osprd_deadlock_check_process(p_node, dir, dir_index+1) == -1)
		{
			return -1;
		}
	}

	return 0;
}


static int osprd_deadlock_check_process(pid_t p_node, int* dir, int dir_index)
{
	int x, y;
	int disk;
	
	//check every lock held by the disk
	for(x =0; x < NOSPRD; x++) {
		for(y = 0; y < MAX_LOCKS; y++) {
			disk = -1;
			//search for process
			if((osprds[x].w_wait[y] == p_node) || (osprds[x].r_wait[y] == p_node)){
				disk = x;
			}
		
			//find any disk waiting for a lock
			if(disk != -1){
				//deadlocked
				if(osprd_deadlock_check_disk(disk, dir, dir_index) == -1)
				{
					return -1;
				}
			}
		}
	}

	//no deadlocks
	return 0;
}

static int osprd_detect_deadlock(void)
{
	int x;
	int dir[NOSPRD];
	//check every block disk to see if deadlocked
	for(x =0; x < NOSPRD; x++)
	{
		if(osprd_deadlock_check_disk(x, dir, 0) == -1)
		{	
			return -1;
		}
	}
	return 0;

}
/*
 * osprd_lock
 */

/*
 * osprd_ioctl(inode, filp, cmd, arg)
 *   Called to perform an ioctl on the named file.
 */
int osprd_ioctl(struct inode *inode, struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	osprd_info_t *d = file2osprd(filp);	// device info
	int r = 0;				// return value: initially 0
	int x, o_slot;				//open slot
	
	if(d == NULL)
	{
		return -1;
	}

	// is file open for writing?
	int filp_writable = (filp->f_mode & FMODE_WRITE) != 0;

	// This line avoids compiler warnings; you may remove it.
	(void) filp_writable, (void) d;

	// Set 'r' to the ioctl's return value: 0 on success, negative on error

	if ( (cmd == OSPRDIOCACQUIRE) || (cmd == OSPRDIOCTRYACQUIRE)) {

		// EXERCISE: Lock the ramdisk.
		//
		// If *filp is open for writing (filp_writable), then attempt
		// to write-lock the ramdisk; otherwise attempt to read-lock
		// the ramdisk.
		//
                // This lock request must block using 'd->blockq' until:
		// 1) no other process holds a write lock;
		// 2) either the request is for a read lock, or no other process
		//    holds a read lock; and
		// 3) lock requests should be serviced in order, so no process
		//    that blocked earlier is still blocked waiting for the
		//    lock.
		//
		// If a process acquires a lock, mark this fact by setting
		// 'filp->f_flags |= F_OSPRD_LOCKED'.  You also need to
		// keep track of how many read and write locks are held:
		// change the 'osprd_info_t' structure to do this.
		//
		// Also wake up processes waiting on 'd->blockq' as needed.
		//
		// If the lock request would cause a deadlock, return -EDEADLK.
		// If the lock request blocks and is awoken by a signal, then
		// return -ERESTARTSYS.
		// Otherwise, if we can grant the lock request, return 0.

		// 'd->ticket_head' and 'd->ticket_tail' should help you
		// service lock requests in order.  These implement a ticket
		// order: 'ticket_tail' is the next ticket, and 'ticket_head'
		// is the ticket currently being served.  You should set a local
		// variable to 'd->ticket_head' and increment 'd->ticket_head'.
		// Then, block at least until 'd->ticket_tail == local_ticket'.
		// (Some of these operations are in a critical section and must
		// be protected by a spinlock; which ones?)

		// Your code here (instead of the next two lines).
		//eprintk("Attempting to acquire\n");
		//r = -ENOTTY;

		for(;;)
		{	//set a lock
			osp_spin_lock(&d->mutex);
	
			//file is writable, attempt to write lock
			if(filp_writable)
			{
				if(d->w_locks ==0 && d->r_locks==0)
				{
					//set write lock
					d->w_lock = current->pid;
					filp->f_flags |= F_OSPRD_LOCKED;
					d->w_locks++;

					//remove from write_wait_pid
					for(x = 0; x < MAX_LOCKS; x++)
					{
						if(d->w_wait[x] == current->pid)
						{
							d->w_wait[x] = 0;
							break;
						}
					}
					//unlock
					osp_spin_unlock(&d->mutex);
					break;
				}
				//write lock failed
				else if (cmd == OSPRDIOCACQUIRE)
				{
					o_slot = -1;
					//add to write_wait_pid array
					for(x = 0; x < MAX_LOCKS; x++)
					{
						if((o_slot == -1) && (d->w_wait[x] == 0 ))
						{
							o_slot = x;
						}
						if(d->w_wait[x] == current->pid) {
							x= -1;
							break;
						}
					}
					if(x != -1){
						d->w_wait[o_slot] = current->pid;
					}
				}

			}
			//attempt to read a lock
			else {
				//obtain read lock
				if(d->w_locks == 0)
				{
					//add to read_hold_pid array
					for(x=0; x < MAX_LOCKS; x++)
					{
						if(d->r_hold[x]==0)
						{
							d->r_hold[x] = current->pid;
							break;
						}
					}
					filp->f_flags |= F_OSPRD_LOCKED;
					d->r_locks++;

					//remove from read_wait_pid array
					for(x=0; x < MAX_LOCKS; x++)
					{
						if(d->r_wait[x] == current->pid)
						{
							d->r_wait[x] = 0;
							break;
						}
					}
					//unlock
					osp_spin_unlock(&d->mutex);
					break;
				}
				//read lock failed
				else if(cmd== OSPRDIOCACQUIRE)
				{
					int o_slot = -1;
					//add to read_wait_pid
					for(x = 0; x < MAX_LOCKS; x++)
					{
						if((o_slot==-1) && (d->r_wait[x] == 0))
						{
							o_slot = x;
						}
						//check if already on the array
						if(d->r_wait[x]==current->pid)
						{
							x=-1;
							break;
						}
					}
					if( x != -1)
					{
						d->r_wait[o_slot] = current->pid;
					}
				}
			}
			
			if(cmd == OSPRDIOCACQUIRE)
			{
				//check if deadlocked
				if(osprd_detect_deadlock() == -1)
				{
					//unlock process
					osp_spin_unlock(&d->mutex);
					return -EDEADLK;
				}
		
				int wait_result = wait_event_interruptible(d->blockq, 1);
	
				//unlock process
				osp_spin_unlock(&d->mutex);
				if(wait_result == -ERESTARTSYS)
				{
					return -ERESTARTSYS;
				}
				schedule();
			}
			else
			{
				//unlock
				osp_spin_unlock(&d->mutex);
				r = -EBUSY;
				break;
			}
		}
/*
	} else if (cmd == OSPRDIOCTRYACQUIRE) {

		// EXERCISE: ATTEMPT to lock the ramdisk.
		//
		// This is just like OSPRDIOCACQUIRE, except it should never
		// block.  If OSPRDIOCACQUIRE would block or return deadlock,
		// OSPRDIOCTRYACQUIRE should return -EBUSY.
		// Otherwise, if we can grant the lock request, return 0.

		// Your code here (instead of the next two lines).
		eprintk("Attempting to try acquire\n");
		r = -ENOTTY;
*/
	} else if (cmd == OSPRDIOCRELEASE) {

		// EXERCISE: Unlock the ramdisk.
		//
		// If the file hasn't locked the ramdisk, return -EINVAL.
		// Otherwise, clear the lock from filp->f_flags, wake up
		// the wait queue, perform any additional accounting steps
		// you need, and return 0.

		// Your code here (instead of the next line).
		//r = -ENOTTY;
		
		//lock
		osp_spin_lock(&d->mutex);

		//file hasnt been locked the ramdisk
		if((filp->f_flags & F_OSPRD_LOCKED) == 0)
		{
			r= -EINVAL;
		}
		else
		{
			//if file is open for writing
			if(filp_writable)
			{
				//clear write lock and wake up queue of processes
				d->w_locks--;
				d->w_lock=0;
				wake_up_all(&d->blockq);
			}
			else
			{
				//clear read locks
				d->r_locks--;
				for(x=0; x < MAX_LOCKS; x++)
				{
					//remove pid from read_hold_pid array
					if(d->r_hold[x] == current->pid)
					{
						d->r_hold[x]=0;
						break;
					}
				}
				//wake up the queue of processes
				wake_up_all(&d->blockq);
			}
			//clear lock
			filp->f_flags &= !F_OSPRD_LOCKED;
		}
		//unlock processes
		osp_spin_unlock(&d->mutex);
	} else
		r = -ENOTTY; /* unknown command */
	return r;
}


// Initialize internal fields for an osprd_info_t.

static void osprd_setup(osprd_info_t *d)
{
	/* Initialize the wait queue. */
	init_waitqueue_head(&d->blockq);
	osp_spin_lock_init(&d->mutex);
	d->ticket_head = d->ticket_tail = 0;
	/* Add code here if you add fields to osprd_info_t. */
	d->r_locks = 0;
	d->w_locks = 0;
	int x;
	for(x =0; x < MAX_LOCKS; x++)
	{
		d->r_hold[x]=0;
		d->r_wait[x]=0;
		d->w_wait[x] = 0;
	}
}


/*****************************************************************************/
/*         THERE IS NO NEED TO UNDERSTAND ANY CODE BELOW THIS LINE!          */
/*                                                                           */
/*****************************************************************************/

// Process a list of requests for a osprd_info_t.
// Calls osprd_process_request for each element of the queue.

static void osprd_process_request_queue(request_queue_t *q)
{
	osprd_info_t *d = (osprd_info_t *) q->queuedata;
	struct request *req;

	while ((req = elv_next_request(q)) != NULL)
		osprd_process_request(d, req);
}


// Some particularly horrible stuff to get around some Linux issues:
// the Linux block device interface doesn't let a block device find out
// which file has been closed.  We need this information.

static struct file_operations osprd_blk_fops;
static int (*blkdev_release)(struct inode *, struct file *);

static int _osprd_release(struct inode *inode, struct file *filp)
{
	if (file2osprd(filp))
		osprd_close_last(inode, filp);
	return (*blkdev_release)(inode, filp);
}

static int _osprd_open(struct inode *inode, struct file *filp)
{
	if (!osprd_blk_fops.open) {
		memcpy(&osprd_blk_fops, filp->f_op, sizeof(osprd_blk_fops));
		blkdev_release = osprd_blk_fops.release;
		osprd_blk_fops.release = _osprd_release;
	}
	filp->f_op = &osprd_blk_fops;
	return osprd_open(inode, filp);
}


// The device operations structure.

static struct block_device_operations osprd_ops = {
	.owner = THIS_MODULE,
	.open = _osprd_open,
	// .release = osprd_release, // we must call our own release
	.ioctl = osprd_ioctl
};


// Given an open file, check whether that file corresponds to an OSP ramdisk.
// If so, return a pointer to the ramdisk's osprd_info_t.
// If not, return NULL.

static osprd_info_t *file2osprd(struct file *filp)
{
	if (filp) {
		struct inode *ino = filp->f_dentry->d_inode;
		if (ino->i_bdev
		    && ino->i_bdev->bd_disk
		    && ino->i_bdev->bd_disk->major == OSPRD_MAJOR
		    && ino->i_bdev->bd_disk->fops == &osprd_ops)
			return (osprd_info_t *) ino->i_bdev->bd_disk->private_data;
	}
	return NULL;
}


// Call the function 'callback' with data 'user_data' for each of 'task's
// open files.

static void for_each_open_file(struct task_struct *task,
		  void (*callback)(struct file *filp, osprd_info_t *user_data),
		  osprd_info_t *user_data)
{
	int fd;
	task_lock(task);
	spin_lock(&task->files->file_lock);
	{
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 13)
		struct files_struct *f = task->files;
#else
		struct fdtable *f = task->files->fdt;
#endif
		for (fd = 0; fd < f->max_fds; fd++)
			if (f->fd[fd])
				(*callback)(f->fd[fd], user_data);
	}
	spin_unlock(&task->files->file_lock);
	task_unlock(task);
}


// Destroy a osprd_info_t.

static void cleanup_device(osprd_info_t *d)
{
	wake_up_all(&d->blockq);
	if (d->gd) {
		del_gendisk(d->gd);
		put_disk(d->gd);
	}
	if (d->queue)
		blk_cleanup_queue(d->queue);
	if (d->data)
		vfree(d->data);
}


// Initialize a osprd_info_t.

static int setup_device(osprd_info_t *d, int which)
{
	memset(d, 0, sizeof(osprd_info_t));

	/* Get memory to store the actual block data. */
	if (!(d->data = vmalloc(nsectors * SECTOR_SIZE)))
		return -1;
	memset(d->data, 0, nsectors * SECTOR_SIZE);

	/* Set up the I/O queue. */
	spin_lock_init(&d->qlock);
	if (!(d->queue = blk_init_queue(osprd_process_request_queue, &d->qlock)))
		return -1;
	blk_queue_hardsect_size(d->queue, SECTOR_SIZE);
	d->queue->queuedata = d;

	/* The gendisk structure. */
	if (!(d->gd = alloc_disk(1)))
		return -1;
	d->gd->major = OSPRD_MAJOR;
	d->gd->first_minor = which;
	d->gd->fops = &osprd_ops;
	d->gd->queue = d->queue;
	d->gd->private_data = d;
	snprintf(d->gd->disk_name, 32, "osprd%c", which + 'a');
	set_capacity(d->gd, nsectors);
	add_disk(d->gd);

	/* Call the setup function. */
	osprd_setup(d);

	return 0;
}

static void osprd_exit(void);


// The kernel calls this function when the module is loaded.
// It initializes the 4 osprd block devices.

static int __init osprd_init(void)
{
	int i, r;

	// shut up the compiler
	(void) for_each_open_file;
#ifndef osp_spin_lock
	(void) osp_spin_lock;
	(void) osp_spin_unlock;
#endif

	/* Register the block device name. */
	if (register_blkdev(OSPRD_MAJOR, "osprd") < 0) {
		printk(KERN_WARNING "osprd: unable to get major number\n");
		return -EBUSY;
	}

	/* Initialize the device structures. */
	for (i = r = 0; i < NOSPRD; i++)
		if (setup_device(&osprds[i], i) < 0)
			r = -EINVAL;

	if (r < 0) {
		printk(KERN_EMERG "osprd: can't set up device structures\n");
		osprd_exit();
		return -EBUSY;
	} else
		return 0;
}


// The kernel calls this function to unload the osprd module.
// It destroys the osprd devices.

static void osprd_exit(void)
{
	int i;
	for (i = 0; i < NOSPRD; i++)
		cleanup_device(&osprds[i]);
	unregister_blkdev(OSPRD_MAJOR, "osprd");
}


// Tell Linux to call those functions at init and exit time.
module_init(osprd_init);
module_exit(osprd_exit);
