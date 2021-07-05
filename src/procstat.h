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


#ifndef _PROCSTAT_H_
#define _PROCSTAT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <linux/types.h>
#include <stddef.h>
#include <unistd.h>
#include <stdio.h>
#include "percentile.h"

struct procstat_context;
struct procstat_item;

/**
 * @brief: stats formatter method
 * @param object registered with statistics
 * @param arg registered with statistics
 * @param buffer to format object to
 * @param lenght of buffer
 * @param offset to return from beginning of buffer. This can be ignored in case statistics string
 * is large > 4K
 */
typedef ssize_t (*procstats_formatter)(void *object, uint64_t arg, char *buffer, size_t length);

/**
 * @brief registration parameter for simple value statistics
 * @name of statistics
 * @bject to be passed to the formatter.
 * @arg to be passed together with object to formatter
 * @fmt data formatter
 */
struct procstat_simple_handle {
	const char 	    *name;
	void 	 	    *object;
	uint64_t 	    arg;
	procstats_formatter fmt;
	procstats_formatter writer;
};


/**
 * @brief create statstics context and mount it on running machine under @mountpoint.
 * @mountpoint root directory for statistics will be created in case it does not exists.
 * @return context to be used for all statistics operations. or NULL in case of error. errno will be
 * set accordingly
 */
struct procstat_context *procstat_create(const char *mountpoint);

/**
 * @brief signal the loop to exit
 */
void procstat_stop(struct procstat_context *context);

/**
 * @brief unregister and destroy all registered statistics
 */
void procstat_destroy(struct procstat_context *context);

/**
 * @return get root item under which statstics can be registered
 */
struct procstat_item *procstat_root(struct procstat_context *context);


/**
 * @return context that @item is attached to in hierarchy of procstat
 */
struct procstat_context *procstat_context(struct procstat_item *item);

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

/*
 * @brief return an item with @name under @parent directory
 * @context statistics context
 * @parent directory under which to lookup an item with @name, in case parent is NULL
 * 	    @name will be lookup under root directory
 * @name of the item to lookup
 * @return the found item NULL, in case of failure and errno will be set accordingly
 */
struct procstat_item *procstat_lookup_item(struct procstat_context *context,
		struct procstat_item *parent, const char *name);


/**
 * @brief - increments refcount for @item
 * @context -statistics context
 * @item - object to take reference for
 */
void procstat_refget(struct procstat_context *context, struct procstat_item *item);

/**
 * @brief - decrements refcount for @item, when reaches 0 item is released
 * @context -statistics context
 * @item - object to take reference for
 */
void procstat_refput(struct procstat_context *context, struct procstat_item *item);

/**
 * @brief creates counter, which will be exposed as @name under @parent dictory.
 * @return 0 on success, -1  in case of failure and errno will be set accordingly
 */
int procstat_create_simple(struct procstat_context *context,
			   struct procstat_item *parent,
			   struct procstat_simple_handle *descriptors,
			   size_t descriptors_len);

/**
 * @brief creates a file that on read outputs the contents of the entire directory tree.
 * @return 0 on success, -1  in case of failure and errno will be set accordingly
 */
int procstat_create_aggregator(struct procstat_context *context,
			   struct procstat_item *parent,
			   const char *name);


#define DEFINE_PROCSTAT_FORMATTER(__type, __fmt, __fmt_name)\
static inline ssize_t procstat_format_ ## __type ##_## __fmt_name(void *object, uint64_t arg, char *buffer, size_t len)\
{\
	return snprintf(buffer, len, __fmt, *((__type *)object));\
}\

#define DEFINE_PROCSTAT_WRITER(__type, __fmt, __fmt_name)\
static inline ssize_t procstat_write_ ## __type ##_## __fmt_name(void *object, uint64_t arg, char *buffer, size_t size)\
{\
	return sscanf(buffer, __fmt, ((__type *)object));\
}\


#ifndef u64
#define u64 uint64_t
#endif

#ifndef u32
#define u32 uint32_t
#endif


DEFINE_PROCSTAT_FORMATTER(u64, "%lu\n", decimal);
DEFINE_PROCSTAT_FORMATTER(u64, "%lx\n", hex);
DEFINE_PROCSTAT_FORMATTER(u64, "0x%lx\n", address);
DEFINE_PROCSTAT_FORMATTER(u32, "%u\n", decimal);
DEFINE_PROCSTAT_FORMATTER(u32, "%x\n", hex);
DEFINE_PROCSTAT_FORMATTER(int, "%d\n", decimal);

DEFINE_PROCSTAT_WRITER(u64, "%lu\n", decimal);
DEFINE_PROCSTAT_WRITER(u32, "%u\n", decimal);
DEFINE_PROCSTAT_WRITER(int, "%d\n", decimal);

