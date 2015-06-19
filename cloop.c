/*
 *  cloop.c: Read-only compressed loop blockdevice
 *  hacked up by Rusty in 1999, extended and maintained by Klaus Knopper
 *  re-hacked for cloop v3 by Daniel Plasa 2013
 *
 *  For format specification of a compresses loop image see
 *   cloop.h
 *
 * Every version greatly inspired by code seen in loop.c
 * by Theodore Ts'o, 3/29/93.
 *
 * Copyright 1999-2009 by Paul `Rusty' Russell & Klaus Knopper.
 * Redistribution of this file is permitted under the GNU Public License.
 *
 */

#define CLOOP_NAME "cloop"
#define CLOOP_VERSION "3.00-alpha"
#define CLOOP_MAX 8

#ifndef KBUILD_MODNAME
#define KBUILD_MODNAME cloop
#endif

#ifndef KBUILD_BASENAME
#define KBUILD_BASENAME cloop
#endif

/* Check for ZLIB, LZO1X, LZ4 decompression algorithms in kernel. */
#if (!(defined(CONFIG_ZLIB_INFLATE) || defined(CONFIG_ZLIB_INFLATE_MODULE)))
#error  "Invalid Kernel configuration. CONFIG_ZLIB_INFLATE support is needed for cloop."
#endif
#if (!(defined(CONFIG_LZO_DECOMPRESS) || defined(CONFIG_LZO_DECOMPRESS_MODULE)))
#error  "Invalid Kernel configuration. CONFIG_LZO_DECOMPRESS support is needed for cloop."
#endif
#if (!(defined(CONFIG_DECOMPRESS_LZ4) || defined(CONFIG_DECOMPRESS_LZ4_MODULE)))
#error  "Invalid Kernel configuration. CONFIG_LZ4_DECOMPRESS support is needed for cloop."
#endif
#if (!(defined(CONFIG_DECOMPRESS_LZMA) || defined(CONFIG_DECOMPRESS_LZMA_MODULE)))
#error  "Invalid Kernel configuration. CONFIG_LZMA_DECOMPRESS support is needed for cloop."
#endif

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/major.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <asm/div64.h> /* do_div() for 64bit division */
#include <asm/uaccess.h>
#include <asm/byteorder.h>
/* Use zlib_inflate from lib/zlib_inflate */
#include <linux/zutil.h>
/* Use lzo */
#include <linux/lzo.h>
/* Use LZ4 */
#include <linux/lz4.h>
/* Use kernel xz decoder */
#include <linux/xz.h>


#include <linux/loop.h>
#include <linux/kthread.h>
#include <linux/compat.h>
#include "cloop.h"

/* New License scheme */
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif
#ifdef MODULE_AUTHOR
MODULE_AUTHOR("Klaus Knopper (current maintainer), Paul Russel (initial Kernel 2.2 version)");
#endif
#ifdef MODULE_DESCRIPTION
MODULE_DESCRIPTION("Transparently decompressing loopback block device");
#endif

#ifndef MIN
#define MIN(x,y) ((x) < (y) ? (x) : (y))
#endif

#ifndef MAX
#define MAX(x,y) ((x) > (y) ? (x) : (y))
#endif

/* Use experimental major for now */
#define MAJOR_NR 240

/* #define DEVICE_NAME CLOOP_NAME */
/* #define DEVICE_NR(device) (MINOR(device)) */
/* #define DEVICE_ON(device) */
/* #define DEVICE_OFF(device) */
/* #define DEVICE_NO_RANDOM */
/* #define TIMEOUT_VALUE (6 * HZ) */

#include <linux/blkdev.h>
#include <linux/buffer_head.h>

#if 1
#define DEBUGP printk
#else
#define DEBUGP(format, x...)
#endif

/* Default size of buffer to keep some decompressed blocks in memory to speed up access */
#define BLOCK_BUFFER_MEM (16*65536)

/* One file can be opened at module insertion time */
/* insmod cloop file=/path/to/file */
static char *file=NULL;
static unsigned int preload=0;
static unsigned int cloop_max=CLOOP_MAX;
static unsigned int buffers=BLOCK_BUFFER_MEM;
module_param(file, charp, 0);
module_param(preload, uint, 0);
module_param(cloop_max, uint, 0);
MODULE_PARM_DESC(file, "Initial cloop image file (full path) for /dev/cloop");
MODULE_PARM_DESC(preload, "Preload n blocks of cloop data into memory");
MODULE_PARM_DESC(cloop_max, "Maximum number of cloop devices (default 8)");
MODULE_PARM_DESC(buffers, "Size of buffer to keep uncompressed blocks in memory (default 1 MiB)");

static struct file *initial_file=NULL;
static int cloop_major=MAJOR_NR;

struct cloop_device
{
 /* Header filled from the file */
 struct cloop_info info;

 /* An or'd sum of all flags of each compressed block (v3) */
 u_int32_t allflags;

 /* We buffer some uncompressed blocks for performance */
 size_t num_buffered_blocks;	/* how many uncompressed blocks buffered for performance */
 u_int32_t *buffered_blocknum;  /* list of numbers of uncompressed blocks in buffer */
 u_int32_t current_bufnum;      /* which block is current */
 unsigned char **buffer;        /* cache space for num_buffered_blocks uncompressed blocks */
 void *compressed_buffer;       /* space for the largest compressed block */
 size_t preload_array_size;     /* Size of pointer array in blocks */
 size_t preload_size;           /* Number of successfully allocated blocks */
 char **preload_cache;          /* Pointers to preloaded blocks */

 z_stream zstream;
 struct xz_dec * xzdecoderstate;
 struct xz_buf xz_buffer;

 struct file   *backing_file;  /* associated file */
 struct inode  *backing_inode; /* for bmap */

 unsigned long largest_block;
 unsigned int underlying_blksize;
 int clo_number;
 int refcnt;
 struct block_device *bdev;
 int isblkdev;
 /* Lock for kernel block device queue */
 spinlock_t queue_lock;
 /* mutex for ioctl() */
 struct mutex clo_ctl_mutex;
 struct list_head clo_list;
 struct task_struct *clo_thread;
 wait_queue_head_t clo_event;
 struct request_queue *clo_queue;
 struct gendisk *clo_disk;
 int suspended;
 char clo_file_name[LO_NAME_SIZE];
};

/* Changed in 2.639: cloop_dev is now a an array of cloop_dev pointers,
   so we can specify how many devices we need via parameters. */
static struct cloop_device **cloop_dev;
static const char *cloop_name=CLOOP_NAME;
static int cloop_count = 0;

/* Use __get_free_pages instead of vmalloc, allows up to 32 pages,
 * 2MB in one piece */
static void *cloop_malloc(size_t size)
{
 int order = get_order(size);
 if(order <= KMALLOC_MAX_ORDER)
   return (void *)kmalloc(size, GFP_KERNEL);
 else if(order < MAX_ORDER)
   return (void *)__get_free_pages(GFP_KERNEL, order);
  return (void *)vmalloc(size);
}

static void cloop_free(void *mem, size_t size)
{
 int order = get_order(size);
 if(order <= KMALLOC_MAX_ORDER)
   kfree(mem);
 else if(order < MAX_ORDER)
   free_pages((unsigned long)mem, order);
 else vfree(mem);
}

