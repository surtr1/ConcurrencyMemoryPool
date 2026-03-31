#include "ConcurrentAlloc.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <initializer_list>
#include <string>

// 这里统一使用 steady_clock 统计时间：
// 1. 它是单调时钟，不会因为系统时间调整而倒退；
// 2. 更适合做 benchmark 的阶段耗时统计。
using Clock = std::chrono::steady_clock;

namespace
{
	// 一个简单的循环栅栏。
	//
	// benchmark 里有多个工作线程和一个主线程：
	// - 工作线程负责真正执行 alloc/free；
	// - 主线程负责在阶段边界取时间。
	//
	// 使用栅栏的目的是让大家尽量同时进入 alloc 阶段、同时进入 free 阶段，
	// 从而减少“有的线程已经开始了，有的线程还没就绪”带来的计时偏差。
	class CyclicBarrier
	{
	public:
		explicit CyclicBarrier(size_t parties)
			: _parties(parties)
		{}

		void ArriveAndWait()
		{
			std::unique_lock<std::mutex> lock(_mtx);
			const size_t generation = _generation;

			++_arrived;
			if (_arrived == _parties)
			{
				_arrived = 0;
				++_generation;
				_cv.notify_all();
				return;
			}

			_cv.wait(lock, [&]() {
				return generation != _generation;
			});
		}

	private:
		std::mutex _mtx;
		std::condition_variable _cv;
		size_t _parties = 0;
		size_t _arrived = 0;
		size_t _generation = 0;
	};

	// 一次 benchmark 的结果分成两段：
	// 1. alloc 阶段总耗时；
	// 2. free 阶段总耗时。
	struct BenchmarkResult
	{
		double allocMs = 0.0;
		double freeMs = 0.0;

		double TotalMs() const
		{
			return allocMs + freeMs;
		}
	};

	// 描述一组测试用例。
	//
	// sizes：
	//   表示这一轮每个线程按顺序要申请哪些大小的对象。
	//   如果是固定尺寸场景，这个数组里的值都一样；
	//   如果是混合场景，这里会是一组混合大小。
	struct BenchmarkCase
	{
		std::string name;
		std::vector<size_t> sizes;
		size_t nworks = 0;
		size_t rounds = 0;
		bool crossThreadFree = false;
	};

	using AllocFn = void* (*)(size_t);
	using FreeFn = void (*)(void*);

	// 把 duration 统一转成毫秒，方便最终打印。
	double ToMilliseconds(Clock::duration duration)
	{
		return std::chrono::duration<double, std::milli>(duration).count();
	}

	// 用适配器的方式把系统 malloc/free 纳入同一套 benchmark 框架。
	void* MallocAdapter(size_t size)
	{
		return malloc(size);
	}

	void FreeAdapter(void* ptr)
	{
		free(ptr);
	}

	// 一个轻量级伪随机函数。
	// 主要目的是构造“可重复”的混合尺寸测试，而不是追求高质量随机性。
	uint32_t NextRandom(uint32_t& state)
	{
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		return state;
	}

	// 构造固定尺寸测试序列，例如全部 16B。
	std::vector<size_t> MakeFixedSizes(size_t count, size_t size)
	{
		return std::vector<size_t>(count, size);
	}

	// 构造混合尺寸测试序列。
	// choices 里给出候选大小，然后按伪随机方式从中取值。
	std::vector<size_t> MakeMixedSizes(size_t count, std::initializer_list<size_t> choices)
	{
		std::vector<size_t> palette(choices);
		std::vector<size_t> result;
		result.reserve(count);

		uint32_t state = 0x9E3779B9u;
		for (size_t i = 0; i < count; ++i)
		{
			result.push_back(palette[NextRandom(state) % palette.size()]);
		}

		return result;
	}

