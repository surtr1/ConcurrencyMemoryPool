#include "CentralCache.h"

#include "PageCache.h"

CentralCache CentralCache::_sInst;

// 获取一个可用 span 的逻辑分两步：
// 1. 先在当前桶里找一个还有空闲对象的 span；
// 2. 如果没有，就向 PageCache 申请新页并现场切成小对象。
//
// 另外，这里会尽量把“还有空闲对象的 span”挪到链表前面，
// 这样下次 refill 时更容易命中，减少线性扫描成本。
Span* CentralCache::GetOneSpan(SpanList& list, size_t size)
{
	Span* it = list.Begin();
	while (it != list.End())
	{
		if (it->_freeList != nullptr)
		{
			if (it != list.Begin())
			{
				list.Erase(it);
				list.PushFront(it);
			}

			return it;
		}

		it = it->_next;
	}

	// 当前桶没有可用 span 时，需要去 PageCache 申请新页。
	// 这里先释放 CentralCache 桶锁，避免锁顺序反转。
	list._mtx.unlock();

	PageCache::GetInstance()->_pageMtx.lock();
	Span* span = PageCache::GetInstance()->NewSpan(SizeClass::NumMovePage(size));
	span->_isUse = true;
	span->_objSize = size;
	PageCache::GetInstance()->_pageMtx.unlock();

	// 把整段 span 切成多个等长小对象，并串成自由链表。
	char* start = (char*)(span->_pageId << PAGE_SHIFT);
	size_t bytes = span->_n << PAGE_SHIFT;
	char* end = start + bytes;

	span->_freeList = start;
	start += size;

	void* tail = span->_freeList;
	while (start < end)
	{
		NextObj(tail) = start;
		tail = NextObj(tail);
		start += size;
	}
	NextObj(tail) = nullptr;

	list._mtx.lock();
	list.PushFront(span);

	return span;
}

// 从中央缓存批量拿一段对象给 ThreadCache。
//
// 这里不是每次只拿一个对象，而是按 batchNum 一次摘下一段，
// 目的是减少 ThreadCache 频繁访问 CentralCache 的次数。
size_t CentralCache::FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size)
{
	size_t index = SizeClass::Index(size);
	_spanLists[index]._mtx.lock();

	Span* span = GetOneSpan(_spanLists[index], size);
	assert(span);
	assert(span->_freeList);

	start = span->_freeList;
	end = start;

	size_t actualNum = 1;
	for (size_t i = 0; i < batchNum - 1 && NextObj(end) != nullptr; ++i)
	{
		end = NextObj(end);
		++actualNum;
	}

	// 从 span 的空闲链表头部摘下一段对象。
	span->_freeList = NextObj(end);
	NextObj(end) = nullptr;
	span->_useCount += actualNum;

	// 如果这个 span 已经被取空了，就把它放到链表尾部，
	// 避免下一次又优先扫到它。
	if (span->_freeList == nullptr)
	{
		_spanLists[index].Erase(span);
		_spanLists[index].PushBack(span);
	}

	_spanLists[index]._mtx.unlock();
	return actualNum;
}

// 把一段对象归还给各自所属的 span。
//
// 这里会逐个对象处理，因为一段对象可能来自多个 span。
// 处理流程：
// 1. 通过地址反查对象所属的 span；
// 2. 把对象挂回 span 的自由链表；
// 3. useCount 减一；
// 4. 如果整个 span 已经完全空闲，再继续归还给 PageCache。
void CentralCache::ReleaseListToSpans(void* start, size_t size)
{
	size_t index = SizeClass::Index(size);
	_spanLists[index]._mtx.lock();

	while (start)
	{
		void* next = NextObj(start);

		Span* span = PageCache::GetInstance()->MapObjectToSpan(start);
		bool wasEmpty = (span->_freeList == nullptr);
		NextObj(start) = span->_freeList;
		span->_freeList = start;
		span->_useCount--;

		// 如果这个 span 已经没有任何对象在外部被使用，
		// 说明它整段页都可以继续往下归还给 PageCache 了。
		if (span->_useCount == 0)
		{
			_spanLists[index].Erase(span);
			span->_freeList = nullptr;
			span->_next = nullptr;
			span->_prev = nullptr;

			// 同样先释放桶锁，再进入 PageCache，避免锁顺序问题。
			_spanLists[index]._mtx.unlock();

			PageCache::GetInstance()->_pageMtx.lock();
			PageCache::GetInstance()->ReleaseSpanToPageCache(span);
			PageCache::GetInstance()->_pageMtx.unlock();

			_spanLists[index]._mtx.lock();
		}
		// 如果它之前是空的，现在重新有空闲对象了，
		// 就把它提到前面，方便下次分配优先命中。
		else if (wasEmpty && span != _spanLists[index].Begin())
		{
			_spanLists[index].Erase(span);
			_spanLists[index].PushFront(span);
		}

		start = next;
	}

	_spanLists[index]._mtx.unlock();
}
