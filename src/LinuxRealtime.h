#pragma once

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>

#include "Platform.h"

namespace Voice::Linux {

using Clock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds MaxSleepSlice{50};

inline std::vector<int> AllowedCpus() {
	std::vector<int> Cpus;
	cpu_set_t Set;
	CPU_ZERO(&Set);
	if (sched_getaffinity(0, sizeof(Set), &Set) != 0) {
		return Cpus;
	}
	for (int Index = 0; Index < CPU_SETSIZE; ++Index) {
		if (CPU_ISSET(Index, &Set)) {
			Cpus.push_back(Index);
		}
	}
	return Cpus;
}

inline bool PinCurrentThread(int Cpu) {
	cpu_set_t Set;
	CPU_ZERO(&Set);
	CPU_SET(Cpu, &Set);
	return pthread_setaffinity_np(pthread_self(), sizeof(Set), &Set) == 0;
}

inline bool ElevateCurrentThread(int Priority = 80) {
	sched_param Param{};
	Param.sched_priority = Priority;
	return pthread_setschedparam(pthread_self(), SCHED_FIFO, &Param) == 0;
}

inline bool LockMemory() {
	return mlockall(MCL_CURRENT) == 0;
}

inline timespec ToTimespec(Clock::time_point Point) {
	const long long Nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
		Point.time_since_epoch()
	)
		.count();
	timespec Spec{};
	Spec.tv_sec = static_cast<time_t>(Nanoseconds / 1000000000LL);
	Spec.tv_nsec = static_cast<long>(Nanoseconds % 1000000000LL);
	return Spec;
}

inline Clock::time_point WaitUntil(
	Clock::time_point Deadline,
	std::chrono::nanoseconds SpinWindow,
	const std::atomic<bool>& Stopping
) {
	while (!Stopping.load(std::memory_order_relaxed)) {
		const Clock::time_point Now = Clock::now();
		if (Now >= Deadline) {
			return Now;
		}
		if (Deadline - Now > SpinWindow) {
			const Clock::time_point Slice = std::min(Deadline - SpinWindow, Now + MaxSleepSlice);
			const timespec Spec = ToTimespec(Slice);
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &Spec, nullptr);
		} else {
			Platform::CpuPause();
		}
	}
	return Clock::now();
}

}
