#include <gtest/gtest.h>

#ifdef RADRAY_ENABLE_D3D12

#include <cstdint>
#include <limits>
#include <random>
#include <unordered_set>
#include <vector>

#include <radray/render/backend/d3d12_impl.h>

namespace {

using radray::render::d3d12::ComPtr;

static ComPtr<ID3D12Device> CreateD3D12DeviceOrNull() {
	ComPtr<ID3D12Device> device;
	HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf()));
	if (FAILED(hr) || device == nullptr) {
		return {};
	}
	return device;
}

static std::unordered_set<const void*> CollectHeapNativePtrs(std::span<const radray::render::d3d12::CpuDescriptorAllocator::Allocation> allocs) {
	std::unordered_set<const void*> heaps;
	heaps.reserve(allocs.size());
	for (const auto& a : allocs) {
		if (a.Heap != nullptr) {
			heaps.insert(static_cast<const void*>(a.Heap->Get()));
		}
	}
	return heaps;
}

// Tag heaps to robustly detect reuse vs. recreation.
// Pointer addresses are not a reliable indicator on their own.
static const GUID kHeapReuseTagGuid = {0x7b6f73bb, 0x28f2, 0x4c55, {0x9c, 0x21, 0x1c, 0x52, 0x4b, 0x6a, 0x2f, 0x13}};

static bool HeapHasReuseTag(ID3D12DescriptorHeap* heap) {
	if (heap == nullptr) {
		return false;
	}
	uint64_t value = 0;
	UINT size = sizeof(value);
	const HRESULT hr = heap->GetPrivateData(kHeapReuseTagGuid, &size, &value);
	return SUCCEEDED(hr) && size == sizeof(value);
}

}  // namespace

TEST(D3D12_CpuDescriptorAllocator, AllocateZeroReturnsNullopt) {
	auto device = CreateD3D12DeviceOrNull();
	if (!device) {
		GTEST_SKIP() << "D3D12CreateDevice failed; skipping.";
	}

	radray::render::d3d12::CpuDescriptorAllocator alloc{
		device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64u, 1u};
	auto a = alloc.Allocate(0);
	EXPECT_FALSE(a.has_value());
}

TEST(D3D12_CpuDescriptorAllocator, OversizedBitCeilPageCapacityReturnsNullopt) {
	auto device = CreateD3D12DeviceOrNull();
	if (!device) {
		GTEST_SKIP() << "D3D12CreateDevice failed; skipping.";
	}

	radray::render::d3d12::CpuDescriptorAllocator alloc{
		device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1u, 1u};

	// For 32-bit UINT, request > 2^31 makes bit_ceil(request) become 2^32,
	// which exceeds UINT::max and should be rejected.
	constexpr UINT huge = 0x80000001u;
	auto a = alloc.Allocate(huge);
	EXPECT_FALSE(a.has_value());
}

TEST(D3D12_CpuDescriptorAllocator, BasicAllocateDestroyAndReuseSameHeapWhenKept) {
	auto device = CreateD3D12DeviceOrNull();
	if (!device) {
		GTEST_SKIP() << "D3D12CreateDevice failed; skipping.";
	}

	// keepFreePages = max -> never releases empty pages.
	radray::render::d3d12::CpuDescriptorAllocator alloc{
		device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 64u, std::numeric_limits<uint32_t>::max()};

	auto a1 = alloc.Allocate(1);
	ASSERT_TRUE(a1.has_value());
	ASSERT_NE(a1->Heap, nullptr);
	const void* heapPtr = a1->Heap->Get();
	EXPECT_NE(heapPtr, nullptr);

	alloc.Destroy(*a1);

	auto a2 = alloc.Allocate(1);
	ASSERT_TRUE(a2.has_value());
	ASSERT_NE(a2->Heap, nullptr);
	EXPECT_EQ(a2->Heap->Get(), heapPtr);
	alloc.Destroy(*a2);
}

TEST(D3D12_CpuDescriptorAllocator, PageCapacityRoundingUsesBasicSizeAtLeast) {
	auto device = CreateD3D12DeviceOrNull();
	if (!device) {
		GTEST_SKIP() << "D3D12CreateDevice failed; skipping.";
	}

	// basicSize=63 -> page capacity should be bit_ceil(63)=64 even if requesting 1.
	radray::render::d3d12::CpuDescriptorAllocator alloc{
		device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 63u, 1u};

	auto a = alloc.Allocate(1);
	ASSERT_TRUE(a.has_value());
	ASSERT_NE(a->Heap, nullptr);
	EXPECT_EQ(a->Heap->GetLength(), 64u);
	alloc.Destroy(*a);
}

TEST(D3D12_CpuDescriptorAllocator, PageCapacityRoundingUsesRequestWhenLargerThanBasicSize) {
	auto device = CreateD3D12DeviceOrNull();
	if (!device) {
		GTEST_SKIP() << "D3D12CreateDevice failed; skipping.";
	}

	// request=65 -> desired=65 -> page capacity bit_ceil(65)=128
	radray::render::d3d12::CpuDescriptorAllocator alloc{
		device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64u, 1u};

	auto a = alloc.Allocate(65);
	ASSERT_TRUE(a.has_value());
	ASSERT_NE(a->Heap, nullptr);
	EXPECT_EQ(a->Heap->GetLength(), 128u);
	alloc.Destroy(*a);
}

