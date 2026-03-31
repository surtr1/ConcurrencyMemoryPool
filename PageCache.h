#pragma once

#include "Common.h"
#include "ObjectPool.h"
#include "PageMap.h"

// PageCache 是第三层缓存，也是最接近“页管理”的一层。
//
// 它负责：
// 1. 管理不同页数的空闲 span；
// 2. 在需要时向系统申请新的页；
// 3. 维护“页号 -> Span*”映射，供 free 时反查归属；
// 4. 在 span 完全空闲时做相邻页合并，缓解外部碎片。
class PageCache
{
public:
	static PageCache* GetInstance()
	{
		return &_sInst;
	}

	// 根据对象地址反查它归属的 span。
	// 做法是先把地址右移 PAGE_SHIFT，得到所属页号，再查页映射表。
	MP_FORCEINLINE Span* MapObjectToSpan(void* obj)
	{
		PAGE_ID id = ((PAGE_ID)obj >> PAGE_SHIFT);
		Span* span = (Span*)_idSpanMap.get(id);
		assert(span != nullptr);
		return span;
	}

	// 归还一个 span，并尽量和前后相邻的空闲 span 合并。
	void ReleaseSpanToPageCache(Span* span);

	// 获取一个恰好包含 k 个页的 span。
	Span* NewSpan(size_t k);

	std::mutex _pageMtx;

private:
	// 下标 i 对应“恰好 i 页”的空闲 span 链表。
	SpanList _spanLists[NPAGES];
	ObjectPool<Span> _spanPool;

#ifdef _WIN64
	// 64 位地址空间下，页映射使用三级基数树，避免一级数组太大。
	TCMalloc_PageMap3<48 - PAGE_SHIFT> _idSpanMap;
#else
	TCMalloc_PageMap3<32 - PAGE_SHIFT> _idSpanMap;
#endif

	PageCache()
	{}

	PageCache(const PageCache&) = delete;

	static PageCache _sInst;
};