static int uncompress(struct cloop_device *clo, u_int32_t block_num, u_int32_t compressed_length, unsigned long *uncompressed_length)
{
  /* uncompress block block_num which is the content of clo->copmpressed_buffer */
  /* into the cache buffer clo->current_bufnum                                  */
  int err = 0;
  if (0 == (clo->info.flags[block_num] & CLOOP3_COMPRESSOR_ANY))
  {
    /* block is umcompressed, swap pointers only! */
    char *tmp = clo->compressed_buffer;
    clo->compressed_buffer = clo->buffer[clo->current_bufnum];
    clo->buffer[clo->current_bufnum] = tmp;
    DEBUGP("cloop: block %d is uncompressed (flags=%d), just swapping %u bytes\n", block_num, clo->info.flags[block_num], compressed_length);
    
    /* block is umcompressed, copy only! */
    //memcpy(clo->buffer[clo->current_bufnum], clo->compressed_buffer, compressed_length);
    //DEBUGP("cloop: block %d is uncompressed, just copying %u bytes\n", block_num, compressed_length);
  }
  else if (clo->info.flags[block_num] & CLOOP3_COMPRESSOR_ZLIB)
  {
    clo->zstream.next_in = clo->compressed_buffer;
    clo->zstream.avail_in = compressed_length;
    clo->zstream.next_out = clo->buffer[clo->current_bufnum];
    clo->zstream.avail_out = clo->info.block_size;
    err = zlib_inflateReset(&clo->zstream);
    if (err != Z_OK)
    {
      printk(KERN_ERR "%s: zlib_inflateReset error %d\n", cloop_name, err);
      zlib_inflateEnd(&clo->zstream); zlib_inflateInit(&clo->zstream);
    }
    err = zlib_inflate(&clo->zstream, Z_FINISH);
    *uncompressed_length = clo->zstream.total_out;
    if (err == Z_STREAM_END) err = 0;
    DEBUGP("cloop: zlib decompression done, ret =%d, size =%lu\n", err, *uncompressed_length);
  }
  else if (clo->info.flags[block_num] & CLOOP3_COMPRESSOR_LZO1X)
  {
     uint32_t tmp = clo->info.block_size;
     err = lzo1x_decompress_safe(clo->compressed_buffer, compressed_length,
             clo->buffer[clo->current_bufnum], &tmp);
     if (err == LZO_E_OK) *uncompressed_length = tmp;
  }
  else if (clo->info.flags[block_num] & CLOOP3_COMPRESSOR_LZ4)
  {
     size_t outputSize = clo->info.block_size;
     if (block_num == clo->info.num_blocks-1)
    	 outputSize= MIN(outputSize, clo->info.original_size - block_num*clo->info.block_size);
	 err = lz4_decompress(clo->compressed_buffer, &compressed_length,
		 clo->buffer[clo->current_bufnum], outputSize);
     if (err >= 0) 
     {
       err = 0;
       *uncompressed_length = outputSize;
     }
  }
  else if (clo->info.flags[block_num] & CLOOP3_COMPRESSOR_XZ)
  {
	clo->xz_buffer.in = clo->compressed_buffer;
	clo->xz_buffer.in_pos = 0;
	clo->xz_buffer.in_size = compressed_length;
	clo->xz_buffer.out = clo->buffer[clo->current_bufnum];
	clo->xz_buffer.out_pos = 0;
	clo->xz_buffer.out_size = clo->info.block_size;
	xz_dec_reset(clo->xzdecoderstate);
	err = xz_dec_run(clo->xzdecoderstate, &clo->xz_buffer);
	if (err == XZ_STREAM_END || err == XZ_OK)
	{
		err = 0;
	}
	else
	{
		printk(KERN_ERR "%s: xz_dec_run error %d\n", cloop_name, err);
		err = 1;
	}
  }
  else if (clo->info.flags[block_num] & CLOOP3_COMPRESSOR_ANY)
  {
     printk(KERN_ERR "%s: compression method is not supported!\n", cloop_name);
  }
  return err;
}

static ssize_t cloop_read_from_file(struct cloop_device *clo, struct file *f, char *buf,
  loff_t pos, size_t buf_len)
{
 size_t buf_done=0;
 while (buf_done < buf_len)
  {
   size_t size = buf_len - buf_done, size_read;
   /* kernel_read() only supports 32 bit offsets, so we use vfs_read() instead. */
   /* int size_read = kernel_read(f, pos, buf + buf_done, size); */
   mm_segment_t old_fs = get_fs();
   set_fs(get_ds());
   size_read = vfs_read(f, (void __user *)(buf + buf_done), size, &pos);
   set_fs(old_fs);

   if(size_read <= 0)
    {
     printk(KERN_ERR "%s: Read error %d at pos %Lu in file %s, "
                     "%d bytes lost.\n", cloop_name, (int)size_read, pos,
		     file, (int)size);
     memset(buf + buf_len - size, 0, size);
     break;
    }
   buf_done += size_read;
  }
 return buf_done;
}

/* This looks more complicated than it is */
/* Returns number of cache block buffer to use for this request */
static int cloop_load_buffer(struct cloop_device *clo, int blocknum)
{
  unsigned long compressed_block_len;
  unsigned long uncompressed_block_len=0;

  int ret;
  int i;
  if(blocknum >= clo->info.num_blocks || blocknum < 0)
  {
    printk(KERN_WARNING "%s: Invalid block number %d requested.\n",
                       cloop_name, blocknum);
    return -1;
  }

  /* Quick return if the block we seek is already in one of the cache buffers. */
  /* Return number of buffer */
  for(i=0; i<clo->num_buffered_blocks; ++i)
    if (blocknum == clo->buffered_blocknum[i])
    {
      //DEBUGP(KERN_INFO "cloop_load_buffer: Found buffered block %d\n", i);
      return i;
    }

  /* get length of compressed block */
  /* therefore we have +1 offsets in memory! */
  compressed_block_len = clo->info.offsets[blocknum+1] - clo->info.offsets[blocknum];
  DEBUGP("cloop: load compressed block %d with len %lu\n", blocknum, compressed_block_len);

  /* Load that one compressed block from the file into compressed_buffer */
  {size_t n = cloop_read_from_file(clo, clo->backing_file, (char *)clo->compressed_buffer,
                    clo->info.offsets[blocknum], compressed_block_len);
   if (n!= compressed_block_len)
   {
      printk(KERN_ERR "%s: error while reading %lu bytes @ %lu from underlying file\n",
          cloop_name, compressed_block_len, (long unsigned int) clo->info.offsets[blocknum]);
      return -1;
   }
  }
  /* Go to next position in the cache block buffer (which is used as a cyclic buffer) */
  if(++clo->current_bufnum >= clo->num_buffered_blocks) clo->current_bufnum = 0;

  /* Do the uncompression */
  ret = uncompress(clo, blocknum, compressed_block_len, &uncompressed_block_len);

  DEBUGP("cloop: buflen after uncompress: %ld, ret=%d\n",uncompressed_block_len, ret); 
  if (ret != 0)
  {
    printk(KERN_ERR "%s: decompression error %i uncompressing block %u %lu bytes @ %lu, flags %u\n",
          cloop_name, ret, blocknum,
	  compressed_block_len, (long unsigned int) clo->info.offsets[blocknum], clo->info.flags[blocknum] );
    clo->buffered_blocknum[clo->current_bufnum] = -1;
    return -1;
  }
  clo->buffered_blocknum[clo->current_bufnum] = blocknum;
  return clo->current_bufnum;
}

