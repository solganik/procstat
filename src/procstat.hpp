#pragma once


#include <string>
#include <memory>
#include <system_error>
#include <thread>
#include <sstream>
#include <iostream>
#include <fstream>
#include "procstat.h"

namespace procstat {

	template<typename T>
	ssize_t formatter(void *object, uint64_t arg, char *buffer, size_t length) {
		auto *field = reinterpret_cast<T *>(object);
		std::stringbuf output;
		output.pubsetbuf(buffer, length);
		std::ostream os(&output);
		os << *field << std::endl;
		return os.tellp();
	}


	class context;

	class directory;
	
	class series {
		friend class directory;

	public:
		series(const series &other) = delete;

		series(const series &&other) = delete;

		series() = default;

		void add_point(const uint64_t value) {
			procstat_u64_series_add_point(&impl, value);
		}

	private:
		procstat_series_u64 impl;
	};


	class histogram {
		friend class directory;

	public:
		histogram(const histogram &other) = delete;

		histogram(const histogram &&other) = delete;

		histogram(std::initializer_list<float> percentiles) : impl{} {
			if (percentiles.size() > MAX_SUPPORTED_PERCENTILE) {
				throw std::invalid_argument("max supported percentiles size exceeded");
			}

			int i = 0;
			for (auto &percentile: percentiles) {
				if (percentile >= 1) {
					throw std::invalid_argument("percentile must be < 1");
				}
				impl.percentile[i].fraction = percentile;
				++i;
			}
			impl.npercentile = percentiles.size();
		}

		std::vector<float> get_percentiles() {
			std::vector<float> result;
			for (int i = 0; i < impl.npercentile; ++i) {
				result.push_back(impl.percentile[i].fraction);
			}
			return result;
		}

		void add_point(const uint32_t value) {
			procstat_histogram_u32_add_point(&impl, value);
		}

	private:
		procstat_histogram_u32 impl;
	};

	class directory {
	public:
		directory(const directory &other) = default;

		directory(directory &&other) = default;

		directory create_directory(const std::string &name) {
			auto *item = procstat_create_directory(procstat_context(impl), impl, name.c_str());
			if (!item) {
				throw std::system_error(errno, std::generic_category(), name);
			}
			return directory(item);
		}

		template<typename T>
		void create(const std::string &name, T &parameter) {
			procstat_simple_handle handle{name.c_str(),
										  &parameter,
										  0,
										  formatter<T>,
										  nullptr};

			int error = procstat_create_simple(procstat_context(impl), impl, &handle, 1);
			if (error != 0) {
				throw std::system_error(errno, std::generic_category(), name);
			}
		}


		template<typename T>
		void create_start_end(const std::string &name, std::pair<T, T> &start_end) {
			procstat_start_end_handle handle{name.c_str(),
											 &start_end.first,
											 &start_end.second,
											 formatter<T>};
			int error = procstat_create_start_end(procstat_context(impl), impl, &handle, 1);
			if (error != 0) {
				throw std::system_error(errno, std::generic_category(), name);
			}
		}

		std::unique_ptr<series> create_series(const std::string &name) {

			auto result = std::make_unique<series>();
			auto *ctx = procstat_context(impl);

			int error = procstat_create_u64_series(ctx, impl, name.c_str(), &result->impl);
			if (error != 0) {
				throw std::system_error(errno, std::generic_category(), name);
			}
			return result;
		}


		std::unique_ptr<histogram>
		create_histogram(const std::string &name, std::initializer_list<float> percentiles) {

			auto result = std::make_unique<histogram>(percentiles);
			auto *ctx = procstat_context(impl);

			int error = procstat_create_histogram_u32_series(ctx, impl, name.c_str(), &result->impl);
			if (error != 0) {
				throw std::system_error(errno, std::generic_category(), name);
			}
			return result;
		}

		void delete_child(const std::string &name) {
			procstat_remove_by_name(procstat_context(impl), impl, name.c_str());
		}

	private:
		friend class context;

		friend class series;

		directory(struct procstat_item *item) : impl(item) { ; }

		struct procstat_item *impl;
	};


	class context {
	public:
		context(const std::string &mountpoint) {
			impl = procstat_create(mountpoint.c_str());
			if (!impl) {
				throw std::system_error(errno, std::generic_category(), mountpoint);
			}
		}

		context(const context &other) = delete;

		context(context &&other) = delete;

		~context() {
			stop();
			procstat_destroy(impl);
		}

		void start() {
			fuse_thread = std::thread{procstat_loop, impl};
		}


		void stop() {

			if (fuse_thread.joinable()) {
				procstat_stop(impl);
				fuse_thread.join();
			}
		}

		directory root() {
			return directory(procstat_root(impl));
		}

	private:
		struct procstat_context *impl;
		std::thread fuse_thread;
	};
}