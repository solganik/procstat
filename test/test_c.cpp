#include <iostream>
#include "gtest/gtest.h"
#include "../src/procstat.h"
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem.hpp>
#include <unordered_map>
#include "utils.hpp"
#include <boost/format.hpp>

void* fuse_loop(void *arg)
{
	struct procstat_context *ctx = (struct procstat_context *)arg;
	procstat_loop(ctx);
	return NULL;
}

class ProcstatTest: public ::testing::Test {
public:
	void SetUp() {
		context = procstat_create(mount_name().c_str());
		pthread_create(&looper, NULL, fuse_loop, context);
	}

	void TearDown() {
		procstat_stop(context);
		pthread_join(looper, NULL);
		procstat_destroy(context);
	}
	pthread_t looper;
	struct procstat_context *context;
};

TEST_F (ProcstatTest, test_create_remove) {
	int value = 0;
	int error;
	error = procstat_create_int_parameter(context, NULL, "param", &value);
	ASSERT_TRUE(!error);


	ASSERT_EQ(0, read_stat_file<int>(mount_name() + "/param"));
	value = 10;
	ASSERT_EQ(10, read_stat_file<int>(mount_name() + "/param"));
	procstat_remove_by_name(context, NULL, "param");
}

TEST_F (ProcstatTest, test_dirs_cannot_contain_slash) {
	struct procstat_item *item1, *item2;
	item1 = procstat_create_directory(context, procstat_root(context), "start/end");
	ASSERT_FALSE(item1);
}