TEST(D3D12_CpuDescriptorAllocator, FragmentationAndCoalescingWithinOnePage) {
	auto device = CreateD3D12DeviceOrNull();
	if (!device) {
		GTEST_SKIP() << "D3D12CreateDevice failed; skipping.";
	}

	// Use keep=max so pages are never released and we can observe reuse.
	radray::render::d3d12::CpuDescriptorAllocator alloc{
		device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 64u, std::numeric_limits<uint32_t>::max()};

	// Allocate 4 blocks of 16 => should fill one 64-capacity page.
	auto a1 = alloc.Allocate(16);
	auto a2 = alloc.Allocate(16);
	auto a3 = alloc.Allocate(16);
	auto a4 = alloc.Allocate(16);
	ASSERT_TRUE(a1 && a2 && a3 && a4);
	ASSERT_NE(a1->Heap, nullptr);
	EXPECT_EQ(a1->Heap, a2->Heap);
	EXPECT_EQ(a2->Heap, a3->Heap);
	EXPECT_EQ(a3->Heap, a4->Heap);

	auto* heap0 = a1->Heap;

	// Verify buddy coalescing: find a buddy pair among the 16-sized allocations.
	// For block size 16, buddies have offsets that differ by xor 16.
	radray::render::d3d12::CpuDescriptorAllocator::Allocation blocks[4] = {*a1, *a2, *a3, *a4};
	int iBuddy = -1;
	int jBuddy = -1;
	for (int i = 0; i < 4 && iBuddy < 0; ++i) {
		for (int j = i + 1; j < 4; ++j) {
			if ((blocks[i].Start ^ 16u) == blocks[j].Start) {
				iBuddy = i;
				jBuddy = j;
				break;
			}
		}
	}
	ASSERT_GE(iBuddy, 0);
	ASSERT_GE(jBuddy, 0);

	const UINT expected32Start = std::min(blocks[iBuddy].Start, blocks[jBuddy].Start);
	alloc.Destroy(blocks[iBuddy]);
	alloc.Destroy(blocks[jBuddy]);

	// Now a 32 allocation should be able to reuse the same page at the coalesced offset.
	auto a32 = alloc.Allocate(32);
	ASSERT_TRUE(a32.has_value());
	ASSERT_NE(a32->Heap, nullptr);
	EXPECT_EQ(a32->Heap, heap0);
	EXPECT_EQ(a32->Start, expected32Start);

	// Cleanup remaining 16 blocks (those not in buddy pair) and the 32 block.
	for (int i = 0; i < 4; ++i) {
		if (i == iBuddy || i == jBuddy) {
			continue;
		}
		alloc.Destroy(blocks[i]);
	}
	alloc.Destroy(*a32);

	// Fully free: should be able to allocate whole 64 from the original heap.
	auto a64 = alloc.Allocate(64);
	ASSERT_TRUE(a64.has_value());
	ASSERT_NE(a64->Heap, nullptr);
	EXPECT_EQ(a64->Heap, heap0);
	EXPECT_EQ(a64->Start, 0u);
	alloc.Destroy(*a64);
}

TEST(D3D12_CpuDescriptorAllocator, DestroyInvalidIsNoop) {
	auto device = CreateD3D12DeviceOrNull();
	if (!device) {
		GTEST_SKIP() << "D3D12CreateDevice failed; skipping.";
	}

	radray::render::d3d12::CpuDescriptorAllocator alloc{
		device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64u, 1u};

	radray::render::d3d12::CpuDescriptorAllocator::Allocation invalid =
		radray::render::d3d12::CpuDescriptorAllocator::Allocation::Invalid();
	EXPECT_NO_THROW(alloc.Destroy(invalid));
}

