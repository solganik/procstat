#ifndef _PROCSTAT_H_
#define _PROCSTAT_H_

#include <stdint.h>
#include <linux/types.h>
#include <stddef.h>
#include <unistd.h>
#include <stdio.h>

struct procstat_context;

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
 * @brief blocking method to handle statstics operations. This must be run from
 * dedicated thread exactly once.
 */
void procstat_loop(struct procstat_context *context);

#endif