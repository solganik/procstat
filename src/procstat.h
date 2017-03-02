#ifndef _PROCSTAT_H_
#define _PROCSTAT_H_

#include <stdint.h>
#include <linux/types.h>
#include <stddef.h>
#include <unistd.h>
#include <stdio.h>

struct procstat_context;
struct procstat_item;

/**
 * @brief create statstics context and mount it on running machine under @mountpoint.
 * @mountpoint root directory for statistics will be created in case it does not exists.
 * @return context to be used for all statistics operations. or NULL in case of error. errno will be
 * set accordingly
 */
struct procstat_context *procstat_create(const char *mountpoint);

/**
 * @brief unregister and destroy all registered statistics
 */
void procstat_destroy(struct procstat_context *context);

/**
 * @return get root item under which statstics can be registered
 */
struct procstat_item *procstat_root(struct procstat_context *context);

/**
 * @brief blocking method to handle statstics operations. This must be run from
 * dedicated thread exactly once.
 */
void procstat_loop(struct procstat_context *context);

/**
 * @brief create directory @name under @parent directory
 * @context statistics context
 * @parent directory under which to create directory with @name, in case parent is NULL
 * 	    @name will be created under root directory
 * @name of the directory to create.
 * @return created directory or NULL in case of failure and errno will be set accordingly
 */
struct procstat_item *procstat_create_directory(struct procstat_context *context,
					  	struct procstat_item *parent,
						const char *name);

/**
 * @brief removes statistics item previosly created with any of creation methods
 */
void procstat_remove(struct procstat_context *context, struct procstat_item *item);

/**
 * @brief searches for @name item under @parent directory and removes it
 * @return 0 in case of success or error code
 */
int procstat_remove_by_name(struct procstat_context *context, struct procstat_item *parent, const char *name);


#endif