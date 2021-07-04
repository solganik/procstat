#pragma once

#include <iostream>
#include "gtest/gtest.h"
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem.hpp>
#include <unordered_map>

namespace fs = boost::filesystem;
using namespace std;

inline string mount_name()
{
	return string("/tmp/") + ::testing::UnitTest::GetInstance()->current_test_info()->name();
}

template <typename T>
T read_stat_file(const string& stat_path) {
	fs::ifstream file(stat_path);
	if (file.fail()) {
		cout << "File " << stat_path << "Could not be read" << endl;
		abort();
	}
	T result;
	file >> result;
	return result;
}

template <typename T>
void write_to_stat_file(const string& stat_path, T value) {
	fs::ofstream file(stat_path);
	if (file.fail()) {
		cout << "File " << stat_path << "Could not be read" << endl;
		abort();
	}
	file << value;
}


inline unordered_map<string, uint64_t> read_series(const string& series_path)
{
	unordered_map<string, uint64_t> result;

	auto count = read_stat_file<uint64_t >(series_path + "/count");

	result["sum"] = read_stat_file<uint64_t >(series_path + "/sum");
	result["count"] = read_stat_file<uint64_t >(series_path + "/count");
	if (count > 0) {
		result["min"] = read_stat_file<uint64_t>(series_path + "/min");
		result["max"] = read_stat_file<uint64_t>(series_path + "/max");
		result["last"] = read_stat_file<uint64_t>(series_path + "/last");
		result["avg"] = read_stat_file<uint64_t>(series_path + "/avg");
		result["mean"] = read_stat_file<uint64_t>(series_path + "/mean");
		result["stddev"] = read_stat_file<uint64_t>(series_path + "/stddev");
	}
	return result;

}

inline unordered_map<string, uint64_t> read_histogram(const string& histo_path, vector<std::string> percentiles)
{
	unordered_map<string, uint64_t> result;

	result["sum"] = read_stat_file<uint64_t >(histo_path + "/sum");
	result["count"] = read_stat_file<uint64_t >(histo_path + "/count");
	result["last"] = read_stat_file<uint64_t>(histo_path + "/last");
	result["avg"] = read_stat_file<uint64_t>(histo_path + "/avg");

	for (auto percentile : percentiles) {
		result[percentile] = read_stat_file<uint64_t>(histo_path + "/" + percentile);
	}
	return result;
}