TEST(D3D12_CpuDescriptorAllocator, KeepFreePagesMaxKeepsAndReusesOldHeaps) {
	auto device = CreateD3D12DeviceOrNull();
	if (!device) {
		GTEST_SKIP() << "D3D12CreateDevice failed; skipping.";
	}

	radray::render::d3d12::CpuDescriptorAllocator alloc{
		device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 32u, std::numeric_limits<uint32_t>::max()};

	// Force multiple pages by allocating full pages.
	std::vector<radray::render::d3d12::CpuDescriptorAllocator::Allocation> allocs;
	for (int i = 0; i < 3; ++i) {
		auto a = alloc.Allocate(32);
		ASSERT_TRUE(a.has_value());
		allocs.push_back(*a);
	}
	{
		auto oldHeaps = CollectHeapNativePtrs(allocs);
		EXPECT_GE(oldHeaps.size(), 2u);
	}

	// Keep strong refs to the unique heaps we created, and tag them.
	std::vector<ComPtr<ID3D12DescriptorHeap>> oldHeapRefs;
	std::unordered_set<const void*> seen;
	for (const auto& a : allocs) {
		auto* p = (a.Heap != nullptr) ? a.Heap->Get() : nullptr;
		if (p != nullptr && seen.insert(static_cast<const void*>(p)).second) {
			oldHeapRefs.push_back(p);
		}
	}
	ASSERT_GE(oldHeapRefs.size(), 2u);
	for (size_t i = 0; i < oldHeapRefs.size(); ++i) {
		const uint64_t tag = 0xA11C0C11u + static_cast<uint64_t>(i);
		ASSERT_TRUE(SUCCEEDED(oldHeapRefs[i]->SetPrivateData(kHeapReuseTagGuid, sizeof(tag), &tag)));
	}

	for (auto& a : allocs) {
		alloc.Destroy(a);
	}

	// With keep=max, pages should remain cached; a new allocation should reuse one of them.
	auto b = alloc.Allocate(1);
	ASSERT_TRUE(b.has_value());
	ASSERT_NE(b->Heap, nullptr);
	EXPECT_TRUE(HeapHasReuseTag(b->Heap->Get()));
	alloc.Destroy(*b);
}

TEST(D3D12_CpuDescriptorAllocator, KeepFreePagesZeroReleasesAllFreePages) {
	auto device = CreateD3D12DeviceOrNull();
	if (!device) {
		GTEST_SKIP() << "D3D12CreateDevice failed; skipping.";
	}

	radray::render::d3d12::CpuDescriptorAllocator alloc{
		device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 32u, 0u};

	std::vector<radray::render::d3d12::CpuDescriptorAllocator::Allocation> allocs;
	for (int i = 0; i < 3; ++i) {
		auto a = alloc.Allocate(32);
		ASSERT_TRUE(a.has_value());
		allocs.push_back(*a);
	}
	{
		auto oldHeaps = CollectHeapNativePtrs(allocs);
		EXPECT_GE(oldHeaps.size(), 2u);
	}

	// Keep strong refs to the unique heaps we created, and tag them.
	std::vector<ComPtr<ID3D12DescriptorHeap>> oldHeapRefs;
	std::unordered_set<const void*> seen;
	for (const auto& a : allocs) {
		auto* p = (a.Heap != nullptr) ? a.Heap->Get() : nullptr;
		if (p != nullptr && seen.insert(static_cast<const void*>(p)).second) {
			oldHeapRefs.push_back(p);
		}
	}
	ASSERT_GE(oldHeapRefs.size(), 2u);
	for (size_t i = 0; i < oldHeapRefs.size(); ++i) {
		const uint64_t tag = 0xDEADBEAFu + static_cast<uint64_t>(i);
		ASSERT_TRUE(SUCCEEDED(oldHeapRefs[i]->SetPrivateData(kHeapReuseTagGuid, sizeof(tag), &tag)));
	}

	for (auto& a : allocs) {
		alloc.Destroy(a);
	}

	// keep=0 means all free pages should be released. Next alloc should create a new heap.
	auto b = alloc.Allocate(1);
	ASSERT_TRUE(b.has_value());
	ASSERT_NE(b->Heap, nullptr);
	EXPECT_FALSE(HeapHasReuseTag(b->Heap->Get()));
	alloc.Destroy(*b);
}

TEST(D3D12_CpuDescriptorAllocator, RandomStressAllocFreeNoCrash) {
	auto device = CreateD3D12DeviceOrNull();
	if (!device) {
		GTEST_SKIP() << "D3D12CreateDevice failed; skipping.";
	}

	radray::render::d3d12::CpuDescriptorAllocator alloc{
		device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 256u, 1u};

	std::mt19937 rng(42);
	std::vector<radray::render::d3d12::CpuDescriptorAllocator::Allocation> live;
	live.reserve(2048);

	for (int i = 0; i < 3000; ++i) {
		const bool doAlloc = live.empty() || (std::uniform_int_distribution<>(0, 2)(rng) != 0);
		if (doAlloc) {
			UINT sz = std::uniform_int_distribution<UINT>(1u, 512u)(rng);
			auto a = alloc.Allocate(sz);
			if (a.has_value()) {
				EXPECT_NE(a->Heap, nullptr);
				live.push_back(*a);
			}
		} else {
			size_t idx = std::uniform_int_distribution<size_t>(0, live.size() - 1)(rng);
			alloc.Destroy(live[idx]);
			live.erase(live.begin() + static_cast<std::ptrdiff_t>(idx));
		}
	}

	for (auto& a : live) {
		alloc.Destroy(a);
	}

	// Should still be functional after stress.
	auto final = alloc.Allocate(128);
	ASSERT_TRUE(final.has_value());
	alloc.Destroy(*final);
}

#else

TEST(D3D12_CpuDescriptorAllocator, Disabled) {
	GTEST_SKIP() << "RADRAY_ENABLE_D3D12 is disabled.";
}

#endif