/* This function does all the real work. */
/* returns "uptodate" */
static int cloop_handle_request(struct cloop_device *clo, struct request *req)
{
 int buffered_blocknum = -1;
 int preloaded = 0;
 loff_t offset     = (loff_t) blk_rq_pos(req)<<9; /* req->sector<<9 */
 struct bio_vec bvec;
 struct req_iterator iter;
 rq_for_each_segment(bvec, req, iter)
  {
   unsigned long len = bvec.bv_len;
   char *to_ptr      = kmap(bvec.bv_page) + bvec.bv_offset;
   while(len > 0)
    {
     u_int32_t length_in_buffer;
     loff_t block_offset = offset;
     u_int32_t offset_in_buffer;
     char *from_ptr;
     /* do_div (div64.h) returns the 64bit division remainder and  */
     /* puts the result in the first argument, i.e. block_offset   */
     /* becomes the blocknumber to load, and offset_in_buffer the  */
     /* position in the buffer */
     offset_in_buffer = do_div(block_offset, clo->info.block_size);
     /* Lookup preload cache */
     if(block_offset < clo->preload_size && clo->preload_cache != NULL &&
        clo->preload_cache[block_offset] != NULL)
      { /* Copy from cache */
       preloaded = 1;
       from_ptr = clo->preload_cache[block_offset];
      }
     else
      {
       preloaded = 0;
       buffered_blocknum = cloop_load_buffer(clo,block_offset);
       if(buffered_blocknum == -1) break; /* invalid data, leave inner loop */
       /* Copy from buffer */
       from_ptr = clo->buffer[buffered_blocknum];
      }
     /* Now, at least part of what we want will be in the buffer. */
     length_in_buffer = clo->info.block_size - offset_in_buffer;
     if(length_in_buffer > len)
      {
/*   DEBUGP("Warning: length_in_buffer=%u > len=%u\n",
                      length_in_buffer,len); */
       length_in_buffer = len;
      }
     memcpy(to_ptr, from_ptr + offset_in_buffer, length_in_buffer);
     to_ptr      += length_in_buffer;
     len         -= length_in_buffer;
     offset      += length_in_buffer;
    } /* while inner loop */
   kunmap(bvec.bv_page);
  } /* end rq_for_each_segment*/
 return ((buffered_blocknum!=-1) || preloaded);
}

/* Adopted from loop.c, a kernel thread to handle physical reads and
 * decompression. */
static int cloop_thread(void *data)
{
 struct cloop_device *clo = data;
 current->flags |= PF_NOFREEZE;
 set_user_nice(current, 10);
 while (!kthread_should_stop()||!list_empty(&clo->clo_list))
  {
   int err;
   err = wait_event_interruptible(clo->clo_event, !list_empty(&clo->clo_list) || 
                                  kthread_should_stop());
   if(unlikely(err))
    {
     DEBUGP(KERN_ERR "cloop thread activated on error!? Continuing.\n");
     continue;
    }
   if(!list_empty(&clo->clo_list))
    {
     struct request *req;
     unsigned long flags;
     int uptodate;
     spin_lock_irq(&clo->queue_lock);
     req = list_entry(clo->clo_list.next, struct request, queuelist);
     list_del_init(&req->queuelist);
     spin_unlock_irq(&clo->queue_lock);
     uptodate = cloop_handle_request(clo, req);
     spin_lock_irqsave(&clo->queue_lock, flags);
     __blk_end_request_all(req, uptodate ? 0 : -EIO);
     spin_unlock_irqrestore(&clo->queue_lock, flags);
    }
  }
 DEBUGP(KERN_ERR "cloop_thread exited.\n");
 return 0;
}

/* This is called by the kernel block queue management every now and then,
 * with successive read requests qeued and sorted in a (hopefully)
 * "most efficient way". spin_lock_irq() is being held by the kernel. */
static void cloop_do_request(struct request_queue *q)
{
 struct request *req;
 while((req = blk_fetch_request(q)) != NULL)
  {
   struct cloop_device *clo;
   int rw;
 /* quick sanity checks */
   /* blk_fs_request() was removed in 2.6.36 */
   if (unlikely(req == NULL || (req->cmd_type != REQ_TYPE_FS)))
    goto error_continue;
   rw = rq_data_dir(req);
   if (unlikely(rw != READ && rw != READA))
    {
     DEBUGP("cloop_do_request: bad command\n");
     goto error_continue;
    }
   clo = req->rq_disk->private_data;
   if (unlikely(!clo->backing_file && !clo->suspended))
    {
     DEBUGP("cloop_do_request: not connected to a file\n");
     goto error_continue;
    }
   list_add_tail(&req->queuelist, &clo->clo_list); /* Add to working list for thread */
   wake_up(&clo->clo_event);    /* Wake up cloop_thread */
   continue; /* next request */
  error_continue:
   DEBUGP(KERN_ERR "cloop_do_request: Discarding request %p.\n", req);
   req->errors++;
   __blk_end_request_all(req, -EIO);
  }
}

