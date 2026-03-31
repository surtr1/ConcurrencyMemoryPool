#pragma once

#include "Common.h"

// PageMap 的职责是：
// 给定一个对象地址所属的“页号”，快速找到它归属的 Span。
//
// 之所以不直接用 std::map / unordered_map，
// 是因为这条路径在 free 时非常热，需要尽量降低查找成本。
// 这里保留了 1 级 / 2 级 / 3 级三种页映射结构，项目当前主要使用三级映射。

// 一级页号映射：
// 直接拿一个大数组按页号索引。
// 优点是查询最快；
// 缺点是需要预留一整块大数组，对地址空间利用率要求高。
template <int BITS>
class TCMalloc_PageMap1 {
private:
	static const int LENGTH = 1 << BITS;
	void** array_;

public:
	typedef uintptr_t Number;

	explicit TCMalloc_PageMap1()
	{
		size_t size = sizeof(void*) * LENGTH;
		size_t alignSize = SizeClass::_RoundUp(size, (size_t)1 << PAGE_SHIFT);
		array_ = (void**)SystemAlloc(alignSize >> PAGE_SHIFT);
		memset(array_, 0, size);
	}

	// 查询页号对应的映射值。
	void* get(Number k) const
	{
		if ((k >> BITS) > 0) {
			return NULL;
		}
		return array_[k];
	}

	// 写入页号对应的映射值。
	void set(Number k, void* v)
	{
		array_[k] = v;
	}
};

// 二级基数树：
// 相比一级数组更节省空间，只在需要时创建叶子节点。
template <int BITS>
class TCMalloc_PageMap2 {
private:
	static const int ROOT_BITS = 5;
	static const int ROOT_LENGTH = 1 << ROOT_BITS;

	static const int LEAF_BITS = BITS - ROOT_BITS;
	static const int LEAF_LENGTH = 1 << LEAF_BITS;

	struct Leaf {
		void* values[LEAF_LENGTH];
	};

	Leaf* root_[ROOT_LENGTH];

public:
	typedef uintptr_t Number;

	explicit TCMalloc_PageMap2()
	{
		memset(root_, 0, sizeof(root_));
		PreallocateMoreMemory();
	}

	void* get(Number k) const
	{
		const Number i1 = k >> LEAF_BITS;
		const Number i2 = k & (LEAF_LENGTH - 1);
		if ((k >> BITS) > 0 || root_[i1] == NULL) {
			return NULL;
		}
		return root_[i1]->values[i2];
	}

	void set(Number k, void* v)
	{
		const Number i1 = k >> LEAF_BITS;
		const Number i2 = k & (LEAF_LENGTH - 1);
		assert(i1 < ROOT_LENGTH);
		root_[i1]->values[i2] = v;
	}

	// 确保 [start, start + n) 这段页号范围所需的叶子节点都已存在。
	bool Ensure(Number start, size_t n)
	{
		for (Number key = start; key <= start + n - 1;) {
			const Number i1 = key >> LEAF_BITS;
			if (i1 >= ROOT_LENGTH) {
				return false;
			}

			if (root_[i1] == NULL) {
				static ObjectPool<Leaf> leafPool;
				Leaf* leaf = leafPool.New();
				memset(leaf, 0, sizeof(*leaf));
				root_[i1] = leaf;
			}

			key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
		}
		return true;
	}

	void PreallocateMoreMemory()
	{
		Ensure(0, ((size_t)1 << BITS));
	}
};

// 三级基数树：
// 进一步提高空间利用率，适合更大的地址空间。
// 当前工程在 x64 下主要使用它来存“页号 -> Span*”映射。
template <int BITS>
class TCMalloc_PageMap3 {
private:
	static const int INTERIOR_BITS = (BITS + 2) / 3;
	static const int INTERIOR_LENGTH = 1 << INTERIOR_BITS;

	static const int LEAF_BITS = BITS - 2 * INTERIOR_BITS;
	static const int LEAF_LENGTH = 1 << LEAF_BITS;

