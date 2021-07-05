/*
 *   BSD LICENSE
 *
 *   Copyright (C) 2016 LightBits Labs Ltd. - All Rights Reserved
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of LightBits Labs Ltd nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#define FUSE_USE_VERSION 26
#include <fuse/fuse_lowlevel.h>
#include <dirent.h>
#include <stdbool.h>
#include <errno.h>
#include "procstat.h"
#include "list.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>
#include <sys/param.h>
#include <ctype.h>
#include <stdlib.h>
#include <time.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*a))
#endif

enum {
	STATS_ENTRY_FLAG_REGISTERED  = 1 << 0,
	STATS_ENTRY_FLAG_DIR	     = 1 << 1,
	STATS_ENTRY_FLAG_HISTOGRAM   = 1 << 2,
	STATS_ENTRY_FLAG_AGGREGATOR  = 1 << 3,
};

#define SERIES_RESET_CLOCK CLOCK_MONOTONIC_COARSE

#define ATTRIBUTES_TIMEOUT_SEC (60.0 * 60)
#define DNAME_INLINE_LEN 32
struct procstat_dynamic_name {
	unsigned zero:8;
	char     *buffer;
};

struct procstat_directory;
struct procstat_item {
	union {
		char iname[DNAME_INLINE_LEN];
		struct procstat_dynamic_name name;
	};
	struct procstat_directory *parent;
	uint32_t 	       name_hash;
	struct list_head       entry;
	int 		       refcnt;
	unsigned 	       flags;
};

struct procstat_directory {
	struct procstat_item base;
	struct list_head       children;
};

struct procstat_file {
	struct procstat_item	base;
	void  	    		*private;
	uint64_t		arg;
	procstats_formatter  	fmt;
	procstats_formatter  	writer;
};

struct procstat_context {
	struct procstat_directory root;
	char *mountpoint;
	struct fuse_session *session;
	gid_t	gid;
	uid_t   uid;
	pthread_mutex_t global_lock;
};

struct procstat_series {
	struct procstat_directory root;
	void  	    		  *private;
};

static uint32_t string_hash(const char *string)
{
	uint32_t hash = 0;
	unsigned char *i;

	for (i = (unsigned char*)string; *i; ++i)
	hash = 31 * hash + *i;
	return hash;
}

static struct procstat_file* fuse_inode_to_file(fuse_ino_t inode)
{
	return (struct procstat_file*)(inode);
}

static struct procstat_item *fuse_inode_to_item(struct procstat_context *context, fuse_ino_t inode)
{
	return (inode == FUSE_ROOT_ID) ? &context->root.base : (struct procstat_item *)(inode);
}

static struct procstat_directory *fuse_inode_to_dir(struct procstat_context *context, fuse_ino_t inode)
{
	return (struct procstat_directory *)fuse_inode_to_item(context, inode);
}

static struct procstat_context *request_context(fuse_req_t req)
{
	return (struct procstat_context *)fuse_req_userdata(req);
}

static bool root_directory(struct procstat_context *context, struct procstat_directory *directory)
{
	return &context->root == directory;
}

static bool stats_item_short_name(struct procstat_item *item)
{
	/* file name cannot start with \0, so in case
	 * of long dynamic name we mark first byte with 0
	 * this is how we know*/
	return item->name.zero != 0;
}

static const char *procstat_item_name(struct procstat_item *item)
{
	if (stats_item_short_name(item))
		return item->iname;
	return item->name.buffer;
}

static bool item_registered(struct procstat_item *item)
{
	return item->flags & STATS_ENTRY_FLAG_REGISTERED;
}

static bool item_type_directory(struct procstat_item *item)
{
	return (item->flags & STATS_ENTRY_FLAG_DIR);
}

static void free_histogram(struct procstat_series *series)
{
	struct procstat_histogram_u32 *hist = series->private;

	if (!hist->histogram)
		return;
	free(hist->histogram);
}

static void free_item(struct procstat_item *item)
{
	list_del(&item->entry);
	if (item_type_directory(item)) {
		struct procstat_directory *directory = (struct procstat_directory *)item;
		assert(list_empty(&directory->children));
	}

	if (!stats_item_short_name(item))
		free(item->name.buffer);

	if (item->flags & STATS_ENTRY_FLAG_HISTOGRAM)
		free_histogram((struct procstat_series *)item);

	free(item);
}

static void free_directory(struct procstat_directory *directory)
{
	free_item(&directory->base);
}

#define INODE_BLK_SIZE 4096
static void fill_item_stats(struct procstat_context *context, struct procstat_item *item, struct stat *stat)
{
	stat->st_uid = context->uid;
	stat->st_gid = context->gid;
	stat->st_ino = (__ino_t)item;
	struct procstat_file *file;

	if (item_type_directory(item)) {
		stat->st_mode = S_IFDIR | 0755;
		stat->st_nlink = root_directory(context, (struct procstat_directory *)item) ? 2 : 1;
		return;
	}

	stat->st_mode = S_IFREG;
	file = container_of(item, struct procstat_file, base);
	if (file->fmt)
		stat->st_mode |= 0444;
	if (file->writer)
		stat->st_mode |= 0222;
	stat->st_nlink = 1;
	stat->st_size = 0;
	stat->st_blocks = 0;
	stat->st_blksize = INODE_BLK_SIZE;
}

static struct procstat_item *lookup_item_locked(struct procstat_directory *parent,
						  const char *name,
						  uint32_t name_hash)
{
	struct procstat_item *item;

	list_for_each_entry(item, &parent->children, entry) {
		if (item->name_hash != name_hash)
			continue;
		if (strcmp(procstat_item_name(item), name) == 0)
			return item;
	}

	return NULL;
}