/* Read header and offsets (and flags) from already opened file */
static int cloop_set_file(int cloop_num, struct file *file, char *filename)
{
	enum sState {doBLKCHECK, doDETECTVERSION, doSEEKSIGNATURE, doLOADINDEX, doLOADINDEX2, doCHECKS, doOFFSETS, doFLAGS, doFINALCHECKS, doCOMPRESSORS, doDONE };
	enum sState curr_state = doBLKCHECK;

	struct cloop_device *clo = cloop_dev[cloop_num];
	struct inode *inode;
	char *bbuf=NULL;
	unsigned int i, offsets_read=0, total_offsets=0, flags_read=0, total_flags=0, min_required_filesize=0;
	loff_t fs_read_position = 0;
	int isblkdev;
	int error = 0;
	inode = file->f_inode;
	isblkdev=S_ISBLK(inode->i_mode)?1:0;
	if(!isblkdev&&!S_ISREG(inode->i_mode))
	{
		printk(KERN_ERR "%s: %s not a regular file or block device\n",
				cloop_name, filename);
		error=-EBADF; goto error_release;
	}
	clo->backing_file = file;
	clo->backing_inode= inode ;
	/* In suspended mode, we have done all checks necessary - FF */
	if (clo->suspended)
		return error;
	if(isblkdev)
	{
		struct request_queue *q = bdev_get_queue(inode->i_bdev);
		blk_queue_max_hw_sectors(clo->clo_queue, queue_max_hw_sectors(q)); /* Renamed in 2.6.34 */
		blk_queue_max_segments(clo->clo_queue, queue_max_segments(q)); /* Renamed in 2.6.34 */
		/* blk_queue_max_hw_segments(clo->clo_queue, queue_max_hw_segments(q)); */ /* Removed in 2.6.34 */
		blk_queue_max_segment_size(clo->clo_queue, queue_max_segment_size(q));
		blk_queue_segment_boundary(clo->clo_queue, queue_segment_boundary(q));
		blk_queue_merge_bvec(clo->clo_queue, q->merge_bvec_fn);
		clo->underlying_blksize = block_size(inode->i_bdev);
	}
	else
		clo->underlying_blksize = PAGE_SIZE;
	DEBUGP("cloop: underlying blocksize is %u\n", clo->underlying_blksize);

	/* we are about to check the header */
	curr_state = doDETECTVERSION;
	bbuf = cloop_malloc(clo->underlying_blksize);
	if(!bbuf)
	{
		printk(KERN_ERR "%s: out of kernel mem for block buffer (%lu bytes)\n",
				cloop_name, (unsigned long)clo->underlying_blksize);
		error=-ENOMEM; goto error_release;
	}

	/* now start reading header and block flags/offsets from file */
	while (curr_state != doDONE)
	{
		/* read a block of data from file */
		unsigned int offset = 0, num_readable;
		size_t bytes_readable = MIN(clo->underlying_blksize, inode->i_size - fs_read_position);
		size_t bytes_read = cloop_read_from_file(clo, file, bbuf, fs_read_position, bytes_readable);
		DEBUGP("cloop: try read %u bytes @ %llu\n", bytes_readable, fs_read_position);
		if(bytes_read != bytes_readable)
		{
			printk(KERN_ERR "%s: Bad file, read() %lu bytes @ %llu returned %d.\n",
					cloop_name, (unsigned long)clo->underlying_blksize, fs_read_position, (int)bytes_read);
			error=-EBADF;
			goto error_release;
		}
		/* remember where to read the next blk from file */
		fs_read_position +=bytes_read;

		if (curr_state == doDETECTVERSION)
		{
			/* v2 Header will be in block zero */
			if (strncmp(bbuf +CLOOP2_SIG_OFFSET, CLOOP2_SIGNATURE, strlen(CLOOP2_SIGNATURE))==0)
			{
				// this is a version 2.0 Format
				clo->info.file_version = 2;
				clo->info.block_size =  ntohl( * (u_int32_t*) (bbuf+ 128));
				clo->info.num_blocks =  ntohl( * (u_int32_t*) (bbuf+ 128+sizeof(u_int32_t)));
				DEBUGP(KERN_INFO "cloop: detected v2 file bs=%u, count=%u\n", clo->info.block_size, clo->info.num_blocks);
				// no way of telling the original size of uncompressed data - guessing blocksize*num.
				clo->info.original_size = clo->info.block_size; clo->info.original_size *= clo->info.num_blocks;
				// cloop data starts at offset
				offset = CLOOP2_HEADERSIZE;
				total_offsets = clo->info.num_blocks+1;
				total_flags = 0;
				min_required_filesize = CLOOP2_HEADERSIZE + sizeof(loff_t)*total_offsets;
				// no more data needed, do some checks
				curr_state = doCHECKS;
			}
			else
			{
				/* check if this is a v3+ file with signature at the end of image file */
				/* read last block of file                                             */
				loff_t tmp = inode->i_size;
				fs_read_position = inode->i_size - do_div(tmp, clo->underlying_blksize);
				DEBUGP(KERN_INFO "cloop: checking v3+ signature, load last block @ offset %llu\n", fs_read_position);
				curr_state = doSEEKSIGNATURE;
				continue; /* restart, load block at end of file */
			}
		}
		if (curr_state == doSEEKSIGNATURE)
		{
			/* check wich cloop file format is in the file */
			if (strncmp(bbuf + bytes_read - strlen(CLOOP3_SIGNATURE), CLOOP3_SIGNATURE, strlen(CLOOP3_SIGNATURE))==0)
			{
				// this is version 3+, copy file version and flags
				clo->info.file_version = ntohl( * (u_int32_t*) (bbuf+ bytes_read - 12));

				if (clo->info.file_version != 3)
				{
					printk(KERN_ERR "%s: Cannot handle cloop v3+ file format %d\n",
							cloop_name, (int) clo->info.file_version);
					error=-EBADF;
					goto error_release;
				}
				curr_state = doLOADINDEX;
			}
			else
			{
				printk(KERN_ERR "%s: Cannot determine cloop file format of %s. To read images (< V2.0), "
						"please use an older version of %s.\n",
						cloop_name, filename, cloop_name);
				error=-EBADF; goto error_release;
			}
		}

		if (curr_state == doLOADINDEX)
		{
			/* v3 header detected, get position of index data from end of file */
			/* last 8 bytes contain offset to that position ...                */
			fs_read_position = be64_to_cpu(*(loff_t*) ( bbuf + bytes_read - 20) );
			DEBUGP(KERN_INFO "cloop: index data offset is %llu\n", fs_read_position);
			curr_state = doLOADINDEX2;
			continue; /* start over, load block at the beginning of index data */
		}

		if (curr_state == doLOADINDEX2)
		{
			/* get block number and block size                      */
			clo->info.num_blocks =  ntohl( * (u_int32_t*) (bbuf));
			clo->info.block_size =  ntohl( * (u_int32_t*) (bbuf+4));
			clo->info.original_size =  be64_to_cpu( * (loff_t*) (bbuf+8));
			DEBUGP(KERN_INFO "cloop: block_size=%u, num_blocks=%u, original size was %llu bytes.\n", clo->info.block_size, clo->info.num_blocks, clo->info.original_size );
			// block offsets will start @ 16 bytes
			offset = 16;
			min_required_filesize = 0; // we would not have got here if the file was not large enough...
			total_offsets = clo->info.num_blocks+1;	// one offset more than actually blocks in file
													// to calculate size of last compressed block
			total_flags = clo->info.num_blocks;
			/* done with header, load offsets next */
			curr_state = doCHECKS;
		}

		if (curr_state == doCHECKS)
		{
			/* sanity checks */
			if (clo->info.block_size % 512 != 0)
			{
				printk(KERN_ERR "%s: blocksize %u not multiple of 512\n", cloop_name, clo->info.block_size);
				error=-EBADF; goto error_release;
			}
			if (!isblkdev && (min_required_filesize > inode->i_size))
			{
				printk(KERN_ERR "%s: file too small for %u blocks\n", cloop_name, clo->info.num_blocks);
				error=-EBADF; goto error_release;
			}
			/* allocate memory for block compression flags */
			clo->info.flags = cloop_malloc(sizeof (u_int32_t) * clo->info.num_blocks);
			if (!clo->info.flags)
			{
				printk(KERN_ERR "%s: out of kernel mem for flags\n", cloop_name);
				error=-ENOMEM; goto error_release;
			}
			memset(clo->info.flags, 0, sizeof (u_int32_t) * clo->info.num_blocks);

			/* allocate memory for block offsets */
			clo->info.offsets = cloop_malloc(sizeof(loff_t) * total_offsets);
			if (!clo->info.offsets)
			{
				printk(KERN_ERR "%s: out of kernel mem for offsets\n", cloop_name);
				error=-ENOMEM; goto error_release;
			}

			/* done with header, do offsets next */
			DEBUGP(KERN_INFO "cloop: header seems all good!\n");
			curr_state = doOFFSETS;
		}

		/* parse offsets */
		if (curr_state == doOFFSETS)
		{
			/* calculate how many offsets can be taken from current bbuf */
			num_readable = MIN(total_offsets - offsets_read,
					(bytes_read - offset) / sizeof(loff_t));
			DEBUGP( KERN_INFO "cloop: parsing %d offsets %d to %d\n", num_readable, offsets_read, offsets_read+num_readable-1);
			for (i=0; i<num_readable; i++)
			{
				loff_t tmp = be64_to_cpu( *(loff_t*) (bbuf+offset) );
				if (i%50==0) DEBUGP(KERN_INFO "cloop: offset %03d: %llu\n", offsets_read, tmp);
				clo->info.offsets[offsets_read++] = tmp;
				offset += sizeof(loff_t);
			}
			/* if done with offsets, do flags next */
			if (offsets_read >= total_offsets)
			{
				curr_state = doFLAGS;
			}
			else DEBUGP("cloop: need another block for offsets\n");
		}

		/* parse block flags */
		if (curr_state == doFLAGS)
		{
			/* calculate how many flags can be taken from current bbuf */
			/* this will not do anything on <= v2 since total_flags is zero thus num_readable will also be 0 */
			num_readable = MIN(total_flags - flags_read,
					(bytes_read - offset) / sizeof(u_int32_t));
			DEBUGP( KERN_INFO "cloop: parsing %d flags %d to %d\n", num_readable, flags_read, flags_read+num_readable-1);
			for (i=0; i<num_readable; i++)
			{
				clo->info.flags[flags_read++] = ntohl(*(u_int32_t*) (bbuf+offset) );
				if (i%50==0) DEBUGP(KERN_INFO "cloop: flag %03d: %llu\n", flags_read-1, fs_read_position-bytes_read+offset);
				offset += sizeof(u_int32_t);
			}
			/* if done with flags, do DONE next */
			if (flags_read >= total_flags)
				curr_state = doFINALCHECKS;
			else DEBUGP("cloop: need another block for flags\n");
		}

		if (curr_state == doFINALCHECKS )
		{
			/* Search for largest block rather than estimate. KK. */
			/* also check blocks for all used compressors and     */
			/* if <= v2 set compression flags accordingly         */
			size_t largest_block = 0;
			for(i=0; i<clo->info.num_blocks; ++i)
			{
				loff_t d=clo->info.offsets[i+1] - clo->info.offsets[i];
				largest_block=MAX(largest_block, d);
				if (clo->info.file_version <= 2)
				{
					clo->info.flags[i] = CLOOP3_COMPRESSOR_ZLIB;
				}
				clo->allflags |= clo->info.flags[i];
			}
			clo->largest_block = largest_block;

			printk(KERN_INFO "%s: %s [file version %d] %u blocks, %u bytes/block, largest block is %lu bytes, flags=%#x.\n",
					cloop_name, filename, clo->info.file_version, clo->info.num_blocks,
					clo->info.block_size, clo->largest_block, clo->allflags);

			/* check for last file offset correct */
			/* skip this check for v3+            */
			/* what is it good for anyway?        */
			if (clo->info.file_version == 2 && (!isblkdev && (clo->info.offsets[clo->info.num_blocks] != inode->i_size)))
			{
				printk(KERN_ERR "%s: final offset wrong (%Lu not %Lu)\n",
						cloop_name,
						clo->info.offsets[clo->info.num_blocks], inode->i_size);
				error = -EBADF; goto error_release;
			}

			/* calculate how many blocks fit in the cache buffer, allocate index memory */
			/* we will use only one buffer size for cache and compressed buffer - so we */
			/* might swap buffers in some cases instead of copying it                   */
			largest_block = MAX(clo->info.block_size, clo->largest_block);
			clo->num_buffered_blocks = buffers / largest_block;
			clo->buffered_blocknum = cloop_malloc(clo->num_buffered_blocks * sizeof (u_int32_t));
			clo->buffer = cloop_malloc(clo->num_buffered_blocks * sizeof (char*));
			if (!clo->buffered_blocknum || !clo->buffer)
			{
				printk(KERN_ERR "%s: out of kernel mem for index of cache buffer (%lu bytes)\n",
						cloop_name, (unsigned long)clo->num_buffered_blocks * sizeof (u_int32_t) + sizeof(char*) );
				error=-ENOMEM; goto error_release;
			}
			/* init with -1 to indicate that no valid block data is in that buffer */
			memset(clo->buffer, 0, clo->num_buffered_blocks * sizeof (char*));
			for (i=0; i < clo->num_buffered_blocks; i++)
			{
				clo->buffered_blocknum[i] = -1;
				clo->buffer[i] = cloop_malloc(largest_block);
				if (!clo->buffer[i])
				{
					printk(KERN_ERR "%s: out of kernel mem for cache buffer %d (%u bytes)\n",
							cloop_name, i, MAX(clo->info.block_size, largest_block) );
					error=-ENOMEM; goto error_release;
				}
			}
			clo->current_bufnum = 0;

			/* alloc mem for largest compressed block */
			/* also allocate cache buffers for uncompressed blocks */
			clo->compressed_buffer = cloop_malloc(largest_block);
			if(!clo->compressed_buffer)
			{
				printk(KERN_ERR "%s: out of memory for compressed buffer %u\n",
						cloop_name, largest_block);
				error=-ENOMEM; goto error_release;
			}
			curr_state = doCOMPRESSORS;
			DEBUGP(KERN_INFO "cloop: offsets and flags done\n");
		}

		/* allocate memory for used decompressors */
		if (curr_state == doCOMPRESSORS)
		{
			if (clo->allflags & CLOOP3_COMPRESSOR_ZLIB)
			{
				clo->zstream.workspace = cloop_malloc(zlib_inflate_workspacesize());
				if(!clo->zstream.workspace)
				{
					printk(KERN_ERR "%s: out of mem for zlib working area %u\n",
							cloop_name, zlib_inflate_workspacesize());
					error=-ENOMEM; goto error_release;
				}
				zlib_inflateInit(&clo->zstream);
			}
			if (clo->allflags & CLOOP3_COMPRESSOR_LZO1X)
			{
				/* no memory needed for LZO1X */
			}
			if (clo->allflags & CLOOP3_COMPRESSOR_XZ)
			{
				#if XZ_INTERNAL_CRC32
				/*
				 * This must be called before any other xz_* function to initialize
				 * the CRC32 lookup table.
				 */
				xz_crc32_init(void);
				#endif
				clo->xzdecoderstate = xz_dec_init(XZ_SINGLE, 0);
			}
			if (clo->allflags & CLOOP3_COMPRESSOR_LZ4)
			{
				/* no memory needed for LZ4 */
			}
			curr_state = doDONE;
		}
	} /* while (curr_state != doDONE) */

	{
		u_int64_t total_size = clo->info.original_size;
		u_int64_t secs = (total_size >> 9);
		u_int64_t ratio = 10000 * clo->info.offsets[clo->info.num_blocks];

		/* set capycity of compressed loop device in 512 byte sectors */
		if (secs << 9 < total_size) ++secs;
		set_capacity(clo->clo_disk, secs);

		/* do_div (div64.h) returns the 64bit division remainder and  */
		/* puts the result in the first argument, i.e. ratio   */
		do_div(ratio, total_size);
		printk(KERN_INFO "%s: cloop%d has %llu sectors, total %llu bytes "
				"(%llu bytes compressed, ratio %d.%d%%)\n",
				cloop_name, cloop_num, secs, total_size, (u_int64_t) clo->info.offsets[clo->info.num_blocks],
				((u_int32_t) ratio) / 100, ((u_int32_t)ratio)%100);
	}

	/* start cloop%d thread */
	clo->clo_thread = kthread_create(cloop_thread, clo, "cloop%d", cloop_num);
	if(IS_ERR(clo->clo_thread))
	{
		error = PTR_ERR(clo->clo_thread);
		clo->clo_thread=NULL;
		goto error_release;
	}
	if(preload > 0)
	{
		clo->preload_array_size = ((preload<=clo->info.num_blocks)?preload:clo->info.num_blocks);
		clo->preload_size = 0;
		if((clo->preload_cache = cloop_malloc(clo->preload_array_size * sizeof(char *))) != NULL)
		{
			int i;
			for(i=0; i<clo->preload_array_size; i++)
			{
				if((clo->preload_cache[i] = cloop_malloc(clo->info.block_size)) == NULL)
				{ /* Out of memory */
					printk(KERN_WARNING "%s: cloop_malloc(%d) failed for preload_cache[%d] (ignored).\n",
							cloop_name, clo->info.block_size, i);
					break;
				}
			}
			clo->preload_size = i;
			for(i=0; i<clo->preload_size; i++)
			{
				int buffered_blocknum = cloop_load_buffer(clo,i);
				int block_size = clo->info.block_size;
				if(buffered_blocknum >= 0)
				{
					memcpy(clo->preload_cache[i], clo->buffer[buffered_blocknum], block_size);
				}
				else
				{
					printk(KERN_WARNING "%s: can't read block %d into preload cache, set to zero.\n",
							cloop_name, i);
					memset(clo->preload_cache[i], 0, clo->info.block_size);
				}
			}
			printk(KERN_INFO "%s: preloaded %d blocks into cache.\n", cloop_name,
					(int)clo->preload_size);
		}
		else
		{
			/* It is not a fatal error if cloop_malloc(clo->preload_size)
			 * fails, then we just go without cache, but we should at least
			 * let the user know. */
			printk(KERN_WARNING "%s: cloop_malloc(%d) failed, continuing without preloaded buffers.\n",
					cloop_name, (int)(clo->preload_size * sizeof(char *)));
			clo->preload_array_size = clo->preload_size = 0;
		}
	}
	wake_up_process(clo->clo_thread);
	/* Uncheck */
	return error;

	error_release:
	if (clo->compressed_buffer) cloop_free(clo->compressed_buffer, MAX(clo->largest_block, clo->info.block_size));
	clo->compressed_buffer=NULL;
	if (clo->buffer)
	{
		size_t largest_block =  MAX(clo->largest_block, clo->info.block_size);
		for (i=0; i<clo->num_buffered_blocks; ++i)
			if (clo->buffer[i]) cloop_free(clo->buffer[i], largest_block);
		cloop_free(clo->buffer, clo->num_buffered_blocks*sizeof(char*));
	}
	clo->buffer=NULL;
	if (clo->buffered_blocknum) cloop_free(clo->buffered_blocknum, sizeof(u_int32_t)*clo->num_buffered_blocks);
	clo->buffered_blocknum = NULL;
	if (clo->info.flags) cloop_free(clo->info.flags, sizeof(u_int32_t) * total_offsets);
	clo->info.flags=NULL;
	if (clo->info.offsets) cloop_free(clo->info.offsets, sizeof(loff_t) * total_offsets);
	clo->info.offsets=NULL;
	if(bbuf) cloop_free(bbuf, clo->underlying_blksize);
	bbuf = NULL;
	clo->backing_file=NULL;
	return error;
}

