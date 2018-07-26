#pragma once


#include <chrono>
#include <functional>
#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef DISABLE_CPU_COUNTERS

#include <string.h>
#include <sys/types.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>


namespace benchmark {

std::ostream& operator<<(std::ostream& str, std::chrono::system_clock::time_point time_point);
std::ostream& operator<<(std::ostream& str, std::chrono::high_resolution_clock::duration duration);

namespace internal {
namespace perf {


// Trivial wrapper around perf_event_open() syscall to provide type-checking
// for args.
static int
sys_perf_event_open(struct perf_event_attr *attr,
                      pid_t pid, int cpu, int group_fd,
                      unsigned long flags)
{
        return syscall(__NR_perf_event_open, attr, pid, cpu,
                       group_fd, flags);
}


int createHardwareCounter(enum perf_hw_id type)
{
	struct perf_event_attr pea = {0};
	pea.type = PERF_TYPE_HARDWARE;
	pea.config = type;

        // pid == 0: counter is attached to the current task
        //      TODO - does the above mean current thread, or process?
	//      TODO - can we actually create multiple counters of the
	//             same kind for the same thread/process?
        // cpu == -1: count on all cpu's
        // group_fd == -1: create counter in a new group
	int fd = sys_perf_event_open(&pea, 0, -1, -1, PERF_FLAG_FD_CLOEXEC);
        if (fd < 0) {
                printf("Error %d: %s\n", errno, strerror(errno));
                if (errno == EACCES) {
                        printf("Try setting /proc/sys/kernel/perf_event_paranoid to 1\n");
                }
                if (errno == ENOSYS) {
                        printf("No CONFIG_PERF_EVENTS=y kernel support configured?\n");
                }
        }

	return fd;
}


uint64_t readCounter(int fd)
{
	uint64_t value = -1;

	// todo - error handling
	__attribute__((unused))
	ssize_t todo = read(fd, &value, sizeof(value));

	return value;
}

}}} // namespace benchmark::internal::perf

#endif // !DISABLE_CPU_COUNTERS


namespace benchmark {

struct CpuCounter {
	enum struct Type {
		Instructions = PERF_COUNT_HW_INSTRUCTIONS,
		Cycles = PERF_COUNT_HW_CPU_CYCLES,
	};

	CpuCounter(Type type);
	~CpuCounter();

	void reset();

	uint64_t split() const;

private:
	int fd_;
	uint64_t initial_;
};

CpuCounter::CpuCounter(Type type)
  : fd_(internal::perf::createHardwareCounter(static_cast<perf_hw_id>(type)))
  , initial_(internal::perf::readCounter(fd_))
{}

CpuCounter::~CpuCounter()
{
	::close(fd_);
}

void CpuCounter::reset()
{
	initial_ = internal::perf::readCounter(fd_);
}

uint64_t CpuCounter::split() const
{
  uint64_t count = internal::perf::readCounter(fd_);
  return count - initial_;
}

// todo - templatize on clock type?
struct TimeCounter {
	typedef std::chrono::high_resolution_clock::time_point timepoint;

	TimeCounter();

	void reset();

	std::chrono::nanoseconds split() const;

private:
	timepoint initial_;
};

TimeCounter::TimeCounter()
  : initial_(std::chrono::high_resolution_clock::now())
{}

void TimeCounter::reset()
{
  initial_ = std::chrono::high_resolution_clock::now();
}

std::chrono::nanoseconds TimeCounter::split() const
{
  auto now = std::chrono::high_resolution_clock::now();
  return now - initial_;
}


class ScopedSideChannelCollector
{
public:
	ScopedSideChannelCollector(const char* filename, const std::string& annotation);
	~ScopedSideChannelCollector(); // todo - rule of three

	template<typename T>
	void wait_for(const T& t) { asm volatile ("" : : "r"(&t)); }

private:
	const char* filename_;
	int exceptions_;
	std::string annotation_;

