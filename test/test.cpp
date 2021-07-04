#include "gtest/gtest.h"
#include "../src/procstat.hpp"
#include "utils.hpp"
#define GTEST_COUT std::cerr << "[          ] [ INFO ]"

TEST(CppTest, start_stop_no_autostart)
{
	procstat::context ctx(mount_name(), false);
	ctx.start();
	EXPECT_TRUE(fs::is_directory(mount_name()));
	ctx.stop();
	ctx.start();
	EXPECT_TRUE(fs::is_directory(mount_name()));
	ctx.stop();
}


TEST(CppTest, autostart)
{
	procstat::context ctx(mount_name(), true);
	EXPECT_TRUE(fs::is_directory(mount_name()));
	ctx.stop();
}

TEST(CppTest, stop_start_with_registered_value)
{
	procstat::context ctx(mount_name());
	int stat1 = 4;
	ctx.root().create("stat1", stat1);
	EXPECT_EQ(4, read_stat_file<int>(mount_name() + "/stat1"));
	ctx.stop();
	ctx.start();
	EXPECT_EQ(4, read_stat_file<int>(mount_name() + "/stat1"));
	ctx.stop();
}


TEST(procstat, test_simple_value_read)
{
	procstat::context ctx(mount_name());
	int stat1 = 4;
	float stat2 = 5.1;
	uint64_t stat3 = 6;
	uint32_t stat4 = 7;
	int64_t stat5 = -1;

	ctx.root().create("stat1", stat1);
	ctx.root().create("stat2", stat2);
	ctx.root().create("stat3", stat3);
	ctx.root().create("stat4", stat4);
	ctx.root().create("stat5", stat5);

	EXPECT_EQ(4, read_stat_file<int>(mount_name() + "/stat1"));
	EXPECT_FLOAT_EQ(5.1, read_stat_file<float>(mount_name() + "/stat2"));
	EXPECT_EQ(6, read_stat_file<uint64_t>(mount_name() + "/stat3"));
	EXPECT_EQ(7, read_stat_file<uint32_t>(mount_name() + "/stat4"));
	EXPECT_EQ(-1, read_stat_file<int64_t>(mount_name() + "/stat5"));


	stat1 = 20;
	stat2 = 6.1;
	stat3 = 0;
	stat4 = 9;
	stat5 = -100;
	EXPECT_EQ(20, read_stat_file<int>(mount_name() + "/stat1"));
	EXPECT_FLOAT_EQ(6.1, read_stat_file<float>(mount_name() + "/stat2"));
	EXPECT_EQ(0, read_stat_file<uint64_t>(mount_name() + "/stat3"));
	EXPECT_EQ(9, read_stat_file<uint32_t>(mount_name() + "/stat4"));
	EXPECT_EQ(-100, read_stat_file<int64_t>(mount_name() + "/stat5"));

	ctx.stop();
}


TEST(procstat, test_start_end)
{
	procstat::context ctx(mount_name());
	std::pair<int, int> p;
	auto registration = ctx.root().create_start_end("start-1", p);
	EXPECT_EQ(0, read_stat_file<int>(mount_name() + "/start-1/start"));
	EXPECT_EQ(0, read_stat_file<int>(mount_name() + "/start-1/end"));

	p.first = 1;
	p.second = 2;

	EXPECT_EQ(1, read_stat_file<int>(mount_name() + "/start-1/start"));
	EXPECT_EQ(2, read_stat_file<int>(mount_name() + "/start-1/end"));
}


TEST(procstat, test_start_end_destruct_via_registration)
{
	procstat::context ctx(mount_name());
	std::pair<int, int> p{1,2};
	{
		auto registration = ctx.root().create_start_end("start-1", p);
		EXPECT_EQ(1, read_stat_file<int>(mount_name() + "/start-1/start"));
		EXPECT_EQ(2, read_stat_file<int>(mount_name() + "/start-1/end"));
	}

	// Stat should not exists anymore as registration destroyed it
	ASSERT_FALSE(boost::filesystem::exists(mount_name() + "/start-1"));
}


TEST(procstat, test_start_end_registration_detach)
{
	procstat::context ctx(mount_name());
	std::pair<int, int> p{1,2};
	{
		auto registration = ctx.root().create_start_end("start-1", p);
		EXPECT_EQ(1, read_stat_file<int>(mount_name() + "/start-1/start"));
		EXPECT_EQ(2, read_stat_file<int>(mount_name() + "/start-1/end"));
		registration.detatch();
	}

	// File and values should be still accessible
	ASSERT_TRUE(boost::filesystem::exists(mount_name() + "/start-1"));
	EXPECT_EQ(1, read_stat_file<int>(mount_name() + "/start-1/start"));
	EXPECT_EQ(2, read_stat_file<int>(mount_name() + "/start-1/end"));
}


TEST(procstat, test_start_end_registration_detach_manual_unregistry)
{
	procstat::context ctx(mount_name());
	std::pair<int, int> p{1,2};
	{
		auto registration = ctx.root().create_start_end("start-1", p);
		EXPECT_EQ(1, read_stat_file<int>(mount_name() + "/start-1/start"));
		EXPECT_EQ(2, read_stat_file<int>(mount_name() + "/start-1/end"));
		registration.detatch();
	}

	// File and values should be still accessible
	ASSERT_TRUE(boost::filesystem::exists(mount_name() + "/start-1"));
	EXPECT_EQ(1, read_stat_file<int>(mount_name() + "/start-1/start"));
	EXPECT_EQ(2, read_stat_file<int>(mount_name() + "/start-1/end"));

	ctx.root().delete_child("start-1");
	ASSERT_FALSE(boost::filesystem::exists(mount_name() + "/start-1"));
}