/* Get file from ioctl arg (only losetup) */
static int cloop_set_fd(int cloop_num, struct file *clo_file,
                        struct block_device *bdev, unsigned int arg)
{
 struct cloop_device *clo = cloop_dev[cloop_num];
 struct file *file=NULL;
 int error = 0;

 /* Already an allocated file present */
 if(clo->backing_file) return -EBUSY;
 file = fget(arg); /* get filp struct from ioctl arg fd */
 if(!file) return -EBADF;
 error=cloop_set_file(cloop_num,file,"losetup_file");
 set_device_ro(bdev, 1);
 if(error) fput(file);
 return error;
}

/* Drop file and free buffers, both ioctl and initial_file */
static int cloop_clr_fd(int cloop_num, struct block_device *bdev)
{
  struct cloop_device *clo = cloop_dev[cloop_num];
  struct file *filp = clo->backing_file;
  int i;
  if(clo->refcnt > 1)	/* we needed one fd for the ioctl */
    return -EBUSY;
  if(filp==NULL) return -EINVAL;
  if(clo->clo_thread) { kthread_stop(clo->clo_thread); clo->clo_thread=NULL; }
  if (filp!=initial_file) 
    fput(filp);
  else 
  { 
    filp_close(initial_file,0); 
    initial_file=NULL; 
  }
  clo->backing_file  = NULL;
  clo->backing_inode = NULL;
  if(clo->info.offsets) 
  { 
    cloop_free(clo->info.offsets, clo->info.num_blocks*sizeof(loff_t));
    clo->info.offsets = NULL; 
  }
  if(clo->info.flags) 
  { 
    cloop_free(clo->info.flags, clo->info.num_blocks*sizeof(u_int32_t));
    clo->info.flags = NULL; 
  }
  if(clo->preload_cache)
  {
    for(i=0; i < clo->preload_size; i++)
      cloop_free(clo->preload_cache[i], clo->info.block_size);
      cloop_free(clo->preload_cache, clo->preload_array_size * sizeof(char *));
      clo->preload_cache = NULL;
      clo->preload_size = clo->preload_array_size = 0;
  }
  if (clo->buffer) 
  {
    cloop_free(clo->buffer, buffers);
    clo->buffer = NULL;
  }
  if (clo->buffered_blocknum)
  {
    cloop_free(clo->buffered_blocknum, sizeof(u_int32_t) * clo->num_buffered_blocks);
    clo->buffered_blocknum = NULL;
  }
  if (clo->compressed_buffer) 
  { 
     cloop_free(clo->compressed_buffer, clo->largest_block); 
     clo->compressed_buffer = NULL; 
  }
  /* clean up decompressors */
  if (clo->allflags & CLOOP3_COMPRESSOR_ZLIB)
  {
    zlib_inflateEnd(&clo->zstream);
    if(clo->zstream.workspace)
    { 
      cloop_free(clo->zstream.workspace, zlib_inflate_workspacesize()); 
      clo->zstream.workspace = NULL; 
    }
  }
  if (clo->allflags & CLOOP3_COMPRESSOR_LZ4)
  {
     /* no mem needed to decompress LZ4 */
  }
  if (clo->allflags & CLOOP3_COMPRESSOR_LZO1X)
  {
    /* no mem needed to decompress LZO1X */
  }
  if (clo->allflags & CLOOP3_COMPRESSOR_XZ)
  {
	  xz_dec_end(clo->xzdecoderstate);
  }
  if(bdev) invalidate_bdev(bdev);
  if(clo->clo_disk) set_capacity(clo->clo_disk, 0);
  return 0;
}

