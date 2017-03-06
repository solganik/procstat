# Procstat - library to expose userspace process internal state as FUSE filesystem. 

## Background and motivation
Currently there is no standard way to expose process internal state, counters and statistics for running application.
Some applications following the approach of exposing its state via the external interface they provide for normal operation.
Example of such can be MYSQL that exposes statistics and metrics via SQL language, memcached server which exposes its status 
via specialized "stats" keyword, and others. This approach requires specialized client to be written to gather such statistics, and more specialized tools 
to perform analysis on such statistics.

Procstat library takes advantage of filesystem interface as a mean to expose process statistics and counters. This approach makes rich set of already 
existing text processing tools  such as *grep*, *awk* and others available to perform analysis on exposed statistics and counters.



## Installation
mkdir build; cd build; cmake ../; make && sudo make install

## Getting Started:
In order to use **procstat** you need to *#include "procstat.h"* file in you executable. 

```C
#include <procstat.h>
```

Inside your application create procstat context:
```C

struct procstat_context *context;
context = procstat_create(<path to mountpoint>);
```

Next create a dedicated thread for fuse (which is used by procstat to expose statistics as a file system)
Then run:
```C
procstat_loop(context); 
```


## Single-value statistics
It is important to understand that counters are part of you application, hence its validity must be provided by the application from the moment
statistics is registered till it unregistered.
In order to register single value counter 

```C
u64 counter = 0;
procstat_create_u64(context, NULL, "my-counter", &counter);
```

This will expose value of "counter" object as 'my-counter' file under <mount_point>
In order to unregister this counter add the following line 

```C
procstat_remove_by_name(context, NULL, "my-counter");
```

## Creating directory hierarchy
Directory hierarchy can be created to organize statistics and counters according to application requirements.
In order ot create directory do the following:

```C
struct procstat_item *outer_dir, *inner_dir;
outer_dir = procstat_create_directory(context, NULL, "outer-directory");
inner_dir = procstat_create_directory(context, outer_dir, "inner-directory");
procstat_create_u64(context, inner_dir, "my-counter", &counter);
```

This will expose counter value as file <mountpoint>/outer-directory/inner-directory/my-counter


## Advanced Usage
FIXME: add advanced usage examples...