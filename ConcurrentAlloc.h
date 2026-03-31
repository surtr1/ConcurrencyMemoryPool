#pragma once

#include "PageCache.h"
#include "ThreadCache.h"

// ConcurrentAlloc 是整个内存池对外暴露的分配入口。
//
// 它把请求分成两大类：
// 1. 小对象：走 ThreadCache -> CentralCache -> PageCache 这条分层缓存路径；
// 2. 大对象：不再切小块，直接按页向 PageCache 申请。
//
// 这样设计的目的，是让“小对象高频请求”尽量命中线程本地缓存，
// 同时让“大对象请求”避免进入复杂的小对象桶管理逻辑。
MP_FORCEINLINE void* ConcurrentAlloc(size_t size)
{
	// 大于 MAX_BYTES 的对象，直接走按页分配逻辑。
	if (size > MAX_BYTES)
	{
		// 大对象仍然先按内存池规则向上对齐，这样便于统一换算页数。
		size_t alignSize = SizeClass::RoundUp(size);
		size_t kpage = alignSize >> PAGE_SHIFT;

		// 大对象会直接操作 PageCache，因此这里需要持有 PageCache 的总锁。
		std::lock_guard<std::mutex> pageLock(PageCache::GetInstance()->_pageMtx);
		Span* span = PageCache::GetInstance()->NewSpan(kpage);
		span->_isUse = true;
		span->_objSize = size;

		// 返回值就是这段 span 的起始地址。
		return (void*)(span->_pageId << PAGE_SHIFT);
	}

	// 小对象优先走当前线程自己的 ThreadCache，尽量避免跨线程竞争。
	return GetThreadCache()->Allocate(size);
}

// ConcurrentFree 是对外暴露的释放入口。
//
// 它先根据地址找到对象所属的 span，再决定释放路径：
// 1. 如果是大对象，直接回到 PageCache；
// 2. 如果是小对象，回到当前线程的 ThreadCache。
//
// 注意：这里的小对象释放并不要求“在哪个线程申请，就必须在哪个线程释放”。
// 也就是说，跨线程 free 是允许的，只是对象会回到“当前释放线程”的 ThreadCache。
MP_FORCEINLINE void ConcurrentFree(void* ptr)
{
	if (ptr == nullptr)
	{
		return;
	}

	// 先反查这个地址属于哪个 span。
	Span* span = PageCache::GetInstance()->MapObjectToSpan(ptr);
	size_t size = span->_objSize;

	// 大对象直接按页归还。
	if (size > MAX_BYTES)
	{
		std::lock_guard<std::mutex> pageLock(PageCache::GetInstance()->_pageMtx);
		PageCache::GetInstance()->ReleaseSpanToPageCache(span);
		return;
	}

	// 小对象回到当前线程本地缓存。
	GetThreadCache()->Deallocate(ptr, size);
}