	// 中间层节点：只负责把查询继续往下一层转发。
	struct Node {
		std::atomic<Node*> ptrs[INTERIOR_LENGTH];
	};

	// 叶子层节点：真正保存最终的映射值。
	struct Leaf {
		std::atomic<void*> values[LEAF_LENGTH];
	};

	Node* root_;

	// 创建一个新的中间层节点，并把所有槽位初始化为空。
	static Node* NewNode()
	{
		static ObjectPool<Node> nodePool;
		Node* result = nodePool.New();
		for (int i = 0; i < INTERIOR_LENGTH; ++i)
		{
			result->ptrs[i].store(nullptr, std::memory_order_relaxed);
		}
		return result;
	}

	// 创建一个新的叶子节点，并把所有值初始化为空。
	static Leaf* NewLeaf()
	{
		static ObjectPool<Leaf> leafPool;
		Leaf* result = leafPool.New();
		for (int i = 0; i < LEAF_LENGTH; ++i)
		{
			result->values[i].store(nullptr, std::memory_order_relaxed);
		}
		return result;
	}

public:
	typedef uintptr_t Number;

	explicit TCMalloc_PageMap3()
	{
		root_ = NewNode();
	}

	// 查询流程：
	// 1. 先把页号拆成三级下标；
	// 2. 按层级依次下钻；
	// 3. 任意一层不存在，就说明该页号当前没有映射。
	void* get(Number k) const
	{
		const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
		const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
		const Number i3 = k & (LEAF_LENGTH - 1);

		if ((k >> BITS) > 0) {
			return NULL;
		}

		Node* node = root_->ptrs[i1].load(std::memory_order_acquire);
		if (node == NULL) {
			return NULL;
		}

		Node* leaf = node->ptrs[i2].load(std::memory_order_acquire);
		if (leaf == NULL) {
			return NULL;
		}

		return reinterpret_cast<Leaf*>(leaf)->values[i3].load(std::memory_order_acquire);
	}

	// 写入流程：
	// 1. 同样先拆三级下标；
	// 2. 如果中间层或叶子层不存在，就按需创建；
	// 3. 最后把值写到叶子节点里。
	void set(Number k, void* v)
	{
		assert((k >> BITS) == 0);
		const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
		const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
		const Number i3 = k & (LEAF_LENGTH - 1);

		Node* node = root_->ptrs[i1].load(std::memory_order_acquire);
		if (node == NULL) {
			node = NewNode();
			root_->ptrs[i1].store(node, std::memory_order_release);
		}

		Node* leaf = node->ptrs[i2].load(std::memory_order_acquire);
		if (leaf == NULL) {
			leaf = reinterpret_cast<Node*>(NewLeaf());
			node->ptrs[i2].store(leaf, std::memory_order_release);
		}

		reinterpret_cast<Leaf*>(leaf)->values[i3].store(v, std::memory_order_release);
	}

	// 保证一段页号范围对应的路径都已存在。
	// 当前项目里主要是为了在某些场景下提前把映射节点准备好。
	bool Ensure(Number start, size_t n)
	{
		for (Number key = start; key <= start + n - 1;) {
			const Number i1 = key >> (LEAF_BITS + INTERIOR_BITS);
			const Number i2 = (key >> LEAF_BITS) & (INTERIOR_LENGTH - 1);

			if (i1 >= INTERIOR_LENGTH || i2 >= INTERIOR_LENGTH) {
				return false;
			}

			Node* node = root_->ptrs[i1].load(std::memory_order_acquire);
			if (node == NULL) {
				node = NewNode();
				root_->ptrs[i1].store(node, std::memory_order_release);
			}

			if (node->ptrs[i2].load(std::memory_order_acquire) == NULL) {
				node->ptrs[i2].store(reinterpret_cast<Node*>(NewLeaf()), std::memory_order_release);
			}

			key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
		}
		return true;
	}

	// 三级映射当前采用按需创建节点的方式，所以这里不主动做预分配。
	void PreallocateMoreMemory()
	{
	}
};