static void fuse_lookup(fuse_req_t req, fuse_ino_t parent_inode, const char *name)
{
	struct procstat_context *context = request_context(req);
	static struct procstat_directory *parent;
	struct procstat_item *item;
	struct fuse_entry_param fuse_entry;

	memset(&fuse_entry, 0, sizeof(fuse_entry));

	pthread_mutex_lock(&context->global_lock);
	parent = fuse_inode_to_dir(request_context(req), parent_inode);

	item = lookup_item_locked(parent, name, string_hash(name));
	if ((!item) || (!item_registered(item))) {
		pthread_mutex_unlock(&context->global_lock);
		fuse_reply_err(req, ENOENT);
		return;
	}

	fuse_entry.ino = (uintptr_t)item;
	item->refcnt++;
	fuse_entry.attr_timeout = ATTRIBUTES_TIMEOUT_SEC;
	fill_item_stats(context, item, &fuse_entry.attr);
	pthread_mutex_unlock(&context->global_lock);
	fuse_reply_entry(req, &fuse_entry);
}

static void item_put_locked(struct procstat_item *item);
static void fuse_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup) {
	struct procstat_context *context = request_context(req);
	struct procstat_item *item;

	pthread_mutex_lock(&context->global_lock);
	item = (struct procstat_item *)(ino);
	assert(nlookup <= item->refcnt);
	if (nlookup == item->refcnt) {
		item->refcnt = 1;
		item_put_locked(item);
	} else {
		item->refcnt -= nlookup;
	}
	pthread_mutex_unlock(&context->global_lock);
	fuse_reply_none(req);
}

static void fuse_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct stat stat;
	struct procstat_context *context = request_context(req);
	struct procstat_item *item;

	memset(&stat, 0, sizeof(stat));
	pthread_mutex_lock(&context->global_lock);
	item = fuse_inode_to_item(context, ino);
	if (!item_registered(item)) {
		pthread_mutex_unlock(&context->global_lock);
		fuse_reply_err(req, ENOENT);
		return;
	}

	fill_item_stats(context, item, &stat);
	pthread_mutex_unlock(&context->global_lock);
	fuse_reply_attr(req, &stat, ATTRIBUTES_TIMEOUT_SEC);
}

static void fuse_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct procstat_context *context = request_context(req);
	struct procstat_item *item;

	pthread_mutex_lock(&context->global_lock);
	item = fuse_inode_to_item(context, ino);

	if (!item_registered(item)) {
		pthread_mutex_unlock(&context->global_lock);
		fuse_reply_err(req, ENOENT);
		return;
	}
	++item->refcnt;
	pthread_mutex_unlock(&context->global_lock);
	fi->fh = 0;
	fuse_reply_open(req, fi);
}

static void fuse_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
		       size_t size, off_t off, struct fuse_file_info *fi)
{
	struct procstat_file *file = fuse_inode_to_file(ino);
	int num_objects;

	if (!file->writer) {
		fuse_reply_err(req, EIO);
		return;
	}

	num_objects = file->writer(file->private, file->arg, (char *)buf, size);
	/* we currently only support single format parameter */
	if (num_objects == 1)
		fuse_reply_write(req, size);
	else
		fuse_reply_err(req, EINVAL);
	return;
}

#define DEFAULT_BUFER_SIZE 1024
static void fuse_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
{
	struct procstat_context *context = request_context(req);
	static struct procstat_directory *dir;
	static struct procstat_item *iter;
	char *reply_buffer = NULL;
	size_t bufsize;
	size_t offset;
	int alloc_factor = 0;

	pthread_mutex_lock(&context->global_lock);
	dir = fuse_inode_to_dir(context, ino);

	if (!item_registered(&dir->base)) {
		pthread_mutex_unlock(&context->global_lock);
		fuse_reply_err(req, ENOENT);
		return;
	}

	/* FIXME: currently it is very naive implementation which requires lots of reallocations
	 * this can be improved with iovec. */
	/**
	 * FIXME: This is very inefficient since for every lookup this is called twice with offset 0 and with
	 * offset past last, We need to "rebuild" it and save it between "opendir" and "releasedir"
	 */

	bufsize = 0;
	offset = 0;
	list_for_each_entry(iter, &dir->children, entry) {
		const char *fname;
		size_t entry_size;
		struct stat stat;

		if (!item_registered(iter))
			continue;
		if (iter->flags & STATS_ENTRY_FLAG_AGGREGATOR)
			continue;
		memset(&stat, 0, sizeof(stat));
		fname = procstat_item_name(iter);
		fill_item_stats(context, iter, &stat);
		entry_size = fuse_add_direntry(req, NULL, 0, fname, NULL, 0);
		if (bufsize <= entry_size + offset) {
			bufsize = DEFAULT_BUFER_SIZE * (1 << alloc_factor);
			char *new_buffer = realloc(reply_buffer, bufsize);

			++alloc_factor;
			if (!new_buffer) {
				pthread_mutex_unlock(&context->global_lock);
				fuse_reply_err(req, ENOMEM);
				goto done;
			}
			reply_buffer = new_buffer;
		}
		fuse_add_direntry(req, reply_buffer + offset, entry_size, fname, &stat, offset + entry_size);
		offset += entry_size;
	}

	pthread_mutex_unlock(&context->global_lock);
	if (off < offset)
		fuse_reply_buf(req, reply_buffer + off, MIN(size, offset - off));
	else
		fuse_reply_buf(req, NULL, 0);
done:
	free(reply_buffer);
	return;
}

static bool allowed_open(struct procstat_item *item, struct fuse_file_info *fi)
{
	struct procstat_file *file = container_of(item, struct procstat_file, base);
	/* non control items can be only opened as readonly */
	if ((fi->flags & 3) == O_RDONLY)
		return true;

	if (file->writer)
		return true;
	return false;
}

#define READ_BUFFER_SIZE 100
struct read_struct {
	ssize_t size;
	char buffer[READ_BUFFER_SIZE];
	void *ext;
};

static void fuse_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct procstat_context *context = request_context(req);
	struct procstat_item *item;
	struct read_struct *read_buffer;
	int ret = EACCES;

	read_buffer = malloc(sizeof(struct read_struct));
	if (!read_buffer) {
		fuse_reply_err(req, ENOMEM);
		return;
	}

	pthread_mutex_lock(&context->global_lock);
	item = (struct procstat_item *)(ino);

	if (!item_registered(item))
		goto out_locked;

	if (!allowed_open(item, fi))
		goto out_locked;

	read_buffer->ext = NULL;
	fi->fh = (uint64_t)read_buffer;

	/* we dont know size of file in advance so use directio*/
	fi->direct_io = true;

	++item->refcnt;
	if (item->flags & STATS_ENTRY_FLAG_AGGREGATOR)
		++item->parent->base.refcnt;

	pthread_mutex_unlock(&context->global_lock);
	fuse_reply_open(req, fi);

	return;

