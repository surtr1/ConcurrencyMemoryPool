#pragma once

#include "Common.h"

// ThreadCache 是“每个线程独享”的第一层缓存。
//
// 它的职责非常明确：
// 1. 让小对象分配尽量只在当前线程本地完成；
// 2. 减少 CentralCache 的访问频率；
// 3. 把多线程竞争尽量挡在最外层之外。
class ThreadCache
{
public:
	// Allocate 是小对象分配的最快路径。
	//
	// 处理流程：
	// 1. 根据 size 做对齐；
	// 2. 找到这个 size 对应的自由链表；
	// 3. 如果线程本地已经有空闲对象，直接弹出返回；
	// 4. 如果没有，再向 CentralCache 批量取一段对象回来。
	MP_FORCEINLINE void* Allocate(size_t size)
	{
		assert(size <= MAX_BYTES);
		size_t alignSize = SizeClass::RoundUp(size);
		size_t index = SizeClass::Index(size);

		if (!_freeLists[index].Empty())
		{
			return _freeLists[index].Pop();
		}

		return FetchFromCentralCache(index, alignSize);
	}

	// Deallocate 负责把小对象释放回当前线程。
	//
	// 处理流程：
	// 1. 先挂回本线程对应 size class 的自由链表；
	// 2. 更新这条链表的本地缓存上限；
	// 3. 如果本地缓存已经囤得太多，就批量还一部分给 CentralCache。
	//
	// 这样做可以在“缓存命中率”和“内存占用”之间做一个平衡。
	MP_FORCEINLINE void Deallocate(void* ptr, size_t size)
	{
		assert(ptr);
		assert(size <= MAX_BYTES);

		size_t index = SizeClass::Index(size);
		FreeList& list = _freeLists[index];
		list.Push(ptr);

		// 根据对象大小，动态决定这条链表应该在本地保留多少对象。
		size_t targetCacheSize = SizeClass::ThreadCacheMaxSize(size);
		if (list.MaxCacheSize() < targetCacheSize)
		{
			list.MaxCacheSize() = targetCacheSize;
		}

		// 当本地缓存超过“缓存上限 + 一次回吐批量”时，批量归还一部分。
		size_t releaseBatch = SizeClass::NumMoveSize(size);
		if (list.Size() >= list.MaxCacheSize() + releaseBatch)
		{
			ListTooLong(list, size);
		}
	}

	// 当线程本地自由链表为空时，从 CentralCache 补货。
	void* FetchFromCentralCache(size_t index, size_t size);

	// 当线程本地某条自由链表过长时，把一部分对象回吐给 CentralCache。
	void ListTooLong(FreeList& list, size_t size);

private:
	// 每个 size class 都有一条线程本地自由链表。
	FreeList _freeLists[NFREELIST];
};

// 通过 TLS 保证每个线程拿到的是自己的 ThreadCache。
// 这正是 ThreadCache 能做到“无锁小对象分配”的关键前提之一。
MP_FORCEINLINE ThreadCache* GetThreadCache()
{
	thread_local ThreadCache tlsThreadCache;
	return &tlsThreadCache;
}