	TimeCounter walltime_;
	CpuCounter cycles_;
	CpuCounter instructions_;
};


ScopedSideChannelCollector::ScopedSideChannelCollector(const char* filename, const std::string& annotation)
  : filename_(filename)
#ifdef __cpp_lib_uncaught_exceptions
  , exceptions_(std::uncaught_exceptions())
#endif
  , annotation_(annotation)
  , cycles_(CpuCounter::Type::Cycles)
  , instructions_(CpuCounter::Type::Instructions)
{}


ScopedSideChannelCollector::~ScopedSideChannelCollector()
{
#ifdef __cpp_lib_uncaught_exceptions
	if (std::uncaught_exceptions() > exceptions_) {
		return;
	}
#endif

	// Measure in decreasing order of sensitivity.
	uint64_t instructions = instructions_.split();
	uint64_t cycles = cycles_.split();
	auto walltime = walltime_.split();

	std::ofstream file(filename_, std::ios_base::out | std::ios_base::app);
	auto now = std::chrono::system_clock::now();

	file << '\n';
	file << "# " << annotation_ << '\n';
	file << "# " << now << '\n';
	file << "instructions = " << instructions << '\n';
	file << "cycles = " << cycles << '\n';
	file << "walltime = " << walltime << std::endl;
}


class MultiCounter
{
public:
	MultiCounter();

	void reset();

	struct Measurement {
		uint64_t instructions;
		uint64_t cycles;
		std::chrono::high_resolution_clock::duration walltime;
	};

	template<typename T>
	Measurement split_after(const T& t);

private:
	TimeCounter walltime_;
	CpuCounter cycles_;
	CpuCounter instructions_;
};

MultiCounter::MultiCounter()
  : cycles_(CpuCounter::Type::Cycles)
  , instructions_(CpuCounter::Type::Instructions)
{}

void MultiCounter::reset()
{
	// We can't do this atomically, so at least we order it
	// from least to most sensitive.
	walltime_.reset();
	cycles_.reset();
	instructions_.reset();
}


template<typename T>
MultiCounter::Measurement MultiCounter::split_after(const T& t)
{
	asm volatile ("" : : "r"(&t));

	uint64_t instructions = instructions_.split();
	uint64_t cycles = cycles_.split();
	auto walltime = walltime_.split();

	return Measurement {instructions, cycles, walltime};
}

class PyplotScriptFormatter {
public:
	static std::ostream& preamble(std::ostream& str);
	static std::ostream& datapoint(std::ostream& str, const char* tag, int xpos, const MultiCounter::Measurement& m);
};

std::ostream& PyplotScriptFormatter::preamble(std::ostream& str)
{
	return str <<
R"_(#!/usr/bin/python

from __future__ import print_function
import atexit
import collections
import numpy as np
import matplotlib.pyplot as plt

benchmark_data = collections.defaultdict(lambda: collections.defaultdict(list))

def plot(data):
    plot_data = {}
    quantity = "cycles"
    raw = []

    for tag, data_ in data.iteritems():
        plot_data[tag] = {}
        plot_data[tag]["x"] = []
        plot_data[tag]["y"] = []
        plot_data[tag]["yerr"] = []


        print("# " + tag)
        for xpos, lst in data_.iteritems():
            cycles = []
            insns = []
            times = []

            pdata = []

            print(" - " + str(xpos) + " (" + str(len(lst)) + " datapoints)")

            for elem in lst:
                pdata.append(elem[quantity])
                raw.append( (xpos, elem[quantity]) )

                cycles.append(elem["cycles"])
                insns.append(elem["instructions"])
                times.append(elem["walltime"])

            cycles_mean, insns_mean, times_mean = np.mean(cycles), np.mean(insns), np.mean(times)
            cycles_variance, insns_variance, times_variance = np.std(cycles), np.std(insns), np.std(times)
            cycles_variance_pct = cycles_variance / cycles_mean * 100
            insns_variance_pct = insns_variance / insns_mean * 100
            times_variance_pct = times_variance / times_mean * 100
            print("   cycles:       {:10.0f} +- {:5.0f} ({:2.1f}%)".format(cycles_mean, cycles_variance, cycles_variance_pct))
            print("   instructions: {:10.0f} +- {:5.0f} ({:2.1f}%)".format(insns_mean, insns_variance, insns_variance_pct))
            print("   times:        {:10.0f} +- {:5.0f} ({:2.1f}%)".format(times_mean, times_variance, times_variance_pct))
            print("")

            mean = np.mean(pdata[1:-2]) # filter out highest and lowest for rudimentary outlier-cleaning
            variance = np.std(pdata[1:-2]) # same

            plot_data[tag]["x"].append(xpos)
            plot_data[tag]["y"].append(mean)
            plot_data[tag]["yerr"].append(variance)

    plt.title("Results for quantity '{}'".format(quantity))
    plt.scatter(*zip(*raw))
    for tag, pltdata in plot_data.iteritems():
        plt.plot(pltdata["x"], pltdata["y"], 'o', yerr=pltdata["yerr"], label=tag)

    plt.legend()
    plt.show()


atexit.register(lambda: plot(benchmark_data))

)_";
}