out_locked:
	pthread_mutex_unlock(&context->global_lock);
	free(read_buffer);
	fuse_reply_err(req, ret);
}

struct out_stream {
	char *buf;
	size_t size;
	size_t total;
	unsigned lines;
	unsigned discard_lines;
};

#define AGGR_EXTRA_BYTES 0

struct aggregator_context {
	struct list_head *current;
	size_t off;
	unsigned discard_lines; /* from the front of the current */
};

struct aggregator_struct {
	struct aggregator_context c;
	size_t buf_size;
	char buffer[0];
};

#define MAX_PATH_LEN 120
static int out_item(struct out_stream *out, char *path, struct procstat_item *item)
{
	const char *fname;
	int len;
	int ret = 0;

	fname = procstat_item_name(item);
	if (!item_type_directory(item)) {
		struct procstat_file *file = container_of(item, struct procstat_file, base);
		int space = out->size - out->total;
		size_t total = out->total;

		if (!file->fmt)
			return 0; /* skipping write-only files */
		if (out->discard_lines) {
			--out->discard_lines;
			/*
			 * Count the discarded lines, so in case we run out of the buffer space
			 * on the first root directory item, the new discard_lines value will
			 * include the previous one and the number of newly generated lines.
			 */
			++out->lines;
			return 0;
		}
		len = snprintf(&out->buf[total], space, "%s/%s:", path, fname);
		total += len > space ? space : len;
		if (len > space)
			return -1;
		space = out->size - total;
		if (!space)
			return -1;
		len = file->fmt(file->private, file->arg, &out->buf[total], space);
		total += len > space ? space : len;
		if (len > space)
			return -1;
		if (len == space)
			/* exact fit: snprintf clobbers the last char (\n) with a 0: restore it */
			out->buf[total - 1] = '\n';
		/* Now the line is fully generated */
		out->total = total;
		++out->lines;
	} else {
		/* directory walk */
		struct procstat_item *child;
		int path_len = strlen(path);
		int pos = path_len;
		int p_space = MAX_PATH_LEN - path_len;
		struct procstat_directory *dir = container_of(item, struct procstat_directory, base);

		/* See fuse_read(): it is unsafe to read files under a directory that is marked unregistered */
		if (!item_registered(item))
			return 0;

		if (pos && p_space) {
			path[pos++] = '/';
			--p_space;
		}
		strncpy(path + pos, fname, p_space);
		path[MAX_PATH_LEN - 1] = 0;

		list_for_each_entry(child, &dir->children, entry) {
			ret = out_item(out, path, child);
			if (ret)
				break;
		}
		path[path_len] = 0;
	}

	return ret;
}

static void aggregator_read(fuse_req_t req, struct procstat_file *file, struct read_struct *rs, size_t size, off_t off)
{
	struct aggregator_struct *as = (struct aggregator_struct *)rs->ext;
	struct procstat_directory *dir = file->base.parent;
	struct list_head *last = &dir->children;
	struct list_head *self = &file->base.entry;
	struct out_stream out;
	char path[MAX_PATH_LEN];

	struct procstat_context *context = request_context(req);

	if (!as || (as->buf_size < size + AGGR_EXTRA_BYTES)) {
		struct aggregator_context c;

		if (!as) {
			c.current = NULL;
			c.discard_lines = 0;
			c.off = 0;
		} else {
			c = as->c;
			free(as);
		}

		as = malloc(sizeof(*as) + size + AGGR_EXTRA_BYTES);
		if (!as) {
			fuse_reply_buf(req, NULL, 0);
			return;
		}
		as->c = c;
		as->buf_size = size + AGGR_EXTRA_BYTES;
		rs->ext = (void *)as;
	}

	if (as->c.current == last) {
		fuse_reply_buf(req, NULL, 0);
		return;
	}

	out.buf = &as->buffer[0];
	out.total = 0;
	out.size = size;
	out.discard_lines = as->c.discard_lines;
	as->c.discard_lines = 0;

	if (off != as->c.off) {
		/* we do not support non-sequential read */
		out.total = sprintf(&out.buf[0], "Unexpected offset %ld wanted %ld size %ld\n", off, as->c.off, size);
		as->c.current = last;
		fuse_reply_buf(req, &out.buf[0], out.total);
		return;
	}

	/*
	 * While the aggregator node is open, the node and the parent directory node cannot be freed, so setting "last" above was safe.
	 */
	pthread_mutex_lock(&context->global_lock);

	if (!as->c.current) {
		as->c.current = dir->children.next;
	} else if (as->c.current != last) {
		struct procstat_item *current = container_of(as->c.current, struct procstat_item, entry);
		--current->refcnt;
		/* If this node has been deleted it is removed from parent's children list */
		if (list_empty(&current->entry)) {
			as->c.current = last;
		}
	}

	for (; as->c.current != last; as->c.current = as->c.current->next) {
		struct procstat_item *item;
		int ret;
		unsigned start_line = out.lines;

		if (as->c.current == self)
			continue;

		item = container_of(as->c.current, struct procstat_item, entry);
		path[0] = 0;
		ret = out_item(&out, path, item);
		if (ret) {
			/* out.total marks the end of the last complete line generated */
			if (out.total && (out.total < out.size)) {
				/* pad spaces at the end of the last complete line to the buffer end */
				memset(&out.buf[out.total - 1], ' ', out.size - out.total);
				out.total = out.size;
				out.buf[out.total - 1] = '\n';
			}
			as->c.discard_lines = out.lines - start_line; /* lines from current successfully written */
			break;
		}
	}

	/* Protect the current item from being freed, so we can safely access it next time */
	if (as->c.current != last)
		++(container_of(as->c.current, struct procstat_item, entry)->refcnt);

	as->c.off += out.total;
	pthread_mutex_unlock(&context->global_lock);
	fuse_reply_buf(req, &out.buf[0], out.total);
}

