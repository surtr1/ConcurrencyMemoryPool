#include "PageCache.h"

PageCache PageCache::_sInst;

// 获取一个恰好 k 页的 span。
//
// 处理顺序是：
// 1. 先看“恰好 k 页”的桶里有没有现成的；
// 2. 没有就尝试把更大的 span 拆开；
// 3. 还没有，就向系统申请一块大页，再递归回来继续切。
Span* PageCache::NewSpan(size_t k)
{
	assert(k > 0);

	// 超过 128 页的大块请求不再走桶管理，直接向系统申请。
	if (k > NPAGES - 1)
	{
		void* ptr = SystemAlloc(k);
		Span* span = _spanPool.New();

		span->_pageId = (PAGE_ID)ptr >> PAGE_SHIFT;
		span->_n = k;

		// 大块 span 不会再细分，所以这里只需要记录起始页到 span 的映射。
		_idSpanMap.set(span->_pageId, span);

		return span;
	}

	// 第一优先级：直接从恰好 k 页的桶里取。
	if (!_spanLists[k].Empty())
	{
		Span* kSpan = _spanLists[k].PopFront();

		// 小对象 span 后续会通过任意对象地址反查所属 span，
		// 所以这里要把这段 span 覆盖的每一页都映射到它自己。
		for (PAGE_ID i = 0; i < kSpan->_n; ++i)
		{
			_idSpanMap.set(kSpan->_pageId + i, kSpan);
		}

		return kSpan;
	}

	// 第二优先级：从更大的 span 中切一段出来。
	for (size_t i = k + 1; i < NPAGES; ++i)
	{
		if (!_spanLists[i].Empty())
		{
			Span* nSpan = _spanLists[i].PopFront();
			Span* kSpan = _spanPool.New();

			// 前半段切出来作为 k 页的返回结果。
			kSpan->_pageId = nSpan->_pageId;
			kSpan->_n = k;

			// 剩余部分继续作为更小的新 span 留在 PageCache。
			nSpan->_pageId += k;
			nSpan->_n -= k;

			_spanLists[nSpan->_n].PushFront(nSpan);

			// 对剩余 span，只需要记录首尾页，方便后续做相邻合并判断。
			_idSpanMap.set(nSpan->_pageId, nSpan);
			_idSpanMap.set(nSpan->_pageId + nSpan->_n - 1, nSpan);

			// 对切出来返回的这段 span，需要把每一页都映射上，
			// 这样它被切成小对象以后，任意对象地址都能反查所属 span。
			for (PAGE_ID j = 0; j < kSpan->_n; ++j)
			{
				_idSpanMap.set(kSpan->_pageId + j, kSpan);
			}

			return kSpan;
		}
	}

	// 第三优先级：PageCache 里实在没有可拆的大块了，
	// 就先向系统申请一整段 128 页的大块，再回到前面的逻辑继续切。
	Span* bigSpan = _spanPool.New();
	void* ptr = SystemAlloc(NPAGES - 1);
	bigSpan->_pageId = (PAGE_ID)ptr >> PAGE_SHIFT;
	bigSpan->_n = NPAGES - 1;

	_spanLists[bigSpan->_n].PushFront(bigSpan);

	return NewSpan(k);
}

// 归还一个 span 时，尽量向前、向后和相邻空闲 span 合并。
//
// 合并的核心目的有两个：
// 1. 减少页级外部碎片；
// 2. 让未来大页请求更容易命中，而不是再次向系统申请。
void PageCache::ReleaseSpanToPageCache(Span* span)
{
	// 超过桶管理范围的大块，直接归还给系统。
	if (span->_n > NPAGES - 1)
	{
		void* ptr = (void*)(span->_pageId << PAGE_SHIFT);
		_idSpanMap.set(span->_pageId, nullptr);
		SystemFree(ptr);
		_spanPool.Delete(span);

		return;
	}

	// 尝试向前合并。
	while (1)
	{
		PAGE_ID prevId = span->_pageId - 1;
		Span* prevSpan = (Span*)_idSpanMap.get(prevId);
		if (prevSpan == nullptr)
		{
			break;
		}

		// 邻居还在使用中，不能合并。
		if (prevSpan->_isUse == true)
		{
			break;
		}

		// 合并后如果超过 PageCache 的桶管理上限，也不再合并。
		if (prevSpan->_n + span->_n > NPAGES - 1)
		{
			break;
		}

		span->_pageId = prevSpan->_pageId;
		span->_n += prevSpan->_n;

		_spanLists[prevSpan->_n].Erase(prevSpan);
		_spanPool.Delete(prevSpan);
	}

	// 尝试向后合并。
	while (1)
	{
		PAGE_ID nextId = span->_pageId + span->_n;
		Span* nextSpan = (Span*)_idSpanMap.get(nextId);
		if (nextSpan == nullptr)
		{
			break;
		}

		if (nextSpan->_isUse == true)
		{
			break;
		}

		if (nextSpan->_n + span->_n > NPAGES - 1)
		{
			break;
		}

		span->_n += nextSpan->_n;

		_spanLists[nextSpan->_n].Erase(nextSpan);
		_spanPool.Delete(nextSpan);
	}

	// 合并完成后，把这段空闲 span 重新挂回对应页数的桶。
	_spanLists[span->_n].PushFront(span);
	span->_isUse = false;
	span->_objSize = 0;
	span->_useCount = 0;

	// 对空闲 span，只需要维护首尾页到 span 的映射，
	// 因为合并时我们只关心前一页和后一页分别落在哪个 span 上。
	_idSpanMap.set(span->_pageId, span);
	_idSpanMap.set(span->_pageId + span->_n - 1, span);
}
