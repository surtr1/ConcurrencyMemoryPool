#pragma once

#include <iostream>
#include <vector>
#include <unordered_map>
#include <map>
#include <algorithm>
#include <cstring>
#include <new>
#include <cstdint>

#include <time.h>
#include <assert.h>

#include <thread>
#include <mutex>
#include <atomic>

using std::cout;
using std::endl;

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
// 如果后续要支持 Linux，可以在这里补 mmap / munmap 等系统接口。
#endif

#if defined(_MSC_VER)
#define MP_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define MP_FORCEINLINE inline __attribute__((always_inline))
#else
#define MP_FORCEINLINE inline
#endif

// 整个内存池的关键参数：
// 1. MAX_BYTES：超过这个大小的请求不走小对象分配逻辑，直接按页处理；
// 2. NFREELIST：小对象按 size class 划分后，总共有多少个自由链表桶；
// 3. NPAGES：PageCache 一次最多管理 1~128 页的 span，因此数组长度开到 129；
// 4. PAGE_SHIFT=13：表示一页大小是 2^13 = 8192 字节，也就是 8KB。
static const size_t MAX_BYTES = 256 * 1024;
static const size_t NFREELIST = 208;
static const size_t NPAGES = 129;
static const size_t PAGE_SHIFT = 13;

#ifdef _WIN64
typedef unsigned long long PAGE_ID;
#elif _WIN32
typedef size_t PAGE_ID;
#else
// Linux 版本可以在这里补页号类型定义。
#endif

