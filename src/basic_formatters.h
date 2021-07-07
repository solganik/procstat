#include "procstat.h"

#ifndef _PROCSTAT_BASIC_FORMATTERS_H_
#define _PROCSTAT_BASIC_FORMATTERS_H_

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

#endif