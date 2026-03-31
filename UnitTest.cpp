#include "ObjectPool.h"
#include "ConcurrentAlloc.h"

// 线程 1：申请几次 6 字节对象，用来观察 TLS 下 ThreadCache 的创建和使用。
void Alloc1()
{
	for (size_t i = 0; i < 5; ++i)
	{
		void* ptr = ConcurrentAlloc(6);
		(void)ptr;
	}
}

// 线程 2：申请几次 7 字节对象，和线程 1 对比观察不同线程是否各自拥有缓存。
void Alloc2()
{
	for (size_t i = 0; i < 5; ++i)
	{
		void* ptr = ConcurrentAlloc(7);
		(void)ptr;
	}
}

// 验证 TLS 是否生效：
// 两个线程分别走一遍分配流程，调试时可以观察各自持有的 ThreadCache。
void TLSTest()
{
	std::thread t1(Alloc1);
	t1.join();

	std::thread t2(Alloc2);
	t2.join();
}

// 申请并释放几个 8 字节以内、但大小不同的小对象。
// 适合在调试器里单步观察：
// 1. ThreadCache 如何命中/回填；
// 2. CentralCache 如何切分 span；
// 3. 对象释放后如何逐步回收。
void TestConcurrentAlloc1()
{
	void* p1 = ConcurrentAlloc(6);
	void* p2 = ConcurrentAlloc(8);
	void* p3 = ConcurrentAlloc(1);
	void* p4 = ConcurrentAlloc(7);
	void* p5 = ConcurrentAlloc(8);
	void* p6 = ConcurrentAlloc(8);
	void* p7 = ConcurrentAlloc(8);

	cout << p1 << endl;
	cout << p2 << endl;
	cout << p3 << endl;
	cout << p4 << endl;
	cout << p5 << endl;
	cout << p6 << endl;
	cout << p7 << endl;

	ConcurrentFree(p1);
	ConcurrentFree(p2);
	ConcurrentFree(p3);
	ConcurrentFree(p4);
	ConcurrentFree(p5);
	ConcurrentFree(p6);
	ConcurrentFree(p7);
}

// 连续申请 1024 次小对象，基本会把第一个页切完。
// 第 1025 次再申请时，可以重点观察 PageCache 是否会切出新的 span。
void TestConcurrentAlloc2()
{
	for (size_t i = 0; i < 1024; ++i)
	{
		void* p1 = ConcurrentAlloc(6);
		cout << p1 << endl;
	}

	void* p2 = ConcurrentAlloc(8);
	cout << p2 << endl;
}

// 用地址右移 PAGE_SHIFT 的方式，验证页号和地址之间的换算关系。
void TestAddressShift()
{
	PAGE_ID id1 = 2000;
	PAGE_ID id2 = 2001;
	char* p1 = (char*)(id1 << PAGE_SHIFT);
	char* p2 = (char*)(id2 << PAGE_SHIFT);
	while (p1 < p2)
	{
		cout << (void*)p1 << ":" << ((PAGE_ID)p1 >> PAGE_SHIFT) << endl;
		p1 += 8;
	}
}

// 线程 1：多次申请 6 字节对象并释放。
// 适合用来观察多线程场景下小对象的分配输出。
void MultiThreadAlloc1()
{
	std::vector<void*> v;
	for (size_t i = 0; i < 10; ++i)
	{
		void* ptr = ConcurrentAlloc(6);
		cout << std::this_thread::get_id() << ptr << endl;
		v.push_back(ptr);
	}

	for (auto e : v)
	{
		ConcurrentFree(e);
	}
}

// 线程 2：多次申请 16 字节对象并释放。
void MultiThreadAlloc2()
{
	std::vector<void*> v;
	for (size_t i = 0; i < 10; ++i)
	{
		void* ptr = ConcurrentAlloc(16);
		cout << std::this_thread::get_id() << ptr << endl;
		v.push_back(ptr);
	}

	for (auto e : v)
	{
		ConcurrentFree(e);
	}
}

// 创建两个线程同时申请/释放小对象，
// 用来验证基础多线程场景下内存池逻辑是否正常。
void TestMultiThread()
{
	std::thread t1(MultiThreadAlloc1);
	std::thread t2(MultiThreadAlloc2);

	t1.join();
	t2.join();
}

// 测试大对象分配路径：
// 1. 257KB 会走“大对象但仍由 PageCache 管理”的路径；
// 2. 129 * 8KB 会走“超过桶管理范围，直接向系统申请页”的路径。
void BigAlloc()
{
	void* p1 = ConcurrentAlloc(257 * 1024);
	ConcurrentFree(p1);

	void* p2 = ConcurrentAlloc(129 * 8 * 1024);
	ConcurrentFree(p2);
}

// 如果需要手动调试这些单元测试，可以临时打开下面的 main。
// int main()
// {
// 	// TestObjectPool();
// 	// TLSTest();
//
// 	// TestConcurrentAlloc1();
// 	// TestAddressShift();
//
// 	// TestMultiThread();
//
// 	// BigAlloc();
//
// 	return 0;
// }