std::ostream& PyplotScriptFormatter::datapoint(std::ostream& str, const char* tag, int xpos, const MultiCounter::Measurement& m)
{
	auto now = std::chrono::system_clock::now();
	str << "benchmark_data['" << tag << "'][" << xpos << "].append({\n";
	str << "  'cycles': " << m.cycles << ",\n";
	str << "  'instructions': " << m.instructions << ",\n";
	str << "  'walltime': " << m.walltime << ",\n";
	str << "  'date': " << std::chrono::system_clock::to_time_t(now) << ",\n";
	return str << "})\n";
}

class SideChannelCollector
{
public:
	// All measurements for the same quantity and xpos are considered
	// to be sampled from the same distribution, and statistics are computed
	// over them (i.e. mean, variance)
	SideChannelCollector(const char* filename, const char* quantity, int xpos = 0);

	void reset();

	// todo - provide overloads to override quantity and/or xpos
	template<typename T>
	void split_after(const T& t);

	template<typename T>
	void split_after(const T& t, int xpos);
private:
	const char* filename_;
	const char* quantity_;
	int xpos_;

	MultiCounter counter_;

	// todo - add template magic etc.
	typedef PyplotScriptFormatter Formatter;
};


SideChannelCollector::SideChannelCollector(
    const char* filename,
    const char* quantity,
    int xpos)
  : filename_(filename)
  , quantity_(quantity)
  , xpos_(xpos)
{
	this->reset();
}


void SideChannelCollector::reset()
{
	counter_.reset();
	// Attempt to clobber memory to generate a rwbarrier,
	// so initialization cannot be moved too early.
	// (should probably rather go into the individual counters)
	asm volatile("" : : : "memory");
}

template<typename T>
void SideChannelCollector::split_after(const T& t)
{
	split_after(t, xpos_);
}

template<typename T>
void SideChannelCollector::split_after(const T& t, int xpos)
{
	MultiCounter::Measurement m = counter_.split_after(t);

	std::ofstream file(filename_, std::ios_base::out | std::ios_base::app);
	bool empty = file.tellp() == 0;

	if (empty) {
		Formatter::preamble(file);
	}

	Formatter::datapoint(file, quantity_, xpos, m);

	file.close();

	this->reset();
}





std::ostream& operator<<(std::ostream& str, std::chrono::high_resolution_clock::duration duration)
{
	return str << std::chrono::duration_cast<std::chrono::duration<double, std::ratio<1>>>(duration).count();
}

std::ostream& operator<<(std::ostream& str, std::chrono::system_clock::time_point time_point)
{
	time_t time = std::chrono::system_clock::to_time_t(time_point);
        // ctime() prints the time in a fixed-width 24-byte long
	// format string followed by a newline
	char buffer[26];
	ctime_r(&time, buffer);
	buffer[24] = 0;
	return str << buffer;
}


} // namespace benchmark