static void aggregator_release_locked(struct procstat_item *item, struct fuse_file_info *fi)
{
	struct read_struct *rs = (struct read_struct *)fi->fh;

	if (rs) {
		struct aggregator_struct *as = (struct aggregator_struct *)rs->ext;

		if (as && as->c.current) {
			if (as->c.current != &item->parent->children) {
				struct procstat_item *current = container_of(as->c.current, struct procstat_item, entry);

				--current->refcnt;
			}
		}
	}
	--item->parent->base.refcnt;
}

static void fuse_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
{
	struct read_struct *read_buffer = (struct read_struct *)fi->fh;
	struct procstat_file *file = fuse_inode_to_file(ino);

	if (file->base.flags & STATS_ENTRY_FLAG_AGGREGATOR) {
		aggregator_read(req, file, read_buffer, size, off);
		return;
	}


	if (!item_registered(&file->base)) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	if (!file->fmt) {
		fuse_reply_err(req, EPERM);
		return;
	}

	if (off == 0)
		read_buffer->size = file->fmt(file->private, file->arg, read_buffer->buffer, READ_BUFFER_SIZE);

	if (off >= read_buffer->size) {
		fuse_reply_buf(req, NULL, 0);
		return;
	}

	fuse_reply_buf(req, (char *)read_buffer->buffer + off, read_buffer->size - off);
}

static bool valid_filename(const char *name)
{
	const char *c;

	for (c = name; *c; ++c) {
		if (isalnum(*c) || (*c == '.') || (*c == '_') || (*c == '-'))
			continue;
		return false;
	}
	return true;
}

static void init_item(struct procstat_item *item, const char *name)
{
	size_t name_len = strlen(name);

	item->name_hash = string_hash(name);
	if (name_len < DNAME_INLINE_LEN)
		strcpy(item->iname, name);
	else {
		item->name.zero = 0;
		item->name.buffer = strdup(name);
	}
	INIT_LIST_HEAD(&item->entry);
}

static struct procstat_file *allocate_file_item(const char *name,
					   	void *priv,
						procstats_formatter fmt,
						procstats_formatter writer)
{
	struct procstat_file *file;

	file = calloc(1, sizeof(*file));
	if (!file)
		return NULL;

	init_item(&file->base, name);
	file->private = priv;
	file->fmt = fmt;
	file->writer = writer;
	return file;
}

static int register_item(struct procstat_context *context,
			 struct procstat_item *item,
			 struct procstat_directory *parent)
{
	pthread_mutex_lock(&context->global_lock);
	if (parent) {
		struct procstat_item *duplicate;

		duplicate = lookup_item_locked(parent, procstat_item_name(item), item->name_hash);
		if (duplicate) {
			pthread_mutex_unlock(&context->global_lock);
			return EEXIST;
		}
		list_add_tail(&item->entry, &parent->children);
	}
	item->flags |= STATS_ENTRY_FLAG_REGISTERED;
	item->refcnt = 1;
	item->parent = parent;
	pthread_mutex_unlock(&context->global_lock);
	return 0;
}

static int init_directory(struct procstat_context *context,
			  struct procstat_directory *directory,
			  const char *name,
			  struct procstat_directory *parent)
{
	int error;

	init_item(&directory->base, name);
	directory->base.flags = STATS_ENTRY_FLAG_DIR;
	INIT_LIST_HEAD(&directory->children);
	error = register_item(context, &directory->base, parent);
	if (error)
		return error;

	return 0;
}

static void item_put_locked(struct procstat_item *item);
static void item_put_children_locked(struct procstat_directory *directory)
{
	struct procstat_item *iter, *n;
	list_for_each_entry_safe(iter, n, &directory->children, entry) {
		iter->parent = NULL;
		item_put_locked(iter);
	}
}

static void item_put_locked(struct procstat_item *item)
{
	assert(item->refcnt);

	if (!item_registered(item))
		goto free_item;

	list_del_init(&item->entry);
	item->flags &= ~STATS_ENTRY_FLAG_REGISTERED;
	if (item_type_directory(item))
		item_put_children_locked((struct procstat_directory *)item);

free_item:
	if (--item->refcnt)
		return;
	free_item(item);
}

static struct procstat_item *parent_or_root(struct procstat_context *context, struct procstat_item *parent)
{
	if (!parent)
		return &context->root.base;
	else if (item_type_directory(parent))
		return parent;
	return NULL;
}

static struct procstat_file *create_file(struct procstat_context *context,
					 struct procstat_directory *parent,
					 const char *name, void *item,
					 procstats_formatter fmt, procstats_formatter writer)
{
	struct procstat_file *file;
	int error;

	if (!valid_filename(name)) {
		errno = -EINVAL;
		return NULL;
	}

	file = allocate_file_item(name, item, fmt, writer);
	if (!file) {
		errno = ENOMEM;
		return NULL;
	}

	error = register_item(context,&file->base, parent);
	if (error) {
		errno = error;
		free_item(&file->base);
		return NULL;

	}
	return file;
}

struct procstat_item *procstat_create_directory(struct procstat_context *context,
					   	struct procstat_item *parent,
						const char *name)
{
	struct procstat_directory *new_directory;
	int error;

	parent = parent_or_root(context, parent);
	if (!parent) {
		errno = EINVAL;
		return NULL;
	}

	if (!valid_filename(name)) {
		errno = -EINVAL;
		return NULL;
	}

	new_directory = calloc(1, sizeof(*new_directory));
	if (!new_directory) {
		errno = ENOMEM;
		return NULL;
	}

	error = init_directory(context, new_directory, name, (struct procstat_directory *)parent);
	if (error) {
		free_directory(new_directory);
		errno = error;
		return NULL;
	}

	return &new_directory->base;
}

void procstat_remove(struct procstat_context *context, struct procstat_item *item)
{
	struct procstat_directory *directory;

	assert(context);
	assert(item);

	pthread_mutex_lock(&context->global_lock);

	if (!item_registered(item))
		goto done;

	if (!item_type_directory(item))
		goto remove_item;

	directory = (struct procstat_directory *)item;
	if (root_directory(context, directory)) {
		item_put_children_locked(directory);
		goto done;
	}

remove_item:
	item_put_locked(item);
done:
	pthread_mutex_unlock(&context->global_lock);
}