	// 执行一组 benchmark。
	//
	// 整个流程分成两大阶段：
	// 1. 所有线程并发 alloc；
	// 2. 所有线程并发 free。
	//
	// 主线程不参与真正的分配和释放，只负责在阶段边界处统计墙钟时间。
	BenchmarkResult RunBenchmark(const BenchmarkCase& benchCase, AllocFn allocFn, FreeFn freeFn)
	{
		// 每个线程对应一块本地指针缓冲区，记录这一轮申请到的对象地址。
		std::vector<std::vector<void*>> buffers(benchCase.nworks);
		for (size_t i = 0; i < benchCase.nworks; ++i)
		{
			buffers[i].reserve(benchCase.sizes.size());
		}

		CyclicBarrier barrier(benchCase.nworks + 1);
		std::vector<std::thread> workers;
		workers.reserve(benchCase.nworks);

		for (size_t i = 0; i < benchCase.nworks; ++i)
		{
			workers.emplace_back([&, i]() {
				std::vector<void*>& local = buffers[i];

				for (size_t round = 0; round < benchCase.rounds; ++round)
				{
					local.clear();

					// 等待主线程开始统计 alloc 阶段。
					barrier.ArriveAndWait();
					for (size_t size : benchCase.sizes)
					{
						local.push_back(allocFn(size));
					}
					barrier.ArriveAndWait();

					// 等待主线程开始统计 free 阶段。
					barrier.ArriveAndWait();

					// 如果开启跨线程释放，就释放“前一个线程”申请的对象；
					// 否则释放自己刚刚申请的对象。
					const size_t targetIndex = benchCase.crossThreadFree
						? (i + benchCase.nworks - 1) % benchCase.nworks
						: i;
					std::vector<void*>& target = buffers[targetIndex];
					for (void* ptr : target)
					{
						freeFn(ptr);
					}

					barrier.ArriveAndWait();
				}
			});
		}

		BenchmarkResult result;
		for (size_t round = 0; round < benchCase.rounds; ++round)
		{
			// alloc 阶段墙钟时间：
			// 主线程在栅栏两侧取时间，得到“所有线程一起完成这轮分配”用了多久。
			barrier.ArriveAndWait();
			Clock::time_point allocBegin = Clock::now();
			barrier.ArriveAndWait();
			result.allocMs += ToMilliseconds(Clock::now() - allocBegin);

			// free 阶段墙钟时间。
			barrier.ArriveAndWait();
			Clock::time_point freeBegin = Clock::now();
			barrier.ArriveAndWait();
			result.freeMs += ToMilliseconds(Clock::now() - freeBegin);
		}

		for (size_t i = 0; i < workers.size(); ++i)
		{
			workers[i].join();
		}

		return result;
	}

	// 预热一次：
	// 让首次建桶、首次触发页映射、首次线程局部缓存构造等冷启动成本先发生掉。
	// 这样正式统计时更接近“稳定运行阶段”的表现。
	void WarmUpBenchmark(const BenchmarkCase& benchCase, AllocFn allocFn, FreeFn freeFn)
	{
		BenchmarkCase warmUp = benchCase;
		warmUp.rounds = 1;
		if (warmUp.sizes.size() > 2048)
		{
			warmUp.sizes.resize(2048);
		}

		RunBenchmark(warmUp, allocFn, freeFn);
	}

	// 打印当前用例的基本配置。
	void PrintCaseHeader(const BenchmarkCase& benchCase)
	{
		printf("Case: %s\n", benchCase.name.c_str());
		printf("threads=%zu, rounds=%zu, ops/thread=%zu, free-mode=%s\n",
			benchCase.nworks,
			benchCase.rounds,
			benchCase.sizes.size(),
			benchCase.crossThreadFree ? "cross-thread" : "same-thread");
	}

	// 打印某个分配器的 alloc / free / total 三段时间。
	void PrintAllocatorResult(const char* label, const BenchmarkResult& result)
	{
		printf("%-18s alloc: %8.2f ms\n", label, result.allocMs);
		printf("%-18s free : %8.2f ms\n", label, result.freeMs);
		printf("%-18s total: %8.2f ms\n", label, result.TotalMs());
	}