// 直接向操作系统申请 k 个页的连续空间。
// 注意：这里的“页”是当前内存池内部约定的 8KB 页，不是 Windows 默认的 4KB 物理页。
// SystemAlloc 只负责“要来一大块连续地址空间”，后续如何切成小对象由上层决定。
inline static void* SystemAlloc(size_t kpage)
{
#ifdef _WIN32
	void* ptr = VirtualAlloc(0, kpage << PAGE_SHIFT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
	// Linux 版本可以在这里补 mmap/sbrk 路径。
#endif

	if (ptr == nullptr)
		throw std::bad_alloc();

	return ptr;
}

// 将 SystemAlloc 申请到的整块页空间归还给操作系统。
inline static void SystemFree(void* ptr)
{
#ifdef _WIN32
	VirtualFree(ptr, 0, MEM_RELEASE);
#else
	// Linux 版本可以在这里补 munmap 路径。
#endif
}

// 小对象空闲时不会额外分配链表节点。
// 而是直接复用对象起始处的几个字节，存“下一个空闲对象”的指针。
// 这也是很多高性能内存池常见的做法。
MP_FORCEINLINE void*& NextObj(void* obj)
{
	return *(void**)obj;
}

// FreeList 是 ThreadCache 里最核心的局部结构：
// 每个 size class 对应一个 FreeList，里面串的是“当前线程可立即复用的小对象”。
class FreeList
{
public:
	// 头插一个空闲对象。
	MP_FORCEINLINE void Push(void* obj)
	{
		assert(obj);

		NextObj(obj) = _freeList;
		_freeList = obj;
		++_size;
	}

	// 批量头插一段对象。
	// start/end 已经是一条串好的单链表，这里只需要接到当前自由链表前面。
	MP_FORCEINLINE void PushRange(void* start, void* end, size_t n)
	{
		NextObj(end) = _freeList;
		_freeList = start;
		_size += n;
	}

	// 从头部批量摘下 n 个对象，返回 [start, end]。
	MP_FORCEINLINE void PopRange(void*& start, void*& end, size_t n)
	{
		assert(n <= _size);
		start = _freeList;
		end = start;

		for (size_t i = 0; i < n - 1; ++i)
		{
			end = NextObj(end);
		}

		_freeList = NextObj(end);
		NextObj(end) = nullptr;
		_size -= n;
	}

	// 弹出一个对象，作为小对象分配的最快路径。
	MP_FORCEINLINE void* Pop()
	{
		assert(_freeList);

		void* obj = _freeList;
		_freeList = NextObj(obj);
		--_size;

		return obj;
	}

	MP_FORCEINLINE bool Empty()
	{
		return _freeList == nullptr;
	}

	// 慢启动策略下，当前这条链表一次最多从 CentralCache 取多少对象。
	// 它控制“单次 refill 批量大小”。
	MP_FORCEINLINE size_t& MaxSize()
	{
		return _maxSize;
	}

	// 当前线程愿意在本地保留的对象上限。
	// 它控制“ThreadCache 最多囤多少对象”，避免缓存过多。
	MP_FORCEINLINE size_t& MaxCacheSize()
	{
		return _maxCacheSize;
	}

	MP_FORCEINLINE size_t Size() const
	{
		return _size;
	}

private:
	void* _freeList = nullptr;
	size_t _maxSize = 1;
	size_t _maxCacheSize = 1;
	size_t _size = 0;
};

// SizeClass 负责把“用户申请大小”转成“内存池内部规则”：
// 1. 先决定按什么粒度对齐；
// 2. 再决定落在哪个桶；
// 3. 再决定一次批量搬运多少对象、需要多少页。
class SizeClass
{
public:
	static inline size_t _RoundUp(size_t bytes, size_t alignNum)
	{
		return ((bytes + alignNum - 1) & ~(alignNum - 1));
	}

	// 不同大小区间使用不同的对齐粒度，兼顾空间利用率和桶数量。
	// 小对象对齐更细，减少内部碎片；大对象对齐更粗，减少桶数量。
	static inline size_t RoundUp(size_t size)
	{
		if (size <= 128)
		{
			return _RoundUp(size, 8);
		}
		else if (size <= 1024)
		{
			return _RoundUp(size, 16);
		}
		else if (size <= 8 * 1024)
		{
			return _RoundUp(size, 128);
		}
		else if (size <= 64 * 1024)
		{
			return _RoundUp(size, 1024);
		}
		else if (size <= 256 * 1024)
		{
			return _RoundUp(size, 8 * 1024);
		}
		else
		{
			return _RoundUp(size, 1 << PAGE_SHIFT);
		}
	}

	static inline size_t _Index(size_t bytes, size_t align_shift)
	{
		return ((bytes + (((size_t)1 << align_shift) - 1)) >> align_shift) - 1;
	}

	// 把“对齐后的大小”映射到桶下标。
	// 这里的分组和 RoundUp 一一对应，所以同一类大小一定会落到固定桶里。
	static inline size_t Index(size_t bytes)
	{
		assert(bytes <= MAX_BYTES);

		static int group_array[4] = { 16, 56, 56, 56 };
		if (bytes <= 128) {
			return _Index(bytes, 3);
		}
		else if (bytes <= 1024) {
			return _Index(bytes - 128, 4) + group_array[0];
		}
		else if (bytes <= 8 * 1024) {
			return _Index(bytes - 1024, 7) + group_array[1] + group_array[0];
		}
		else if (bytes <= 64 * 1024) {
			return _Index(bytes - 8 * 1024, 10) + group_array[2] + group_array[1] + group_array[0];
		}
		else if (bytes <= 256 * 1024) {
			return _Index(bytes - 64 * 1024, 13) + group_array[3] + group_array[2] + group_array[1] + group_array[0];
		}
		else {
			assert(false);
		}

		return -1;
	}

	// 决定 ThreadCache 和 CentralCache 一次搬运多少对象。
	// 对象越小，一次搬运得越多；对象越大，一次搬运得越少。
	static size_t NumMoveSize(size_t size)
	{
		assert(size > 0);

		size_t num = MAX_BYTES / size;
		if (num < 2)
			num = 2;

		if (num > 512)
			num = 512;

		return num;
	}

	// 决定某个 size class 在单线程本地最多缓存多少对象。
	// 这里让上限和批量搬运值相关，但又设置了兜底下限和上限。
	static size_t ThreadCacheMaxSize(size_t size)
	{
		size_t maxCacheSize = NumMoveSize(size) * 2;
		if (maxCacheSize < 2)
			maxCacheSize = 2;

		if (maxCacheSize > 1024)
			maxCacheSize = 1024;

		return maxCacheSize;
	}

	// 根据一次批量搬运对象的总字节数，换算出需要向 PageCache 申请多少页。
	static size_t NumMovePage(size_t size)
	{
		size_t num = NumMoveSize(size);
		size_t npage = (num * size + ((size_t)1 << PAGE_SHIFT) - 1) >> PAGE_SHIFT;
		if (npage == 0)
			npage = 1;

		return npage;
	}
};

// Span 表示一段连续页，是 PageCache / CentralCache 的核心管理单位。
// 当它被切成小对象后，还会额外记录：
// 1. 小对象大小；
// 2. 当前有多少对象被分配出去了；
// 3. 这一段页里剩余可分配对象组成的自由链表。
struct Span
{
	PAGE_ID _pageId = 0;   // 这段连续页的起始页号。
	size_t  _n = 0;        // 一共跨了多少页。

	Span* _next = nullptr;
	Span* _prev = nullptr;

	size_t _objSize = 0;   // 如果已经切成小对象，这里记录每个对象的大小。
	size_t _useCount = 0;  // 当前有多少个小对象已经被 ThreadCache 借走。
	void* _freeList = nullptr; // 当前 span 中还空闲的小对象链表。

	bool _isUse = false;   // 这段 span 是否正处于“被使用 / 被切分”状态。
};

// SpanList 是带头双向循环链表。
// CentralCache 和 PageCache 都会用它来管理同类 span。
class SpanList
{
public:
	SpanList()
	{
		_head = new Span;
		_head->_next = _head;
		_head->_prev = _head;
	}

	Span* Begin()
	{
		return _head->_next;
	}

	Span* End()
	{
		return _head;
	}

	bool Empty()
	{
		return _head->_next == _head;
	}

	void PushFront(Span* span)
	{
		Insert(Begin(), span);
	}

	void PushBack(Span* span)
	{
		Insert(End(), span);
	}

	Span* PopFront()
	{
		Span* front = _head->_next;
		Erase(front);
		return front;
	}

	// 在 pos 前插入一个新 span。
	void Insert(Span* pos, Span* newSpan)
	{
		assert(pos);
		assert(newSpan);

		Span* prev = pos->_prev;
		prev->_next = newSpan;
		newSpan->_prev = prev;
		newSpan->_next = pos;
		pos->_prev = newSpan;
	}

	// 从链表中摘掉一个 span。
	void Erase(Span* pos)
	{
		assert(pos);
		assert(pos != _head);

		Span* prev = pos->_prev;
		Span* next = pos->_next;

		prev->_next = next;
		next->_prev = prev;
	}

private:
	Span* _head;

public:
	// 每个桶各自带一把锁，让不同 size class 之间尽量互不影响。
	std::mutex _mtx;
};
