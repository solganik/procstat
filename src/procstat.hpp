#pragma once


#include <string>
#include <memory>
#include <system_error>
#include <thread>
#include <sstream>
#include <iostream>
#include <fstream>
#include "procstat.h"

struct procstat_context;
struct procstat_item;

namespace procstat {

	template<typename T>
	ssize_t formatter(void *object, uint64_t arg, char *buffer, size_t length)
	{
		auto *field = reinterpret_cast<T *>(object);
		std::stringbuf output;
		output.pubsetbuf(buffer, length);
		std::ostream os(&output);
		os << *field << std::endl;
		return os.tellp();
	}

	class series;

	class histogram;

	/**
	 * @brief the purpose of this class is to "hold" procstat objects registered and alive.
	 * It is created internally by the procstat and should be kept by "user" to keep the "variable"
	 * registered.
	 */
	class registration {
		friend class histogram;

		friend class series;

		friend class directory;

	public:
		registration(const registration &other)
		{
			impl = other.impl;
			ctx = other.ctx;
			procstat_refget(ctx, impl);
		}

		registration(registration &&other)
		{
			impl = other.impl;
			ctx = other.ctx;
			other.impl = nullptr;
			other.ctx = nullptr;
		}

		void detatch()
		{
			if ((impl) && (ctx)) {
				procstat_refput(ctx, impl);
			}
			ctx = nullptr;
			impl = nullptr;
		}

		virtual ~registration()
		{
			if ((impl) && (ctx)) {
				procstat_remove(ctx, impl);
				procstat_refput(ctx, impl);
			}
		}

	private:
		registration() : impl{}, ctx{} { ; }

		registration(struct procstat_context *ctx, procstat_item *impl) : ctx(ctx), impl(impl) {}

		void attach(struct procstat_context *ctx, procstat_item *impl)
		{
			this->impl = impl;
			this->ctx = ctx;
		}

		procstat_item *impl;
		struct procstat_context *ctx;
	};


	/**
	 * @brief represents series registry of "series" statistics. Series are u64
	 * statistics that exposes basic min, max, avg, sum, count, last, stddev statistics
	 * via fuse. Also statistics can be reset via writing "echo 1 > <series mount>/reset file
	 * Note: There are also no "locks" on hotpath, so histogram is relatively fast, however it is possible
	 * that during some reads you will get slightly inconsistent values. (For example sum and count are updated
	 * separately, so the "read" can catch the values in the middle of the update). This does not really matter
	 * on large input series as it will have only minor affect
	 */
	class series : public registration {
	public:
		series(const series &other) = delete;

		series(const series &&other) = delete;

		series() = default;

		series(struct procstat_item *parent, const std::string &name) : impl{}
		{
			auto *ctx = procstat_context(parent);
			int error = procstat_create_u64_series(ctx, parent, name.c_str(), &impl);
			if (error != 0) {
				throw std::system_error(errno, std::generic_category(), name);
			}

			auto item = procstat_lookup_item(ctx, parent, name.c_str());
			if (!item) {
				throw std::system_error(errno, std::generic_category(), name);
			}
			registry.attach(ctx, item);
		}

		void add_point(const uint64_t value)
		{
			procstat_u64_series_add_point(&impl, value);
		}

	private:
		registration registry;
		procstat_series_u64 impl;
	};


	/**
	 * @brief represents histogram registry of "histogram" statistics. Histogram are u32
	 * statistics that exposes sum, count, last, avg and specified percentiles
	 * via fuse. Also statistics can be reset via writing "echo 1 > <series mount>/reset file
	 * !Note: every histogram takes ~4K bytes for "buckets" calculation. So every "add point"
	 * is essentually a cache miss. (unless entries are really close). There are also no "locks" on
	 * hotpath, so histogram is relatively fast.
	 */
	class histogram : public registration {
	public:
		histogram(const histogram &other) = delete;