	// 直接根据 total 时间比较胜负并打印加速比。
	void PrintComparison(const BenchmarkResult& poolResult, const BenchmarkResult& mallocResult)
	{
		if (poolResult.TotalMs() < mallocResult.TotalMs())
		{
			printf("winner: concurrent pool, speedup %.2fx\n",
				mallocResult.TotalMs() / poolResult.TotalMs());
		}
		else
		{
			printf("winner: malloc/free, speedup %.2fx\n",
				poolResult.TotalMs() / mallocResult.TotalMs());
		}
	}

	// 运行一组完整用例：
	// 1. 先分别预热；
	// 2. 再分别跑内存池和 malloc/free；
	// 3. 最后打印结果。
	void RunAndPrintCase(const BenchmarkCase& benchCase, size_t& poolWins)
	{
		WarmUpBenchmark(benchCase, &ConcurrentAlloc, &ConcurrentFree);
		WarmUpBenchmark(benchCase, &MallocAdapter, &FreeAdapter);

		BenchmarkResult poolResult = RunBenchmark(benchCase, &ConcurrentAlloc, &ConcurrentFree);
		BenchmarkResult mallocResult = RunBenchmark(benchCase, &MallocAdapter, &FreeAdapter);

		PrintCaseHeader(benchCase);
		PrintAllocatorResult("ConcurrentAlloc", poolResult);
		PrintAllocatorResult("malloc/free", mallocResult);
		PrintComparison(poolResult, mallocResult);

		if (poolResult.TotalMs() < mallocResult.TotalMs())
		{
			++poolWins;
		}

		printf("----------------------------------------------------------\n");
	}
}

int main()
{
	const size_t nworks = 4;
	std::vector<BenchmarkCase> cases;

	// 用例 1：固定 16B 热点小对象。
	// 主要观察 ThreadCache 对高频小对象分配的加速效果。
	cases.push_back({
		"Fixed 16B / hot small object",
		MakeFixedSizes(100000, 16),
		nworks,
		10,
		false
	});

	// 用例 2：固定 256B。
	// 相比 16B，对象更大一些，可以观察不同 size class 下的表现。
	cases.push_back({
		"Fixed 256B / same-thread free",
		MakeFixedSizes(60000, 256),
		nworks,
		8,
		false
	});

	// 用例 3：16B~1024B 混合小尺寸。
	// 这个场景更接近真实业务里“多个 size class 同时活跃”的情况。
	cases.push_back({
		"Mixed small sizes / 16B~1024B",
		MakeMixedSizes(60000, { 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024 }),
		nworks,
		8,
		false
	});

	// 用例 4：跨线程 free。
	// 用来观察对象在“线程 A 申请、线程 B 释放”场景下的整体表现。
	cases.push_back({
		"Cross-thread free / fixed 64B",
		MakeFixedSizes(80000, 64),
		nworks,
		8,
		true
	});

	// 用例 5：大对象场景。
	// 这组主要覆盖 PageCache / 系统页分配路径，而不是小对象桶逻辑。
	cases.push_back({
		"Large objects / 64KB~256KB",
		MakeMixedSizes(256, { 64 * 1024, 128 * 1024, 256 * 1024 }),
		nworks,
		40,
		false
	});

	size_t poolWins = 0;

	printf("==========================================================\n");
	// 这里统计的是每个阶段的墙钟时间，而不是进程 CPU 时间。
	printf("steady_clock benchmark, phase wall-clock time\n");
	printf("==========================================================\n");

	for (size_t i = 0; i < cases.size(); ++i)
	{
		RunAndPrintCase(cases[i], poolWins);
	}

	printf("Summary: concurrent pool wins %zu/%zu cases\n", poolWins, cases.size());
	printf("==========================================================\n");

	return 0;
}