TEST(procstat, test_series_count)
{
	auto series_path = mount_name() + "/series1";
	procstat::context ctx(mount_name());

	std::unique_ptr<procstat::series> series1 = ctx.root().create_series("series1");
	auto values = read_series(series_path);
	EXPECT_EQ(values["sum"], 0);
	EXPECT_EQ(values["count"], 0);

	series1->add_point(1);

	values = read_series(series_path);
	EXPECT_EQ(values["sum"], 1);
	EXPECT_EQ(values["count"], 1);
	EXPECT_EQ(values["min"], 1);
	EXPECT_EQ(values["max"], 1);
	EXPECT_EQ(values["avg"], 1);
	EXPECT_EQ(values["stddev"], 0);


	series1->add_point(3);
	values = read_series(series_path);
	EXPECT_EQ(values["sum"], 4);
	EXPECT_EQ(values["count"], 2);
	EXPECT_EQ(values["min"], 1);
	EXPECT_EQ(values["max"], 3);
	EXPECT_EQ(values["avg"], 2);
	EXPECT_EQ(values["stddev"], 2);


	series1->add_point(10);
	values = read_series(series_path);
	EXPECT_EQ(values["sum"], 14);
	EXPECT_EQ(values["count"], 3);
	EXPECT_EQ(values["min"], 1);
	EXPECT_EQ(values["max"], 10);
	EXPECT_EQ(values["avg"], 4);
	EXPECT_EQ(values["stddev"], 25);

	ctx.stop();
}




TEST(procstat, test_series_release_via_dir)
{
	{
		procstat::context ctx(mount_name());
		{
			auto dir = ctx.root().create_directory("dir");
			std::unique_ptr<procstat::series> series = dir.create_series("series");
			ASSERT_TRUE(boost::filesystem::exists(mount_name() + "/dir/series"));

			GTEST_COUT << "Now delete the series via root dir" << endl;
			ctx.root().delete_child("dir");
			GTEST_COUT << "Series should not be accessible" << endl;
			ASSERT_FALSE(boost::filesystem::exists(mount_name() + "/dir"));
			//ASSERT_FALSE(boost::filesystem::exists(mount_name() + "/dir/series"));
			GTEST_COUT << "Before series dies" << endl;
		}
		GTEST_COUT << "Stopping the context" << endl;
		ctx.stop();
	}
	GTEST_COUT << "Done" << endl;
}

TEST(procstat, test_procstat_histogram)
{
	auto series_path = mount_name() + "/histo1";
	procstat::context ctx(mount_name());

	std::unique_ptr<procstat::histogram> hist = ctx.root().create_histogram("histo1", {0.5, 0.99, 0.9999});

	auto values = read_histogram(series_path, {"50", "99", "99.99"});
	EXPECT_EQ(values["sum"], 0);
	EXPECT_EQ(values["count"], 0);
	EXPECT_EQ(values["last"], 0);
	EXPECT_EQ(values["avg"], 0);
	EXPECT_EQ(values["50"], 0);
	EXPECT_EQ(values["99"], 0);
	EXPECT_EQ(values["99.99"], 0);

	// Now run and fill the values
	for (int i = 0; i < 100; ++i) {
		hist->add_point(i);
	}

	values = read_histogram(series_path, {"50", "99", "99.99"});
	EXPECT_EQ(values["sum"], 4950); // sum of all numbers from 0 to 99
	EXPECT_EQ(values["count"], 100);
	EXPECT_EQ(values["last"], 99);
	EXPECT_EQ(values["avg"], 49);
	EXPECT_EQ(values["50"], 49);
	EXPECT_EQ(values["99"], 98);
	EXPECT_EQ(values["99.99"], 99);

	ctx.stop();
}


TEST(procstat, test_procstat_histogram_reset)
{
	auto series_path = mount_name() + "/histo1";
	procstat::context ctx(mount_name());

	std::unique_ptr<procstat::histogram> hist = ctx.root().create_histogram("histo1", {0.5, 0.99, 0.9999});
	// Now run and fill the values
	for (int i = 0; i < 100; ++i) {
		hist->add_point(i);
	}

	auto values = read_histogram(series_path, {"50", "99", "99.99"});
	EXPECT_EQ(values["sum"], 4950); // sum of all numbers from 0 to 99
	EXPECT_EQ(values["count"], 100);
	EXPECT_EQ(values["last"], 99);
	EXPECT_EQ(values["avg"], 49);
	EXPECT_EQ(values["50"], 49);
	EXPECT_EQ(values["99"], 98);
	EXPECT_EQ(values["99.99"], 99);

	cout << "Testing reset " << endl;
	write_to_stat_file(series_path + "/reset", "1");

	values = read_histogram(series_path, {"50", "99", "99.99"});
	EXPECT_EQ(values["sum"], 0); // sum of all numbers from 0 to 99
	EXPECT_EQ(values["count"], 0);
	EXPECT_EQ(values["last"], 0);
	EXPECT_EQ(values["avg"], 0);
	EXPECT_EQ(values["50"], 0);
	EXPECT_EQ(values["99"], 0);
	EXPECT_EQ(values["99.99"], 0);
}