int procstat_remove_by_name(struct procstat_context *context,
			    struct procstat_item *parent,
			    const char *name)
{
	struct procstat_item *item;

	parent = parent_or_root(context, parent);
	if (!parent) {
		errno = EINVAL;
		return -1;
	}

	pthread_mutex_lock(&context->global_lock);
	item = lookup_item_locked((struct procstat_directory *)parent,
				  name, string_hash(name));
	if (!item) {
		pthread_mutex_unlock(&context->global_lock);
		return ENOENT;
	}
	item_put_locked(item);
	pthread_mutex_unlock(&context->global_lock);
	return 0;
}

int procstat_create_simple(struct procstat_context *context,
			   struct procstat_item *parent,
			   struct procstat_simple_handle *descriptors,
			   size_t descriptors_size)
{
	int i;

	parent = parent_or_root(context, parent);
	if (!parent) {
		errno = EINVAL;
		return -1;
	}

	for (i = 0; i < descriptors_size; ++i) {
		struct procstat_file *file;
		struct procstat_simple_handle *descriptor = &descriptors[i];

		file = create_file(context, (struct procstat_directory *)parent,
				   descriptor->name, descriptor->object,
				   descriptor->fmt, descriptor->writer);
		if (!file) {
			--i;
			goto error_release;
		}
		file->arg = descriptor->arg;
	}
	return 0;
error_release:

	for (; i >= 0; --i)
		procstat_remove_by_name(context, parent, descriptors[i].name);
	return -1;
}

int procstat_create_aggregator(struct procstat_context *context,
			      struct procstat_item *parent,
			      const char *name)
{
	parent = parent_or_root(context, parent);
	if (!parent) {
		errno = EINVAL;
		return -1;
	}

	struct procstat_file *file;

	file = create_file(context, (struct procstat_directory *)parent,
			   name, NULL, NULL, NULL);
	if (!file)
		return -1;

	file->base.flags |= STATS_ENTRY_FLAG_AGGREGATOR;

	return 0;
}

bool is_reset(struct reset_info* reset)
{
	unsigned reset_requested = 0;
	bool reset_interval_expired = false;
	uint64_t reset_interval, time_since_last_reset;

	struct timespec cur_time;
	if (clock_gettime(SERIES_RESET_CLOCK, &cur_time) == 0) {
		time_since_last_reset = cur_time.tv_sec - reset->last_reset_time;
		reset_interval = __atomic_load_n(&reset->reset_interval, __ATOMIC_RELAXED);
		if ((reset_interval) && (time_since_last_reset > reset_interval)) {
			reset_interval_expired = true;
			reset->last_reset_time = cur_time.tv_sec;
		}
	}

	if (reset_interval_expired == false) {
		reset_requested = __atomic_load_n(&reset->reset_flag, __ATOMIC_RELAXED);
	}

	if (reset_interval_expired || reset_requested) {
		return true;
	} else {
		return false;
	}
}

void clear_values_series(struct procstat_series_u64 *series)
{
	series->count = 0;
	series->sum = 0;
	series->mean = 0;
	series->aggregated_variance = 0;
	series->min = ULLONG_MAX;
	series->max = 0;
	__atomic_store_n(&series->reset.reset_flag, 0, __ATOMIC_RELEASE);
}

void procstat_u64_series_add_point(struct procstat_series_u64 *series, uint64_t value)
{
	int64_t delta;
	int64_t delta2;
	int64_t avg_delta;

	if (is_reset(&series->reset)) 
		clear_values_series(series);

	if (value < series->min)
		series->min = value;
	if (value > series->max)
		series->max = value;
	++series->count;
	series->last = value;
	series->sum += value;

	/* Calculate mean and estimated variance according to Welford algorithm
	 * https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance*/
	delta = (int64_t)value - series->mean;
	avg_delta = delta / (int64_t)series->count;
	series->mean = (int64_t)series->mean + avg_delta;
	delta2 = (int64_t)value - series->mean;
	series->aggregated_variance += delta * delta2;
}

enum series_u64_type{
	SERIES_SUM = 0,
	SERIES_COUNT = 1,
	SERIES_MIN = 2,
	SERIES_MAX = 3,
	SERIES_LAST = 4,
	SERIES_AVG = 5,
	SERIES_MEAN = 6,
	SERIES_STDEV = 7,
	SERIES_RESET_INTERVAL = 8,
};

static ssize_t series_u64_read(void *object, uint64_t arg, char *buffer, size_t len)
{
	struct procstat_series_u64 *series = object;
	enum series_u64_type type = arg;
	uint64_t *data_ptr = NULL;
	uint64_t data;
	uint64_t count;

	if (is_reset(&series->reset))
		clear_values_series(series);

	count = *((volatile uint64_t *)&series->count);
	switch (type) {
	case SERIES_SUM:
		data_ptr = &series->sum;
		goto write_var;
	case SERIES_COUNT:
		data_ptr = &series->count;
		goto write_var;
	case SERIES_LAST:
		data_ptr = &series->last;
		goto write_var;
	case SERIES_MEAN:
		data_ptr = &series->mean;
		goto write_var;
	case SERIES_MIN:
		data_ptr = &series->min;
		goto write_var;
	case SERIES_MAX:
		data_ptr = &series->max;
		goto write_var;
	case SERIES_AVG:
		if (!count)
			goto write_zero;
		data = series->sum / count;
		data_ptr = &data;
		goto write_var;
	case SERIES_STDEV:
		if (count < 2)
			goto write_zero;
		data = series->aggregated_variance / (count - 1);
		data_ptr = &data;
		goto write_var;
	case SERIES_RESET_INTERVAL:
		data = series->reset.reset_interval;
		data_ptr = &data;
		goto write_var;
	default:
		return -1;
	}
write_zero:
	return snprintf(buffer, len, "0\n");
write_var:
	return procstat_format_u64_decimal(data_ptr, arg, buffer, len);

}