static int clo_suspend_fd(int cloop_num)
{
 struct cloop_device *clo = cloop_dev[cloop_num];
 struct file *filp = clo->backing_file;
 if(filp==NULL || clo->suspended) return -EINVAL;
 /* Suspend all running requests - FF */
 clo->suspended=1;
 if(filp!=initial_file) fput(filp);
 else { filp_close(initial_file,0); initial_file=NULL; }
 clo->backing_file  = NULL;
 clo->backing_inode = NULL;
 return 0;
}

/* Copied from loop.c, stripped down to the really necessary */
static int cloop_set_status(struct cloop_device *clo,
                            const struct loop_info64 *info)
{
 if (!clo->backing_file) return -ENXIO;
 memcpy(clo->clo_file_name, info->lo_file_name, LO_NAME_SIZE);
 clo->clo_file_name[LO_NAME_SIZE-1] = 0;
 return 0;
}

static int cloop_get_status(struct cloop_device *clo,
                            struct loop_info64 *info)
{
 struct file *file = clo->backing_file;
 struct kstat stat;
 int err;
 if (!file) return -ENXIO;
 //err = vfs_getattr(file->f_path.mnt, file->f_path.dentry, &stat);
 err = vfs_getattr(&file->f_path,  &stat);
 if (err) return err;
 memset(info, 0, sizeof(*info));
 info->lo_number  = clo->clo_number;
 info->lo_device  = huge_encode_dev(stat.dev);
 info->lo_inode   = stat.ino;
 info->lo_rdevice = huge_encode_dev(clo->isblkdev ? stat.rdev : stat.dev);
 info->lo_offset  = 0;
 info->lo_sizelimit = 0;
 info->lo_flags   = 0;
 memcpy(info->lo_file_name, clo->clo_file_name, LO_NAME_SIZE);
 return 0;
}