TEST_F (ProcstatTest, test_create_dirs) {
	struct procstat_item *item;
	struct procstat_item *item2;

	item = procstat_create_directory(context, procstat_root(context), "dir1");
	ASSERT_TRUE(item);

	item2 = procstat_create_directory(context, procstat_root(context), "dir1");
	ASSERT_FALSE(item2);
	ASSERT_EQ(errno, EEXIST);

	procstat_remove(context, item);

	item = procstat_create_directory(context, NULL, "dir1");
	ASSERT_TRUE(item);

	procstat_remove_by_name(context, NULL, "dir1");

	item = procstat_create_directory(context, procstat_root(context), "dir1");
	ASSERT_TRUE(item);

	item = procstat_create_directory(context, NULL, "veryveryverty-longlonglongnamemamemeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
	ASSERT_TRUE(item);

	procstat_remove(context, item);
	item = procstat_create_directory(context, NULL, "veryveryverty-longlonglongnamemamemeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
	ASSERT_TRUE(item);
}


TEST_F (ProcstatTest, test_create_invalid_filename) {
	u64 dummy;
	int error;

	error = procstat_create_u64(context, NULL, "value_90%", &dummy);
	ASSERT_TRUE(error);
}


TEST_F (ProcstatTest, test_create_multiple_dirs_and_files) {
	struct procstat_item *item;
	char buffer[100];
	int i, j, k;
	int error;
	uint32_t values_32[10] = {0};

	for ( i = 0; i < 10; ++i) {
		struct procstat_item *outer;
		sprintf(buffer, "outer-%d", i);

		outer = procstat_create_directory(context, NULL, buffer);
		assert(outer);
		assert(procstat_context(outer) == context);
		for (j = 0; j < 10; ++j) {
			struct procstat_item *inner;
			sprintf(buffer, "inner-%d", j);
			inner = procstat_create_directory(context, outer, buffer);
			assert(inner);
			assert(procstat_context(inner) == context);
			for (k = 0; k < 10; ++k) {
				sprintf(buffer, "value-%d", k);
				error = procstat_create_u32(context, inner, buffer , &values_32[k]);
				assert(!error);
			}
		}
	}

	for (int i = 0; i < 10; ++i) {
		for (int j = 0; j < 10;  ++j) {
			for (int k = 0; k < 10; ++k) {
				auto stat_name = (boost::format("/outer-%1%/inner-%2%/value-%3%") % i % j % k).str();
				ASSERT_EQ(0, read_stat_file<int>(mount_name() + stat_name));
			}
		}
	}


	for (int i = 0; i < 10; ++i) {
		values_32[i]++;
	}

	for (int i = 0; i < 10; ++i) {
		for (int j = 0; j < 10;  ++j) {
			for (int k = 0; k < 10; ++k) {
				auto stat_name = (boost::format("/outer-%1%/inner-%2%/value-%3%") % i % j % k).str();
				ASSERT_EQ(1, read_stat_file<int>(mount_name() + stat_name));
			}
		}
	}


	printf("Lookup directory named outer-0 under root\n");
	item = procstat_lookup_item(context, NULL, "outer-0");
	assert(item);
	printf("Found directory outer-0!\n\n");

	printf("Lookup directory named inner-3 under outer-0\n");
	item = procstat_lookup_item(context, item, "inner-3");
	assert(item);
	printf("Found directory inner-3!\n\n");

	printf("Lookup file named value-6 under inner-3\n");
	item = procstat_lookup_item(context, item, "value-6");
	assert(item);
	printf("Found file value-6!\n\n");

	printf("Delete several directories outer-0s\n");
	procstat_remove_by_name(context, NULL, "outer-0");
	printf("Deleted outer-0 Watch values under outer-0 directory should not be here!!!\n");
	ASSERT_FALSE(boost::filesystem::exists(mount_name() + "/outer-0"));

	item = procstat_create_directory(context, NULL, "outer-0");
	assert(item);
	ASSERT_TRUE(boost::filesystem::exists(mount_name() + "/outer-0"));
}



static void fetch_getter(uint16_t *object, uint64_t arg, uint16_t *out)
{
	*out = *object;
}


DEFINE_PROCSTAT_FORMATTER(uint16_t, "%u\n", decimal);
DEFINE_PROCSTAT_SIMPLE_ATTRIBUTE(uint16_t);
DEFINE_PROCSTAT_CUSTOM_FORMATTER(fetch, fetch_getter, uint16_t, "%u");

TEST_F (ProcstatTest, test_create_custom_getter_and_formatter) {
	uint16_t values_16[2] = {1,2};

	struct procstat_item *item;
	struct procstat_simple_handle descriptors[] = {{"val_16_0", &values_16[0], 0, procstat_format_uint16_t_fetch},
												   {"val_16_1", &values_16[1], 0, procstat_format_uint16_t_fetch}};
	int error;

	item = procstat_create_directory(context, NULL, "multiple-simple");
	ASSERT_TRUE(item);
	error = procstat_create_simple(context, item, descriptors, 2);
	ASSERT_FALSE(error);

	ASSERT_EQ(1, read_stat_file<uint16_t >(mount_name() + "/multiple-simple/val_16_0"));
	ASSERT_EQ(2, read_stat_file<uint16_t >(mount_name() + "/multiple-simple/val_16_1"));

	error = procstat_create_uint16_t(context, NULL, "val16_special", &values_16[0]);
	ASSERT_FALSE(error);
	ASSERT_EQ(1, read_stat_file<uint16_t >(mount_name() + "/val16_special"));
}



TEST_F (ProcstatTest, test_create_multiple_start_end_stats)
{
	struct procstat_item *item;
	struct procstat_start_end_u64 start_end_u64[] = {{1,2},
													 {3,4}};

	struct procstat_start_end_u32 start_end_u32[] = {{5,6},
													 {7,8}};

	struct procstat_start_end_handle descriptors[] = {procstat_start_end_u64_handle("s1", start_end_u64[0]),
													  procstat_start_end_u64_handle("s2", start_end_u64[1]),
													  procstat_start_end_u32_handle("s3", start_end_u32[0]),
													  procstat_start_end_u32_handle("s4", start_end_u32[1])};

	int error;

	item = procstat_create_directory(context, NULL, "start-end");
	ASSERT_TRUE(item);

	error = procstat_create_start_end(context, item, descriptors, 4);
	ASSERT_FALSE(error);

	error = procstat_create_start_end(context, item, descriptors, 4);
	ASSERT_TRUE(error) << "Must not succeed as stats already registered";

	printf("Verify stats exists\n");
	ASSERT_EQ(1, read_stat_file<int >(mount_name() + "/start-end/s1/start"));
	ASSERT_EQ(2, read_stat_file<int >(mount_name() + "/start-end/s1/end"));

	ASSERT_EQ(3, read_stat_file<int >(mount_name() + "/start-end/s2/start"));
	ASSERT_EQ(4, read_stat_file<int >(mount_name() + "/start-end/s2/end"));

	ASSERT_EQ(5, read_stat_file<int >(mount_name() + "/start-end/s3/start"));
	ASSERT_EQ(6, read_stat_file<int >(mount_name() + "/start-end/s3/end"));

	ASSERT_EQ(7, read_stat_file<int >(mount_name() + "/start-end/s4/start"));
	ASSERT_EQ(8, read_stat_file<int >(mount_name() + "/start-end/s4/end"));

	procstat_remove(context, item);

	printf("Verify directory does not exists\n");
	ASSERT_FALSE(boost::filesystem::exists(mount_name() + "/start-end"));
}


TEST_F (ProcstatTest, test_multiple_series)
{
	struct procstat_item *item;
	struct procstat_series_u64 series[10];

	struct procstat_series_u64_handle des[] = { {"s1", &series[0]},
												{"s2", &series[1]},
												{"s3", &series[2]},
												{"s4", &series[3]},
												{"s5", &series[4]},
												{"s6", &series[5]},
												{"s7", &series[6]},
												{"s8", &series[7]},
												{"s9", &series[8]},
												{"s10", &series[9] } };

	memset(series, 0, sizeof(series));
	int error;


	item = procstat_create_directory(context, NULL, "series");
	ASSERT_TRUE(item);

	error = procstat_create_multiple_u64_series(context, item, des, 10);
	ASSERT_TRUE(!error);

	read_series(mount_name() + "/series/s1");
	read_series(mount_name() + "/series/s2");
	read_series(mount_name() + "/series/s3");
	read_series(mount_name() + "/series/s4");
	read_series(mount_name() + "/series/s5");
	read_series(mount_name() + "/series/s6");
	read_series(mount_name() + "/series/s7");
	read_series(mount_name() + "/series/s8");
	read_series(mount_name() + "/series/s9");

	procstat_remove(context, item);

	ASSERT_FALSE(boost::filesystem::exists(mount_name() + "/series/s1"));
	ASSERT_FALSE(boost::filesystem::exists(mount_name() + "/series/s2"));
	ASSERT_FALSE(boost::filesystem::exists(mount_name() + "/series/s3"));
	ASSERT_FALSE(boost::filesystem::exists(mount_name() + "/series/s4"));
	ASSERT_FALSE(boost::filesystem::exists(mount_name() + "/series/s5"));
	ASSERT_FALSE(boost::filesystem::exists(mount_name() + "/series/s6"));
	ASSERT_FALSE(boost::filesystem::exists(mount_name() + "/series/s7"));
	ASSERT_FALSE(boost::filesystem::exists(mount_name() + "/series/s8"));
	ASSERT_FALSE(boost::filesystem::exists(mount_name() + "/series/s9"));


}

static inline unsigned long long rdtsc(void)
{
	unsigned long low, high;

	asm volatile("rdtsc" : "=a" (low), "=d" (high));

	return ((low) | (high) << 32);
}

TEST_F (ProcstatTest, test_time_series)
{
	struct procstat_item *item;
	struct procstat_series_u64 series;
	uint64_t start;
	int i;
	int error;

	memset(&series, 0, sizeof(series));
	item = procstat_create_directory(context, NULL, "time_series");
	ASSERT_TRUE(item);

	error = procstat_create_u64_series(context, item, "time1", &series);
	ASSERT_FALSE(error);

	printf("Going to submit several timepoints \n");
	for (i = 0; i < 20; ++i) {
		start = rdtsc();
		usleep(1000 * 100);
		procstat_u64_series_add_point(&series, rdtsc() - start);
	}

	auto s1 = read_series(mount_name() + "/time_series/time1");
	ASSERT_EQ(s1["count"], 20);
	ASSERT_EQ(s1["count"], series.count);
	ASSERT_EQ(s1["sum"], series.sum);
	ASSERT_EQ(s1["min"], series.min);
	ASSERT_EQ(s1["max"], series.max);
	ASSERT_EQ(s1["last"], series.last);
	ASSERT_EQ(s1["mean"], series.mean);

	printf("Now reset series \n");
	write_to_stat_file(mount_name() + "/time_series/time1/reset", 1);

	s1 = read_series(mount_name() + "/time_series/time1");
	ASSERT_EQ(s1["count"], 0);

	for (i = 0; i < 200; ++i) {
		start = rdtsc();
		usleep(1000);
		procstat_u64_series_add_point(&series, rdtsc() - start);
	}

	s1 = read_series(mount_name() + "/time_series/time1");
	ASSERT_EQ(s1["count"], 200);
	ASSERT_EQ(s1["count"], series.count);
	ASSERT_EQ(s1["sum"], series.sum);
	ASSERT_EQ(s1["min"], series.min);
	ASSERT_EQ(s1["max"], series.max);
	ASSERT_EQ(s1["last"], series.last);
	ASSERT_EQ(s1["mean"], series.mean);

	procstat_remove(context, item);
	ASSERT_FALSE(boost::filesystem::exists(mount_name() + "/time_series/time1"));
}




TEST_F (ProcstatTest, test_histogram)
{
	struct procstat_histogram_u32 series = {};

	series.percentile[0].fraction = 0.1f;
	series.percentile[1].fraction = 0.6f;
	series.percentile[2].fraction = 0.9f;
	series.percentile[3].fraction = 0.99f;
	series.percentile[4].fraction = 0.9999f;
	series.npercentile = 5;
	int error;
	int i;

	error = procstat_create_histogram_u32_series(context, NULL, "hist", &series);
	assert(!error);

	for (i = 0; i < 1000000; ++i) {
		procstat_histogram_u32_add_point(&series, i);
	}

	auto hist_values = read_histogram(mount_name() + "/hist", {"10", "60", "90", "99", "99.99"});
	EXPECT_EQ(hist_values["count"], 1000000);
	EXPECT_EQ(hist_values["sum"], 499999500000);
	EXPECT_EQ(hist_values["10"], 99840);
	EXPECT_EQ(hist_values["60"], 602112);
	EXPECT_EQ(hist_values["90"], 897024);
	EXPECT_EQ(hist_values["99"], 987136);
	EXPECT_EQ(hist_values["99.99"], 1003520);

	printf("Now reset Histogram, and observe zeroed values\n");
	write_to_stat_file(mount_name() + "/hist/reset", 1);
	hist_values = read_histogram(mount_name() + "/hist", {"10", "60", "90", "99", "99.99"});
	EXPECT_EQ(hist_values["count"], 0);

	for (i = 0; i < 1000000; ++i) {
		procstat_histogram_u32_add_point(&series, i);
	}

	hist_values = read_histogram(mount_name() + "/hist", {"10", "60", "90", "99", "99.99"});
	EXPECT_EQ(hist_values["count"], 1000000);

	printf("removing histogram\n");
	procstat_remove_by_name(context, NULL, "hist");
	ASSERT_FALSE(boost::filesystem::exists(mount_name() + "/hist"));

}

static ssize_t procstat_control_set_u64(void *object, uint64_t arg, char *buffer, size_t length)
{
	uint64_t *ptr = (uint64_t *)object;
	uint64_t value;

	value = strtoul(buffer, NULL, 10);
	if (errno)
		return errno;
	*ptr = value;
	return 1;
}


TEST_F (ProcstatTest, test_control)
{
	struct procstat_item *item;
	int error;
	struct procstat_simple_handle control = {.name="set", .writer = procstat_control_set_u64};

	item = procstat_create_directory(context, procstat_root(context), "with_control");
	assert(item);
	uint64_t counter;

	error = procstat_create_u64(context, item, "count", &counter);
	assert(!error);

	control.object = &counter;

	error = procstat_create_simple(context, item, &control, 1);
	assert(!error);

	printf("Write to control and read value");
	printf("Press enter To inc values\n");

	procstat_remove(context, item);
}