#define DEFINE_PROCSTAT_SIMPLE_ATTRIBUTE(__type)\
static inline int procstat_create_ ## __type(struct procstat_context *context, struct procstat_item *parent, const char *name, __type *object)\
{\
	struct procstat_simple_handle descriptor = {name, object, 0UL, procstat_format_ ## __type ## _decimal, NULL};\
	\
	return procstat_create_simple(context, parent, &descriptor, 1);\
}\

DEFINE_PROCSTAT_SIMPLE_ATTRIBUTE(u32);
DEFINE_PROCSTAT_SIMPLE_ATTRIBUTE(u64);
DEFINE_PROCSTAT_SIMPLE_ATTRIBUTE(int);


#define DEFINE_PROCSTAT_SIMPLE_PARAMETER(__type)\
static inline int procstat_create_ ## __type ## _parameter(struct procstat_context *context, struct procstat_item *parent, const char *name, __type *object)\
{\
	struct procstat_simple_handle descriptor = {name, object, 0UL, procstat_format_ ## __type ## _decimal, procstat_write_ ## __type ## _decimal};\
	\
	return procstat_create_simple(context, parent, &descriptor, 1);\
}\

DEFINE_PROCSTAT_SIMPLE_PARAMETER(int);
DEFINE_PROCSTAT_SIMPLE_PARAMETER(u32);
DEFINE_PROCSTAT_SIMPLE_PARAMETER(u64);
/**
 * @brief defines formatter with getter method. This can be used to provide standard POD formatting with custom
 * get function to fetch object value
 */
#define DEFINE_PROCSTAT_CUSTOM_FORMATTER(name, getter_function, __type, __fmt)\
static inline ssize_t procstat_format_ ## __type ##_## name(void *object, uint64_t arg, char *buffer, size_t length)\
{\
	__type out;\
	\
	getter_function((__type *)object, arg, &out);\
	return snprintf(buffer, length, __fmt, out);\
}\

/**
 * @brief registration parameter for start end statistics. This is equivalent to creating a directory
 * with @name and registering start and end files bounded to @start and @end
 * @name of statistics
 * @start object to be exposed as "start" filename under @name statistic
 * @end object to be exposed as "end" filename under @name statistic
 * @fmt data formatter
 */
struct procstat_start_end_handle {
	const char *name;
	void *start;
	void *end;
	procstats_formatter fmt;
};


struct procstat_start_end_u32 {
	uint32_t start;
	uint32_t end;
};

struct procstat_start_end_u64 {
	uint64_t start;
	uint64_t end;
};

/**
 * @brief create start end statistics
 * @context of the stats
 * @parent under which to create start end statistics
 * @descriptors array of descriptors for start end statistics
 * @descriptors_len of the array
 */
int procstat_create_start_end(struct procstat_context 	       *context,
			      struct procstat_item 	       *parent,
			      struct procstat_start_end_handle *descriptors,
			      size_t 				descriptors_len);

/**
 * @brief shortcut to create handle for u32 start end stat
 */
#define procstat_start_end_u32_handle(name, start_end)\
	(struct procstat_start_end_handle){name, &start_end.start, &start_end.end, procstat_format_u32_decimal}

/**
 * @brief shortcut to create handle for u64 start end stat
 */
#define procstat_start_end_u64_handle(name, start_end)\
	(struct procstat_start_end_handle){name, &start_end.start, &start_end.end, procstat_format_u64_decimal}

struct reset_info {
	uint64_t reset_interval;
	uint64_t last_reset_time;
	unsigned reset_flag;
};

/**
 * @brief registration parameter for series statistics. statistical analysis will be performed on values
 * submitted as series point. mean and variance will be calculated upon even point submittion
 * stddev and average are additionally exposed and calculated upen request.
 * @name of statistics
 * @object to be passed to the formatter.
 * @fmt data formatter
 */
struct procstat_series_u64_handle {
	const char *name;
	struct procstat_series_u64 *series;
};


struct procstat_series_u64 {
	uint64_t 		sum;
	uint64_t 		count;
	uint64_t 		min;
	uint64_t 		max;
	uint64_t 		last;
	uint64_t 		mean;
	uint64_t 		aggregated_variance;
	struct reset_info 	reset;
};


/**
 * @brief callback to calculate histogram values
 */
typedef void (*percentiles_calculator)(uint32_t *histogram,
					uint64_t samples_count,
					struct procstat_percentile_result *result,
					unsigned result_len);

#define MAX_SUPPORTED_PERCENTILE 20
struct procstat_histogram_u32 {
	uint64_t 				sum;
	uint64_t 				count;
	uint64_t 				last;
	int 					npercentile;
	struct procstat_percentile_result	percentile[MAX_SUPPORTED_PERCENTILE];
	uint32_t 				*histogram;
	percentiles_calculator 			compute_cb;
	struct reset_info 			reset;
};

/**
 * @brief create series statistics.
 */
int procstat_create_u64_series(struct procstat_context *context, struct procstat_item *parent,
			       const char *name, struct procstat_series_u64 *series);


/**
 * @brief create multiple series statstics
 */
int procstat_create_multiple_u64_series(struct procstat_context *context,
					struct procstat_item *parent,
					struct procstat_series_u64_handle *descriptors,
					size_t series_len);

/**
 * @brief add points to series statistics
 */
void procstat_u64_series_add_point(struct procstat_series_u64 *series, uint64_t value);

void procstat_u64_series_set_reset_interval(struct procstat_series_u64 *series, int reset_interval);

int procstat_create_histogram_u32_series(struct procstat_context *context, struct procstat_item *parent,
					 const char *name, struct procstat_histogram_u32 *series);

void procstat_histogram_u32_add_point(struct procstat_histogram_u32 *series, uint32_t value);

void procstat_histogram_u32_series_set_reset_interval(struct procstat_histogram_u32 *series, int reset_interval);

#ifdef __cplusplus
}
#endif

#endif
