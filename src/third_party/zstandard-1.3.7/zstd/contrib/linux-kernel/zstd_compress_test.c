/*
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 */

/* Compression level or 0 to disable */
#define DO_ZLIB 9
/* Compression level or 0 to disable */
#define DO_ZSTD 0
/* Buffer size */
#define BUFFER_SIZE 4096

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#if DO_ZSTD
#include <linux/zstd.h>
#endif

#if DO_ZLIB
#include <linux/zlib.h>
#endif

/* Device name to pass to register_chrdev(). */
#define DEVICE_NAME "zstd_compress_test"

/* Dynamically allocated device major number */
static int device_major;

/*
 * We reuse the same state, and thus can compress only one file at a time.
 */
static bool device_is_open;


static void *workspace = NULL;

/*
 * Input buffer used to put data coming from userspace.
 */
static uint8_t buffer_in[BUFFER_SIZE];
static uint8_t buffer_out[BUFFER_SIZE];

static uint64_t uncompressed_len;
static uint64_t compressed_len;

#if DO_ZSTD

static ZSTD_CStream *state;

static ZSTD_inBuffer input = {
	.src = buffer_in,
	.size = sizeof(buffer_in),
	.pos = sizeof(buffer_in),
};

static ZSTD_outBuffer output = {
	.dst = buffer_out,
	.size = sizeof(buffer_out),
	.pos = sizeof(buffer_out),
};

#endif /* DO_ZSTD */

#if DO_ZLIB

static z_stream state = {
	.next_in = buffer_in,
	.avail_in = 0,
	.total_in = 0,

	.next_out = buffer_out,
	.avail_out = sizeof(buffer_out),
	.total_out = 0,

	.msg = NULL,
	.state = NULL,
	.workspace = NULL,
};

#endif /* DO_ZLIB */

static int zstd_compress_test_open(struct inode *i, struct file *f)
{
	if (device_is_open)
		return -EBUSY;

	device_is_open = true;

	uncompressed_len = compressed_len = 0;

#if DO_ZSTD
	if (ZSTD_isError(ZSTD_resetCStream(state, 0)))
		return -EIO;
#endif

#if DO_ZLIB
	if (zlib_deflateReset(&state) != Z_OK)
		return -EIO;
#endif

	printk(KERN_INFO DEVICE_NAME ": opened\n");
	return 0;
}

static int zstd_compress_test_release(struct inode *i, struct file *f)
{
	device_is_open = false;

#if DO_ZSTD
	do {
		size_t ret;

		output.pos = 0;
		ret = ZSTD_endStream(state, &output);
		if (ZSTD_isError(ret)) {
			printk(KERN_INFO DEVICE_NAME ": zstd end error %u\n", ZSTD_getErrorCode(ret));
			return -EIO;
		}
		compressed_len += output.pos;
	} while (output.pos != output.size);
#endif

#if DO_ZLIB
	for (;;) {
		int ret;

		state.next_out = buffer_out;
		state.avail_out = sizeof(buffer_out);
		ret = zlib_deflate(&state, Z_FINISH);
		compressed_len += sizeof(buffer_out) - state.avail_out;
		if (ret == Z_STREAM_END)
			break;
		if (ret != Z_OK) {
			printk(KERN_INFO DEVICE_NAME ": zlib end error %d: %s\n", ret, state.msg);
			return -EIO;
		}
	}
#endif

	printk(KERN_INFO DEVICE_NAME ": uncompressed_len = %llu\n", uncompressed_len);
	printk(KERN_INFO DEVICE_NAME ": compressed_len   = %llu\n", compressed_len);
	printk(KERN_INFO DEVICE_NAME ": closed\n");
	return 0;
}

/*
 * Hash the data given to us from userspace.
 */
static ssize_t zstd_compress_test_write(struct file *file,
				 const char __user *buf, size_t size, loff_t *pos)
{
	size_t remaining = size;

	while (remaining > 0) {
		size_t const copy_size = min(remaining, sizeof(buffer_in));

		if (copy_from_user(buffer_in, buf, copy_size))
			return -EFAULT;
		buf += copy_size;
		remaining -= copy_size;
		uncompressed_len += copy_size;

#if DO_ZSTD
		input.pos = 0;
		input.size = copy_size;
		while (input.pos != input.size) {
			size_t ret;

			output.pos = 0;
			ret = ZSTD_compressStream(state, &output, &input);
			if (ZSTD_isError(ret)) {
				printk(KERN_INFO DEVICE_NAME ": zstd compress error %u\n", ZSTD_getErrorCode(ret));
				return -EIO;
			}
			compressed_len += output.pos;
		}
#endif
#if DO_ZLIB
		state.next_in = buffer_in;
		state.avail_in = copy_size;
		while (state.avail_in > 0) {
			int ret;

			state.next_out = buffer_out;
			state.avail_out = sizeof(buffer_out);
			ret = zlib_deflate(&state, Z_NO_FLUSH);
			compressed_len += sizeof(buffer_out) - state.avail_out;
			if (ret != Z_OK) {
				printk(KERN_INFO DEVICE_NAME ": zlib end error %d: %s\n", ret, state.msg);
				return -EIO;
			}
		}
#endif
	}
	return size;
}
/* register the character device. */
static int __init zstd_compress_test_init(void)
{
	static const struct file_operations fileops = {
		.owner = THIS_MODULE,
		.open = &zstd_compress_test_open,
		.release = &zstd_compress_test_release,
		.write = &zstd_compress_test_write
	};
	size_t workspace_size = 0;
#if DO_ZSTD
	ZSTD_parameters params;
#endif

	device_major = register_chrdev(0, DEVICE_NAME, &fileops);
	if (device_major < 0) {
		return device_major;
	}

#if DO_ZSTD
	params = ZSTD_getParams(DO_ZSTD, 0, 0);
	workspace_size = ZSTD_CStreamWorkspaceBound(params.cParams);

	if (!(workspace = vmalloc(workspace_size)))
		goto fail;
	if (!(state = ZSTD_initCStream(params, 0, workspace, workspace_size)))
		goto fail;
#endif

#if DO_ZLIB
	workspace_size = zlib_deflate_workspacesize(MAX_WBITS, MAX_MEM_LEVEL);

	if (!(workspace = vmalloc(workspace_size)))
		goto fail;
	state.workspace = workspace;
	if (zlib_deflateInit(&state, DO_ZLIB) != Z_OK)
		goto fail;
#endif

	printk(KERN_INFO DEVICE_NAME ": module loaded\n");
	printk(KERN_INFO DEVICE_NAME ": compression requires %zu bytes of memory\n", workspace_size);
	printk(KERN_INFO DEVICE_NAME ": Create a device node with "
			"'mknod " DEVICE_NAME " c %d 0' and write data "
			"to it.\n", device_major);
	return 0;

fail:
	printk(KERN_INFO DEVICE_NAME ": failed to load module\n");
	if (workspace) {
		vfree(workspace);
		workspace = NULL;
	}
	return -ENOMEM;
}

static void __exit zstd_compress_test_exit(void)
{
	unregister_chrdev(device_major, DEVICE_NAME);
#if DO_ZLIB
	zlib_deflateEnd(&state);
#endif
	if (workspace) {
		vfree(workspace);
		workspace = NULL;
	}
	printk(KERN_INFO DEVICE_NAME ": module unloaded\n");
}

module_init(zstd_compress_test_init);
module_exit(zstd_compress_test_exit);

MODULE_DESCRIPTION("Zstd compression tester");
MODULE_VERSION("1.0");

MODULE_LICENSE("Dual BSD/GPL");