static int register_u64_series_files(struct procstat_context *context,
				     struct procstat_series *series_stat)
{
	struct procstat_series_u64 *series = series_stat->private;
	struct procstat_simple_handle descriptors[] = {
			{"sum",    			series, SERIES_SUM, series_u64_read},
			{"count",  			series, SERIES_COUNT, series_u64_read},
			{"min",    			series, SERIES_MIN, series_u64_read},
			{"max",    			series, SERIES_MAX, series_u64_read},
			{"last",   			series, SERIES_LAST, series_u64_read},
			{"avg",    			series, SERIES_AVG, series_u64_read},
			{"mean",   			series, SERIES_MEAN, series_u64_read},
			{"stddev", 			series, SERIES_STDEV, series_u64_read},
			{"get_reset_interval_sec", 	series, SERIES_RESET_INTERVAL, series_u64_read}};


	return procstat_create_simple(context, &series_stat->root.base, descriptors, ARRAY_SIZE(descriptors));
}

static ssize_t reset_u64_series(void *object, uint64_t arg, char *buffer, size_t length)
{
	struct procstat_series *series_stat = object;
	struct procstat_series_u64 *series = series_stat->private;
	uint32_t control;

	control = strtoul(buffer, NULL, 10);
	if (control != 1)
		return EINVAL;

	__atomic_store_n(&series->reset.reset_flag, 1, __ATOMIC_RELAXED);
	return 1;
}

static ssize_t set_reset_interval_u64_series(void *object, uint64_t arg, char *buffer, size_t length)
{
	struct procstat_series *series_stat = object;
	struct procstat_series_u64 *series = series_stat->private;
	int32_t control;

	control = strtoul(buffer, NULL, 10);
	if (control < 0)
		return EINVAL;

	__atomic_store_n(&series->reset.reset_interval, control, __ATOMIC_RELAXED);
	return 1;
}

int procstat_create_u64_series(struct procstat_context *context, struct procstat_item *parent,
			       const char *name, struct procstat_series_u64 *series)
{
	struct procstat_series *series_stat;
	struct procstat_simple_handle control[] = {
		{.name = "reset", .writer = reset_u64_series},
		{.name = "reset_interval_sec", .writer = set_reset_interval_u64_series}};
	int error;

	parent = parent_or_root(context, parent);
	if (!parent) {
		errno = EINVAL;
		return -1;
	}

	series_stat = calloc(1, sizeof(*series_stat));
	if (!series_stat) {
		errno = ENOMEM;
		return -1;
	}
	series_stat->private = series;

	series->min = ULLONG_MAX;
	error = init_directory(context, &series_stat->root,
			       name, (struct procstat_directory *)parent);
	if (error) {
		free_item(&series_stat->root.base);
		errno = error;
		return -1;
	}

	error = register_u64_series_files(context, series_stat);
	if (error) {
		errno = error;
		goto error_remove_stat;
	}

	struct timespec cur_time;
	if (clock_gettime(SERIES_RESET_CLOCK, &cur_time) == 0) {
		series->reset.last_reset_time = cur_time.tv_sec;
	} else {
		series->reset.last_reset_time = 0;
	}
	series->reset.reset_flag = 0;
	series->reset.reset_interval = 0;

	control[0].object = series_stat;
	control[1].object = series_stat;
	error = procstat_create_simple(context, &series_stat->root.base, control, 2);
	if (error)
		goto error_remove_stat;
	return 0;

error_remove_stat:
	procstat_remove(context, parent);
	return -1;
}

void procstat_u64_series_set_reset_interval(struct procstat_series_u64 *series, int reset_interval)
{
	series->reset.reset_interval = reset_interval;
}

int procstat_create_multiple_u64_series(struct procstat_context *context,
					struct procstat_item *parent,
					struct procstat_series_u64_handle *descriptors,
					size_t series_len)
{
	int i;

	parent = parent_or_root(context, parent);
	if (!parent) {
		errno = EINVAL;
		return -1;
	}

	for (i = 0; i < series_len; ++i) {
		int error;

		error = procstat_create_u64_series(context, parent,
						   descriptors[i].name, descriptors[i].series);
		if (error) {
			errno = error;
			--i;
			goto error_release;
		}
	}
	return 0;
error_release:
	for (;i >= 0; --i)
		procstat_remove_by_name(context, parent, descriptors[i].name);
	return -1;
}

int procstat_create_start_end(struct procstat_context *context,
			      struct procstat_item *parent,
			      struct procstat_start_end_handle *descriptors,
			      size_t descriptors_size)
{
	int i;

	parent = parent_or_root(context, parent);
	if (!parent) {
		errno = EINVAL;
		return -1;
	}

	for (i = 0; i < descriptors_size; ++i) {
		struct procstat_start_end_handle *descriptor = &descriptors[i];
		struct procstat_directory *directory;
		struct procstat_file *file;

		directory = (struct procstat_directory *)procstat_create_directory(context, parent,
										   descriptors[i].name);
		if (!directory) {
			--i;
			goto error_release;
		}

		file = create_file(context, directory, "start", descriptor->start, descriptor->fmt, NULL);
		if (!file)
			goto error_release;

		file = create_file(context, directory, "end", descriptor->end, descriptor->fmt, NULL);
		if (!file) {
			goto error_release;
		}
	}
	return 0;
error_release:
	for (; i >= 0; --i)
		procstat_remove_by_name(context, parent, descriptors[i].name);
	return -1;
}

struct procstat_item *procstat_root(struct procstat_context *context)
{
	assert(context);
	return &context->root.base;
}

struct procstat_context *procstat_context(struct procstat_item *item)
{
	struct procstat_directory *root;

	assert(item);

	if (!item->parent)
		root = (struct procstat_directory *)item;
	else
		for (root = item->parent; root->base.parent != NULL; root = root->base.parent);

	return (struct procstat_context *)root;
}

