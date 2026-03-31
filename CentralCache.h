#pragma once

#include "Common.h"

// CentralCache 是整个内存池的第二层缓存。
//
// 它的职责是：
// 1. 在线程之间共享 span；
// 2. 负责把 PageCache 提供的整段页切成小对象；
// 3. 承担 ThreadCache 和 PageCache 之间的批量调度。
//
// 每个 size class 对应一个 SpanList，
// 链表里放的是“已经被切成该大小对象”的 span。
class CentralCache
{
public:
	static CentralCache* GetInstance()
	{
		return &_sInst;
	}

	// 获取一个“当前还存在空闲对象”的 span。
	// 如果桶里没有可用 span，就向 PageCache 申请新的页并切分。
	Span* GetOneSpan(SpanList& list, size_t byte_size);

	// 从某个 span 上批量取下一段对象，交给 ThreadCache。
	size_t FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size);

	// 接收 ThreadCache 批量归还的一段对象，并把它们挂回各自所属的 span。
	void ReleaseListToSpans(void* start, size_t byte_size);

private:
	// 每个桶管理一个 size class 对应的一组 span。
	SpanList _spanLists[NFREELIST];

private:
	CentralCache()
	{}

	CentralCache(const CentralCache&) = delete;

	static CentralCache _sInst;
};