static void cloop_info64_from_old(const struct loop_info *info,
                                  struct loop_info64 *info64)
{
 memset(info64, 0, sizeof(*info64));
 info64->lo_number = info->lo_number;
 info64->lo_device = info->lo_device;
 info64->lo_inode = info->lo_inode;
 info64->lo_rdevice = info->lo_rdevice;
 info64->lo_offset = info->lo_offset;
 info64->lo_sizelimit = 0;
 info64->lo_flags = info->lo_flags;
 info64->lo_init[0] = info->lo_init[0];
 info64->lo_init[1] = info->lo_init[1];
 memcpy(info64->lo_file_name, info->lo_name, LO_NAME_SIZE);
}

static int cloop_info64_to_old(const struct loop_info64 *info64,
                               struct loop_info *info)
{
 memset(info, 0, sizeof(*info));
 info->lo_number = info64->lo_number;
 info->lo_device = info64->lo_device;
 info->lo_inode = info64->lo_inode;
 info->lo_rdevice = info64->lo_rdevice;
 info->lo_offset = info64->lo_offset;
 info->lo_flags = info64->lo_flags;
 info->lo_init[0] = info64->lo_init[0];
 info->lo_init[1] = info64->lo_init[1];
 memcpy(info->lo_name, info64->lo_file_name, LO_NAME_SIZE);
 return 0;
}

static int cloop_set_status_old(struct cloop_device *clo,
                                const struct loop_info __user *arg)
{
 struct loop_info info;
 struct loop_info64 info64;

 if (copy_from_user(&info, arg, sizeof (struct loop_info))) return -EFAULT;
 cloop_info64_from_old(&info, &info64);
 return cloop_set_status(clo, &info64);
}

static int cloop_set_status64(struct cloop_device *clo,
                              const struct loop_info64 __user *arg)
{
 struct loop_info64 info64;
 if (copy_from_user(&info64, arg, sizeof (struct loop_info64)))
  return -EFAULT;
 return cloop_set_status(clo, &info64);
}

static int cloop_get_status_old(struct cloop_device *clo,
                                struct loop_info __user *arg)
{
 struct loop_info info;
 struct loop_info64 info64;
 int err = 0;

 if (!arg) err = -EINVAL;
 if (!err) err = cloop_get_status(clo, &info64);
 if (!err) err = cloop_info64_to_old(&info64, &info);
 if (!err && copy_to_user(arg, &info, sizeof(info))) err = -EFAULT;
 return err;
}

static int cloop_get_status64(struct cloop_device *clo,
                              struct loop_info64 __user *arg)
{
 struct loop_info64 info64;
 int err = 0;
 if (!arg) err = -EINVAL;
 if (!err) err = cloop_get_status(clo, &info64);
 if (!err && copy_to_user(arg, &info64, sizeof(info64))) err = -EFAULT;
 return err;
}
/* EOF get/set_status */


static int cloop_ioctl(struct block_device *bdev, fmode_t mode,
	unsigned int cmd, unsigned long arg)
{
 struct cloop_device *clo;
 int cloop_num, err=0;
 if (!bdev) return -EINVAL;
 cloop_num = MINOR(bdev->bd_dev);
 if (cloop_num < 0 || cloop_num > cloop_count-1) return -ENODEV;
 clo = cloop_dev[cloop_num];
 mutex_lock(&clo->clo_ctl_mutex);
 switch (cmd)
  { /* We use the same ioctls that loop does */
   case LOOP_CHANGE_FD:
   case LOOP_SET_FD:
    err = cloop_set_fd(cloop_num, NULL, bdev, arg);
    if (err == 0 && clo->suspended)
     {
      /* Okay, we have again a backing file - get reqs again - FF */
      clo->suspended=0;
     }
     break;
   case LOOP_CLR_FD:
     err = cloop_clr_fd(cloop_num, bdev);
     break;
   case LOOP_SET_STATUS:
    err = cloop_set_status_old(clo, (struct loop_info __user *) arg);
    break;
   case LOOP_GET_STATUS:
    err = cloop_get_status_old(clo, (struct loop_info __user *) arg);
    break;
   case LOOP_SET_STATUS64:
    err = cloop_set_status64(clo, (struct loop_info64 __user *) arg);
    break;
   case LOOP_GET_STATUS64:
    err = cloop_get_status64(clo, (struct loop_info64 __user *) arg);
    break;
   case CLOOP_SUSPEND:
     err = clo_suspend_fd(cloop_num);
     break;
   default:
     err = -EINVAL;
  }
 mutex_unlock(&clo->clo_ctl_mutex);
 return err;
}

#ifdef CONFIG_COMPAT
static int cloop_compat_ioctl(struct block_device *bdev, fmode_t mode,
			   unsigned int cmd, unsigned long arg)
{
 switch(cmd) {
  case LOOP_SET_CAPACITY: /* Change arg */ 
  case LOOP_CLR_FD:       /* Change arg */ 
  case LOOP_GET_STATUS64: /* Change arg */ 
  case LOOP_SET_STATUS64: /* Change arg */ 
	arg = (unsigned long) compat_ptr(arg);
  case LOOP_SET_STATUS:   /* unchanged */
  case LOOP_GET_STATUS:   /* unchanged */
  case LOOP_SET_FD:       /* unchanged */
  case LOOP_CHANGE_FD:    /* unchanged */
	return cloop_ioctl(bdev, mode, cmd, arg);
	break;
 }
 return -ENOIOCTLCMD;
}
#endif