void fuse_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr, int to_set, struct fuse_file_info *fi)
{
	struct stat stat;
	struct procstat_context *context = request_context(req);
	struct procstat_item *item;

	memset(&stat, 0, sizeof(stat));
	pthread_mutex_lock(&context->global_lock);

	item = fuse_inode_to_item(context, ino);
	if (!item_registered(item)) {
		pthread_mutex_unlock(&context->global_lock);
		fuse_reply_err(req, ENOENT);
		return;
	}

	if (!fuse_inode_to_file(ino)->writer) {
		pthread_mutex_unlock(&context->global_lock);
		fuse_reply_err(req, EPERM);
		return;
	}

	/* only support for truncate as it is needed during write */
	if (to_set != FUSE_SET_ATTR_SIZE) {
		pthread_mutex_unlock(&context->global_lock);
		fuse_reply_err(req, EINVAL);
		return;
	}

	fill_item_stats(context, item, &stat);
	pthread_mutex_unlock(&context->global_lock);
	fuse_reply_attr(req, &stat, 1.0);
}

static void fuse_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct procstat_context *context = request_context(req);
	struct procstat_item *item = fuse_inode_to_item(request_context(req), ino);

	pthread_mutex_lock(&context->global_lock);
	if (item->flags & STATS_ENTRY_FLAG_AGGREGATOR)
		aggregator_release_locked(item, fi);
	if (--item->refcnt == 0)
		free_item(item);
	pthread_mutex_unlock(&context->global_lock);
	if (fi->fh) {
		struct read_struct *fh = (struct read_struct *)fi->fh;
		free(fh->ext);
		free(fh);
	}
	fuse_reply_err(req, 0);
}

static struct fuse_lowlevel_ops fops = {
	.read = fuse_read,
	.lookup = fuse_lookup,
	.forget = fuse_forget,
	.getattr = fuse_getattr,
	.opendir = fuse_opendir,
	.readdir = fuse_readdir,
	.open = fuse_open,
	.write = fuse_write,
	.setattr = fuse_setattr,
	.release = fuse_release,
	.releasedir = fuse_release,
};

#define ROOT_DIR_NAME "."
struct procstat_context *procstat_create(const char *mountpoint)
{
	struct procstat_context *context;
	char *argv[] = {(char *)"stats", (char *)"-o", (char *)"auto_unmount", (char *)mountpoint, NULL};
	struct fuse_args args = FUSE_ARGS_INIT(ARRAY_SIZE(argv)-1, argv);
	char *full_path_mountpoint;
	int error;
	struct fuse_chan *channel;

	error = mkdir(mountpoint, 0755);
	if (error) {
		if (errno != EEXIST)
			return NULL;
	}

	error = fuse_parse_cmdline(&args, &full_path_mountpoint, NULL, NULL);
	if (error) {
		errno = EINVAL;
		return NULL;
	}

	context = calloc(1, sizeof(*context));
	if (!context) {
		errno = ENOMEM;
		return NULL;
	}
	context->mountpoint = full_path_mountpoint;
	context->uid = getuid();
	context->gid = getgid();

	pthread_mutex_init(&context->global_lock, NULL);
	init_directory(context, &context->root, ROOT_DIR_NAME, NULL);

	channel = fuse_mount(context->mountpoint, &args);
	if (!channel) {
		errno = EFAULT;
		goto free_stats;
	}

	context->session = fuse_lowlevel_new(&args, &fops, sizeof(fops), context);
	if (!context->session) {
		errno = EPERM;
		goto free_stats;
	}

	fuse_opt_free_args(&args);
	fuse_session_add_chan(context->session, channel);
	return context;
free_stats:
	procstat_destroy(context);
	return NULL;
}



void procstat_stop(struct procstat_context *context)
{
	struct fuse_session *session;


	assert(context);
	session = context->session;

	if (session) {
		DIR *root_dir;
		// Closing the dir after "fuse_session_exit" causes fuse looper
		// to receive a callback and exit. Otherwise thread running
		// fuse loop can stuck
		root_dir = opendir(context->mountpoint);
		fuse_session_exit(session);
		closedir(root_dir);
	}
}

void procstat_destroy(struct procstat_context *context)
{
	struct fuse_session *session;

	assert(context);
	session = context->session;

	pthread_mutex_lock(&context->global_lock);
	if (session) {
		struct fuse_chan *channel = NULL;

		assert(context->mountpoint);
		fuse_session_exit(session);
		channel = fuse_session_next_chan(session, channel);
		assert(channel);
		fuse_session_remove_chan(channel);

		fuse_unmount(context->mountpoint, channel);
		fuse_session_destroy(session);
	}

	item_put_children_locked(&context->root);
	free(context->mountpoint);
	pthread_mutex_unlock(&context->global_lock);
	pthread_mutex_destroy(&context->global_lock);

	/* debug purposes of use after free*/
	context->mountpoint = NULL;
	context->session = NULL;
	free(context);
}

void procstat_loop(struct procstat_context *context)
{
	fuse_session_loop(context->session);
}

static ssize_t procstat_fmt_u32_percentile(void *object, uint64_t arg, char *buffer, size_t length)
{
	struct procstat_histogram_u32 *series = object;
	unsigned reset;
	uint32_t zero = 0;
	uint32_t *data_ptr = NULL;

	reset = __atomic_load_n(&series->reset.reset_flag, __ATOMIC_ACQUIRE);
	if (reset) {
		data_ptr = &zero;
	} else {
		series->compute_cb(series->histogram, series->count, series->percentile, series->npercentile);
		data_ptr = &series->percentile[arg].value;
	}
	return procstat_format_u32_decimal(data_ptr, 0, buffer, length);
}

void clear_values_histogram(struct procstat_histogram_u32 *series)
{
	series->count = 0;
	series->sum = 0;
	series->last = 0;
	memset(series->histogram, 0, PROCSTAT_PERCENTILE_ARR_NR * sizeof(*series->histogram));
	__atomic_store_n(&series->reset.reset_flag, 0, __ATOMIC_RELEASE);
}

void procstat_histogram_u32_add_point(struct procstat_histogram_u32 *series, uint32_t value)
{
	if (is_reset(&series->reset)) {
		clear_values_histogram(series);
	}

	++series->count;
	series->sum += value;
	series->last = value;

	procstat_hist_add_point(series->histogram, value);
}

