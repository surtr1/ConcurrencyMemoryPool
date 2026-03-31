#pragma once

#include "Common.h"

// ObjectPool 是一个“定类型对象池”：
// 1. 只负责管理某一种固定类型 T；
// 2. 申请对象时优先复用旧对象；
// 3. 不够时再向系统要一大块内存切分。
//
// 在这个项目里，它主要不是给用户对象用的，
// 而是给内部元数据对象用的，比如 Span、PageMap 的 Node / Leaf。
template<class T>
class ObjectPool
{
public:
	// 申请一个 T 对象。
	// 整体流程：
	// 1. 先看自由链表里有没有之前归还的对象；
	// 2. 如果没有，就从当前大块内存里切一个；
	// 3. 如果大块内存也不够了，再向系统申请新的大块；
	// 4. 最后用定位 new 调用构造函数。
	T* New()
	{
		T* obj = nullptr;

		if (_freeList)
		{
			void* next = *((void**)_freeList);
			obj = (T*)_freeList;
			_freeList = next;
		}
		else
		{
			// 当剩余空间连一个对象都放不下时，重新向系统申请一块 128KB 的空间。
			if (_remainBytes < sizeof(T))
			{
				_remainBytes = 128 * 1024;
				_memory = (char*)SystemAlloc(_remainBytes >> PAGE_SHIFT);
				if (_memory == nullptr)
				{
					throw std::bad_alloc();
				}
			}

			obj = (T*)_memory;

			// 至少按指针大小切分，这样对象归还后才能把自己挂到自由链表里。
			size_t objSize = sizeof(T) < sizeof(void*) ? sizeof(void*) : sizeof(T);
			_memory += objSize;
			_remainBytes -= objSize;
		}

		new(obj)T;
		return obj;
	}

	// 归还一个对象。
	// 注意这里不是直接 free，而是：
	// 1. 先显式调用析构函数；
	// 2. 再把这块内存头插回自由链表，等待下次复用。
	void Delete(T* obj)
	{
		obj->~T();

		*(void**)obj = _freeList;
		_freeList = obj;
	}

private:
	char* _memory = nullptr;      // 当前大块内存的游标位置。
	size_t _remainBytes = 0;      // 当前大块内存剩余多少字节可切分。
	void* _freeList = nullptr;    // 已归还对象组成的自由链表。
};

// 下面保留了一段对象池测试样例，默认注释掉，方便调试时手动打开。
//
// struct TreeNode
// {
// 	int _val;
// 	TreeNode* _left;
// 	TreeNode* _right;
//
// 	TreeNode()
// 		:_val(0)
// 		, _left(nullptr)
// 		, _right(nullptr)
// 	{}
// };
//
// void TestObjectPool()
// {
// 	// 测试轮次。
// 	const size_t Rounds = 5;
// 	// 每轮申请/释放多少个对象。
// 	const size_t N = 100000;
//
// 	std::vector<TreeNode*> v1;
// 	v1.reserve(N);
//
// 	size_t begin1 = clock();
// 	for (size_t j = 0; j < Rounds; ++j)
// 	{
// 		for (int i = 0; i < N; ++i)
// 		{
// 			v1.push_back(new TreeNode);
// 		}
// 		for (int i = 0; i < N; ++i)
// 		{
// 			delete v1[i];
// 		}
// 		v1.clear();
// 	}
// 	size_t end1 = clock();
//
// 	std::vector<TreeNode*> v2;
// 	v2.reserve(N);
//
// 	ObjectPool<TreeNode> TNPool;
// 	size_t begin2 = clock();
// 	for (size_t j = 0; j < Rounds; ++j)
// 	{
// 		for (int i = 0; i < N; ++i)
// 		{
// 			v2.push_back(TNPool.New());
// 		}
// 		for (int i = 0; i < N; ++i)
// 		{
// 			TNPool.Delete(v2[i]);
// 		}
// 		v2.clear();
// 	}
// 	size_t end2 = clock();
//
// 	cout << "new cost time:" << end1 - begin1 << endl;
// 	cout << "object pool cost time:" << end2 - begin2 << endl;
// }