static int cloop_open(struct block_device *bdev, fmode_t mode)
{
 int cloop_num;
 if(!bdev) return -EINVAL;
 cloop_num=MINOR(bdev->bd_dev);
 if(cloop_num > cloop_count-1) return -ENODEV;
 /* Allow write open for ioctl, but not for mount. */
 /* losetup uses write-open and flags=0x8002 to set a new file */
 if(mode & FMODE_WRITE)
  {
   printk(KERN_WARNING "%s: Can't open device read-write in mode 0x%x\n", cloop_name, mode);
   return -EROFS;
  }
 cloop_dev[cloop_num]->refcnt+=1;
 return 0;
}

static void cloop_close(struct gendisk *disk, fmode_t mode)
{
 int cloop_num;
 if(!disk) return;
 cloop_num=((struct cloop_device *)disk->private_data)->clo_number;
 if(cloop_num < 0 || cloop_num > (cloop_count-1)) return;
 cloop_dev[cloop_num]->refcnt-=1;
}

static struct block_device_operations clo_fops =
{
        owner:		THIS_MODULE,
        open:           cloop_open,
        release:        cloop_close,
#ifdef CONFIG_COMPAT
	compat_ioctl:	cloop_compat_ioctl,
#endif
	ioctl:          cloop_ioctl
	/* locked_ioctl ceased to exist in 2.6.36 */
};

static int cloop_register_blkdev(int major_nr)
{
 return register_blkdev(major_nr, cloop_name);
}

static int cloop_unregister_blkdev(void)
{
 unregister_blkdev(cloop_major, cloop_name);
 return 0;
}

static int cloop_alloc(int cloop_num)
{
 struct cloop_device *clo = (struct cloop_device *) cloop_malloc(sizeof(struct cloop_device));;
 if(clo == NULL) goto error_out;
 cloop_dev[cloop_num] = clo;
 memset(clo, 0, sizeof(struct cloop_device));
 clo->clo_number = cloop_num;
 clo->clo_thread = NULL;
 init_waitqueue_head(&clo->clo_event);
 spin_lock_init(&clo->queue_lock);
 mutex_init(&clo->clo_ctl_mutex);
 INIT_LIST_HEAD(&clo->clo_list);
 clo->clo_queue = blk_init_queue(cloop_do_request, &clo->queue_lock);
 if(!clo->clo_queue)
  {
   printk(KERN_ERR "%s: Unable to alloc queue[%d]\n", cloop_name, cloop_num);
   goto error_out;
  }
 clo->clo_queue->queuedata = clo;
 clo->clo_disk = alloc_disk(1);
 if(!clo->clo_disk)
  {
   printk(KERN_ERR "%s: Unable to alloc disk[%d]\n", cloop_name, cloop_num);
   goto error_disk;
  }
 clo->clo_disk->major = cloop_major;
 clo->clo_disk->first_minor = cloop_num;
 clo->clo_disk->fops = &clo_fops;
 clo->clo_disk->queue = clo->clo_queue;
 clo->clo_disk->private_data = clo;
 sprintf(clo->clo_disk->disk_name, "%s%d", cloop_name, cloop_num);
 add_disk(clo->clo_disk);
 return 0;
error_disk:
 blk_cleanup_queue(clo->clo_queue);
error_out:
 return -ENOMEM;
}

static void cloop_dealloc(int cloop_num)
{
 struct cloop_device *clo = cloop_dev[cloop_num];
 if(clo == NULL) return;
 del_gendisk(clo->clo_disk);
 blk_cleanup_queue(clo->clo_queue);
 put_disk(clo->clo_disk);
 cloop_free(clo, sizeof(struct cloop_device));
 cloop_dev[cloop_num] = NULL;
}


/* LZ4 Stuff */
#if (defined USE_LZ4_INTERNAL)
#include "lz4_kmod.c"
#endif

static int __init cloop_init(void)
{
  int error=0;
  printk("%s: Initializing %s v"CLOOP_VERSION"\n", cloop_name, cloop_name);
  cloop_dev = (struct cloop_device **)cloop_malloc(cloop_max * sizeof(struct cloop_device *));
  if(cloop_dev == NULL) 
    return -ENOMEM;
  memset(cloop_dev, 0, cloop_max * sizeof(struct cloop_device *));
  cloop_count=0;
  cloop_major=MAJOR_NR;
  if(cloop_register_blkdev(MAJOR_NR))
  {
    printk(KERN_WARNING "%s: Unable to get major device %d\n", cloop_name, MAJOR_NR);
    /* Try dynamic allocation */
    if((cloop_major=cloop_register_blkdev(0))<0)
    {
      printk(KERN_ERR "%s: Unable to get dynamic major device\n", cloop_name);
      error = -EIO;
      goto init_out_cloop_free;
    }
    printk(KERN_INFO "%s: Got dynamic major device %d, mknod /dev/%s b %d 0\n",
          cloop_name, cloop_major, cloop_name, cloop_major);
  }
  while(cloop_count<cloop_max)
    if((error=cloop_alloc(cloop_count))!=0) break; else ++cloop_count;

  if(!cloop_count) goto init_out_dealloc;
  printk(KERN_INFO "%s: loaded (max %d devices)\n", cloop_name, cloop_count);

 if(file) /* global file name for first cloop-Device is a module option string. */
  {
   int namelen = strlen(file);
   if(namelen<1 ||
      (initial_file=filp_open(file,O_RDONLY|O_LARGEFILE,0x00))==NULL ||
      IS_ERR(initial_file))
    {
     error=PTR_ERR(initial_file);
     if(!error) error=-EINVAL;
     initial_file=NULL; /* if IS_ERR, it's NOT open. */
    }
   else
     error=cloop_set_file(0,initial_file,file);
   if(error)
    {
     printk(KERN_ERR
            "%s: Unable to get file %s for cloop device, error %d\n",
            cloop_name, file, error);
     goto init_out_dealloc;
    }
   if(namelen >= LO_NAME_SIZE) namelen = LO_NAME_SIZE-1;
   memcpy(cloop_dev[0]->clo_file_name, file, namelen);
   cloop_dev[0]->clo_file_name[namelen] = 0;
  }
 return 0;
init_out_dealloc:
 while (cloop_count>0) cloop_dealloc(--cloop_count);
 cloop_unregister_blkdev();
init_out_cloop_free:
 cloop_free(cloop_dev, cloop_max * sizeof(struct cloop_device *));
 cloop_dev = NULL;
 return error;
}

static void __exit cloop_exit(void)
{
 int error=0;
 if((error=cloop_unregister_blkdev())!=0)
  {
   printk(KERN_ERR "%s: cannot unregister block device\n", cloop_name);
   return;
  }
 while(cloop_count>0)
  {
   --cloop_count;
   if(cloop_dev[cloop_count]->backing_file) cloop_clr_fd(cloop_count, NULL);
   cloop_dealloc(cloop_count);
  }
 printk("%s: unloaded.\n", cloop_name);
}

/* The cloop init and exit function registration (especially needed for Kernel 2.6) */
module_init(cloop_init);
module_exit(cloop_exit);

#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, VERMAGIC_STRING);
