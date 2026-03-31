#include "ThreadCache.h"

#include "CentralCache.h"

// 当线程本地某条自由链表耗尽时，就会走到这里。
//
// 主要逻辑：
// 1. 先根据 size 算出理论上一批应该搬多少个对象；
// 2. 再结合 MaxSize 做慢启动，避免第一次就拿太多；
// 3. 从 CentralCache 真正取回一批对象；
// 4. 返回其中一个给当前请求，剩余对象挂回线程本地链表。
void* ThreadCache::FetchFromCentralCache(size_t index, size_t size)
{
	FreeList& list = _freeLists[index];

	// moveNum 是这个 size class 理论上的单次搬运上限。
	size_t moveNum = SizeClass::NumMoveSize(size);

	// batchNum 是这一次实际想取多少，采用慢启动策略逐步增大。
	size_t batchNum = (std::min)(list.MaxSize(), moveNum);

	// 如果当前慢启动值还没达到理论上限，就逐步增长。
	if (list.MaxSize() < moveNum)
	{
		++list.MaxSize();
	}

	// 同步更新线程本地缓存上限。
	size_t targetCacheSize = SizeClass::ThreadCacheMaxSize(size);
	if (list.MaxCacheSize() < targetCacheSize)
	{
		list.MaxCacheSize() = targetCacheSize;
	}

	void* start = nullptr;
	void* end = nullptr;
	size_t actualNum = CentralCache::GetInstance()->FetchRangeObj(start, end, batchNum, size);
	assert(actualNum > 0);

	// 如果中央缓存只给了一个对象，就直接返回给调用方。
	if (actualNum == 1)
	{
		assert(start == end);
		return start;
	}

	// 否则把第一个对象直接返回，剩余对象继续挂回本线程的自由链表，
	// 这样后面的同类请求就可以直接命中 ThreadCache。
	list.PushRange(NextObj(start), end, actualNum - 1);
	return start;
}

// 当某条线程本地自由链表过长时，批量回吐一部分对象给 CentralCache。
//
// 这一步的目的是防止：
// 1. 某个线程囤积太多对象不放；
// 2. 导致其他线程频繁向中央缓存要对象；
// 3. 进而影响整体内存利用率。
void ThreadCache::ListTooLong(FreeList& list, size_t size)
{
	size_t releaseNum = (std::min)(list.Size(), SizeClass::NumMoveSize(size));

	void* start = nullptr;
	void* end = nullptr;
	list.PopRange(start, end, releaseNum);

	CentralCache::GetInstance()->ReleaseListToSpans(start, size);
}