enum histogram_u32_series_type{
	HISTOGRAM_SUM = 0,
	HISTOGRAM_COUNT = 1,
	HISTOGRAM_LAST = 2,
	HISTOGRAM_AVG = 3,
	HISTOGRAM_RESET_INTERVAL = 4,
};

static ssize_t histogram_u32_series_read(void *object, uint64_t arg, char *buffer, size_t len)
{
	struct procstat_histogram_u32 *series = object;
	enum histogram_u32_series_type type = arg;
	uint64_t *data_ptr = NULL;
	uint64_t data;
	uint64_t count;

	if (is_reset(&series->reset)) {
		clear_values_histogram(series);
	}

	switch (type) {
	case HISTOGRAM_SUM:
		data_ptr = &series->sum;
		goto write_var;
	case HISTOGRAM_COUNT:
		count = *((volatile uint64_t *)&series->count);
		data_ptr = &series->count;
		goto write_var;
	case HISTOGRAM_LAST:
		data_ptr = &series->last;
		goto write_var;
	case HISTOGRAM_AVG:
		count = *((volatile uint64_t *)&series->count);
		if (!count)
			goto write_zero;
		data = series->sum / count;
		data_ptr = &data;
		goto write_var;
	case HISTOGRAM_RESET_INTERVAL:
		data_ptr = &series->reset.reset_interval;
		goto write_var;
	default:
		return -1;
	}
write_zero:
	return snprintf(buffer, len, "0\n");
write_var:
	return procstat_format_u64_decimal(data_ptr, arg, buffer, len);
}

static ssize_t reset_histogram_u32_series(void *object, uint64_t arg, char *buffer, size_t length)
{
	struct procstat_series *series_stat = object;
	struct procstat_histogram_u32 *series = series_stat->private;
	uint32_t control;

	control = strtoul(buffer, NULL, 10);
	if (control != 1)
		return EINVAL;

	__atomic_store_n(&series->reset.reset_flag, 1, __ATOMIC_RELAXED);
	return 1;
}

static ssize_t reset_interval_histogram_u32_series(void *object, uint64_t arg, char *buffer, size_t length)
{
	struct procstat_series *series_stat = object;
	struct procstat_histogram_u32 *series = series_stat->private;
	int32_t control;

	control = strtoul(buffer, NULL, 10);
	if (control < 0)
		return EINVAL;

	__atomic_store_n(&series->reset.reset_interval, control, __ATOMIC_RELAXED);
	return 1; 
}

int procstat_create_histogram_u32_series(struct procstat_context *context, struct procstat_item *parent,
					 const char *name, struct procstat_histogram_u32 *series)
{
	int i;
	struct procstat_series *series_stat;
	struct procstat_simple_handle control[] = {
		{.name = "reset", .writer = reset_histogram_u32_series},
		{.name = "reset_interval_sec", .writer = reset_interval_histogram_u32_series},
	};
	int error;
	struct procstat_simple_handle descriptors[] = {
		{"sum",    			series, HISTOGRAM_SUM, histogram_u32_series_read},
		{"count",  			series, HISTOGRAM_COUNT, histogram_u32_series_read},
		{"last",   			series, HISTOGRAM_LAST, histogram_u32_series_read},
		{"avg",    			series, HISTOGRAM_AVG, histogram_u32_series_read},
		{"get_reset_interval_sec",  	series, HISTOGRAM_RESET_INTERVAL, histogram_u32_series_read},
	};

	parent = parent_or_root(context, parent);
	if (!parent) {
		errno = EINVAL;
		return -1;
	}

	series_stat = calloc(1, sizeof(*series_stat));
	if (!series_stat) {
		errno = ENOMEM;
		return -1;
	}

	error = init_directory(context, &series_stat->root, name, (struct procstat_directory *)parent);
	if (error) {
		free_item(&series_stat->root.base);
		errno = error;
		return -1;
	}

	series_stat->root.base.flags |= STATS_ENTRY_FLAG_HISTOGRAM;
	series_stat->private = series;
	series->histogram = calloc(PROCSTAT_PERCENTILE_ARR_NR, sizeof(uint32_t));
	if (!series->histogram) {
		errno = ENOMEM;
		goto fail_remove_stat;
	}

	error = procstat_create_simple(context, &series_stat->root.base, descriptors, ARRAY_SIZE(descriptors));
	if (error) {
		errno = error;
		goto fail_remove_stat;
	}

	if (!series->compute_cb)
		series->compute_cb = procstat_percentile_calculate;

	for (i = 0; i < series->npercentile; ++i) {
		char stat_name[100];
		struct procstat_file *file;

		sprintf(stat_name, "%.4g", series->percentile[i].fraction * 100);
		file = create_file(context, (struct procstat_directory *)&series_stat->root.base,
				   stat_name, series, procstat_fmt_u32_percentile, NULL);
		if (!file)
			goto fail_remove_stat;
		file->arg = i;
	}

	struct timespec cur_time;
	if (clock_gettime(SERIES_RESET_CLOCK, &cur_time) == 0) {
		series->reset.last_reset_time = cur_time.tv_sec;
	} else {
		series->reset.last_reset_time = 0;
	}
	series->reset.reset_flag = 0;
	series->reset.reset_interval = 0;

	control[0].object = series_stat;
	control[1].object = series_stat;
	error = procstat_create_simple(context, &series_stat->root.base, control, 2);
	if (error)
		goto fail_remove_stat;

	return 0;

fail_remove_stat:
	procstat_remove(context, &series_stat->root.base);
	return -1;
}

void procstat_histogram_u32_series_set_reset_interval(struct procstat_histogram_u32 *series, int reset_interval)
{
	series->reset.reset_interval = reset_interval;
}

struct procstat_item *procstat_lookup_item(struct procstat_context *context,
		struct procstat_item *parent, const char *name)
{
	struct procstat_item *item;

	parent = parent_or_root(context, parent);
	pthread_mutex_lock(&context->global_lock);

	item = lookup_item_locked((struct procstat_directory *)parent,
				  name, string_hash(name));

	pthread_mutex_unlock(&context->global_lock);

	return item;
}

