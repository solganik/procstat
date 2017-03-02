#define FUSE_USE_VERSION 26
#include <fuse/fuse_lowlevel.h>
#include <stdbool.h>
#include <errno.h>
#include "procstat.h"
#include "list.h"
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>
#include <sys/param.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*a))
#endif

enum {
	STATS_ENTRY_FLAG_REGISTERED  = 1 << 0,
	STATS_ENTRY_FLAG_DIR	     = 1 << 1
};

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

struct procstat_context {
	struct procstat_directory root;
	char *mountpoint;
	struct fuse_session *session;
	gid_t	gid;
	uid_t   uid;
	pthread_mutex_t global_lock;
};

static uint32_t string_hash(const char *string)
{
	uint32_t hash = 0;
	unsigned char *i;

	for (i = (unsigned char*)string; *i; ++i)
	hash = 31 * hash + *i;
	return hash;
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

static bool item_type_directory(struct procstat_item *item)
{
	return (item->flags & STATS_ENTRY_FLAG_DIR);
}

#define INODE_BLK_SIZE 4096
static void fill_item_stats(struct procstat_context *context, struct procstat_item *item, struct stat *stat)
{
	stat->st_uid = context->uid;
	stat->st_gid = context->gid;
	stat->st_ino = (__ino_t)item;

	if (item_type_directory(item)) {
		stat->st_mode = S_IFDIR | 0755;
		stat->st_nlink = root_directory(context, (struct procstat_directory *)item) ? 2 : 1;
		return;
	}
	stat->st_mode = S_IFREG | 0444;
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
	pthread_mutex_unlock(&context->global_lock);
	return 0;
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

static struct fuse_lowlevel_ops fops = {
};

#define ROOT_DIR_NAME "."
struct procstat_context *procstat_create(const char *mountpoint)
{
	struct procstat_context *context;
	char *argv[] = {(char *)"stats", (char *)"-o", (char *)"auto_unmount", (char *)mountpoint};
	struct fuse_args args = FUSE_ARGS_INIT(ARRAY_SIZE(argv), argv);
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

	fuse_session_add_chan(context->session, channel);
	return context;
free_stats:
	procstat_destroy(context);
	return NULL;
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

	free(context->mountpoint);
	pthread_mutex_unlock(&context->global_lock);
	pthread_mutex_destroy(&context->global_lock);

	/* debug purposes of use after free*/
	context->mountpoint = NULL;
	context->session = NULL;
	free(context);
}