		histogram(const histogram &&other) = delete;

		histogram(struct procstat_item *parent, const std::string &name, std::initializer_list<float> percentiles)
				: impl{}
		{
			auto *ctx = procstat_context(parent);
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

			int error = procstat_create_histogram_u32_series(ctx, parent, name.c_str(), &impl);
			if (error) {
				throw std::system_error(errno, std::generic_category(), name);
			}

			auto item = procstat_lookup_item(ctx, parent, name.c_str());
			if (!item) {
				throw std::system_error(errno, std::generic_category(), name);
			}

			registry.attach(ctx, item);
		}

		std::vector<float> get_percentiles()
		{
			std::vector<float> result;
			for (int i = 0; i < impl.npercentile; ++i) {
				result.push_back(impl.percentile[i].fraction);
			}
			return result;
		}

		void add_point(const uint32_t value)
		{
			procstat_histogram_u32_add_point(&impl, value);
		}

	private:
		procstat_histogram_u32 impl;
		registration registry;
	};

	class directory {
	public:
		directory(const directory &other) = default;

		directory(directory &&other) = default;

		directory create_directory(const std::string &name)
		{
			auto *item = procstat_create_directory(procstat_context(impl), impl, name.c_str());
			if (!item) {
				throw std::system_error(errno, std::generic_category(), name);
			}
			return directory(item);
		}

		template<typename T>
		void create(const std::string &name, T &parameter)
		{
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
		registration create_start_end(const std::string &name, std::pair<T, T> &start_end)
		{
			auto *ctx = procstat_context(impl);
			procstat_start_end_handle handle{name.c_str(),
											 &start_end.first,
											 &start_end.second,
											 formatter<T>};
			int error = procstat_create_start_end(ctx, impl, &handle, 1);
			if (error != 0) {
				throw std::system_error(errno, std::generic_category(), name);
			}

			auto item = procstat_lookup_item(ctx, impl, name.c_str());
			if (!item) {
				throw std::system_error(errno, std::generic_category(), name);
			}


			return registration(ctx, item);
		}

		std::unique_ptr<series> create_series(const std::string &name)
		{
			return std::make_unique<series>(impl, name);
		}


		std::unique_ptr<histogram>
		create_histogram(const std::string &name, std::initializer_list<float> percentiles)
		{
			return std::make_unique<histogram>(impl, name, percentiles);
		}

		void delete_child(const std::string &name)
		{
			procstat_remove_by_name(procstat_context(impl), impl, name.c_str());
		}

	private:
		friend class context;

		friend class series;

		directory(struct procstat_item *item) : impl(item) { ; }

		struct procstat_item *impl;
	};


	/**
	 * @brief represents procstat context, should be initialized on application start and started.
	 */
	class context {
	public:
		/**
		 * @param mountpoint where to mount the stats
		 * @param autostart whether to start serving statistics automatically
		 */
		context(const std::string &mountpoint, bool autostart = true)
		{
			impl = procstat_create(mountpoint.c_str());
			if (!impl) {
				throw std::system_error(errno, std::generic_category(), mountpoint);
			}

			if (autostart) {
				start();
			}
		}

		context(const context &other) = delete;

		context(context &&other) = delete;

		~context()
		{
			stop();
			procstat_destroy(impl);
		}

		/**
		 * @brief starts stats looper
		 */
		void start()
		{
			if (fuse_thread.joinable()) {
				return;
			}
			fuse_thread = std::thread{procstat_loop, impl};
		}

		/**
		 * @brief stops the fuse looper
		 */
		void stop()
		{
			if (fuse_thread.joinable()) {
				procstat_stop(impl);
				fuse_thread.join();
			}
		}

		/**
		 * @return root directory of the statistics
		 */
		directory root()
		{
			return directory(procstat_root(impl));
		}

	private:
		struct procstat_context *impl;
		std::thread fuse_thread;
	};
}