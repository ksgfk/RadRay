// Measurement boundaries and case selection: docs/guide/performance.md#sparse-set
#include <algorithm>
#include <numeric>
#include <random>
#include <string_view>

#include <benchmark/benchmark.h>

#include <radray/nullable.h>
#include <radray/sparse_set.h>

namespace {

using namespace radray;

template <size_t Bytes>
struct Payload {
    array<uint64_t, Bytes / sizeof(uint64_t)> Words;

    explicit Payload(uint64_t seed) noexcept {
        for (size_t i = 0; i < Words.size(); ++i) Words[i] = seed + i;
    }

    uint64_t Key() const noexcept { return Words[0]; }
};

struct Owned64 {
    unique_ptr<Payload<64>> Value;

    explicit Owned64(uint64_t seed) : Value(make_unique<Payload<64>>(seed)) {}
    uint64_t Key() const noexcept { return Value->Key(); }
};

enum class InsertMode { Fresh,
                        Reserved,
                        ReserveEach,
                        ReuseOrdered,
                        ReuseRandom };
enum class Order { Forward,
                   Reverse,
                   Random,
                   Stride };
enum class ReadMode { Dense,
                      Get,
                      TryGet,
                      IsAlive };
enum class MissKind { Invalid,
                      OutOfRange,
                      Dead,
                      Recycled };
enum class ChurnMode { Tail,
                       Random,
                       Burst };

bool Check(benchmark::State& state, bool& failed, bool condition, const char* message) {
    if (condition) return true;
    failed = true;
    state.SkipWithError(message);
    return false;
}

vector<uint32_t> MakeOrder(uint32_t count, Order order) {
    vector<uint32_t> result(count);
    std::iota(result.begin(), result.end(), 0u);
    if (order == Order::Reverse) {
        std::reverse(result.begin(), result.end());
    } else if (order == Order::Random) {
        std::mt19937 random{0x5EED1234u};
        // Explicit Fisher-Yates keeps the permutation independent of std::shuffle implementations.
        for (uint32_t i = count; i > 1; --i) std::swap(result[i - 1], result[random() % i]);
    } else if (order == Order::Stride) {
        // Registered counts are powers of two; the odd stride visits every index exactly once.
        for (uint32_t i = 0; i < count; ++i) result[i] = (i * 257u) % count;
    }
    return result;
}

template <class T>
void Fill(SparseSet<T>& set, vector<SparseSetHandle>& handles, uint32_t count) {
    handles.resize(count);
    for (uint32_t i = 0; i < count; ++i) handles[i] = set.Emplace(i + 1u);
}

template <class T>
bool Validate(const SparseSet<T>& set, const vector<SparseSetHandle>& handles) {
    if (set.Count() != handles.size()) return false;
    for (uint32_t i = 0; i < handles.size(); ++i) {
        Nullable<const T*> value = set.TryGet(handles[i]);
        if (!value || value.Get()->Key() != i + 1u) return false;
    }
    return true;
}

void Report(benchmark::State& state, int64_t operations, uint32_t live, uint32_t highWater, size_t payloadBytes) {
    state.SetItemsProcessed(state.iterations() * operations);
    state.counters["batch_ops"] = static_cast<double>(operations);
    state.counters["live"] = live;
    state.counters["high_water"] = highWater;
    state.counters["payload_bytes"] = static_cast<double>(payloadBytes);
}

template <class T>
void Insert(benchmark::State& state, bool& failed, InsertMode mode) {
    const auto count = static_cast<uint32_t>(state.range(0));
    const bool reuse = mode == InsertMode::ReuseOrdered || mode == InsertMode::ReuseRandom;
    const auto order = MakeOrder(count, mode == InsertMode::ReuseRandom ? Order::Random : Order::Reverse);
    vector<SparseSetHandle> handles(count);
    SparseSet<T> set;
    if (reuse) Fill(set, handles, count);
    benchmark::DoNotOptimize(&set);
    for (auto _ : state) {
        state.PauseTiming();
        if (reuse) {
            for (auto index : order) set.Destroy(handles[index]);
        } else {
            set = SparseSet<T>{};
            if (mode == InsertMode::Reserved) set.Reserve(count);
        }
        state.ResumeTiming();
        for (uint32_t i = 0; i < count; ++i) {
            if (mode == InsertMode::ReserveEach) set.Reserve(i + 1u);
            handles[i] = set.Emplace(i + 1u);
        }
        benchmark::DoNotOptimize(set.Values().data());
        benchmark::ClobberMemory();
    }
    Check(state, failed, Validate(set, handles), "inserted handles or payloads are invalid");
    Report(state, count, count, count, sizeof(T));

    // Untimed public-API probe: no production instrumentation or global allocation hooks.
    SparseSet<T> probe;
    if (mode == InsertMode::Reserved) probe.Reserve(count);
    if (reuse) {
        for (uint32_t i = 0; i < count; ++i) probe.Emplace(i + 1u);
        probe.Clear();
    }
    uint64_t relocations = 0;
    uint64_t moved = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto previous = reinterpret_cast<uintptr_t>(probe.Values().data());
        if (mode == InsertMode::ReserveEach) probe.Reserve(i + 1u);
        probe.Emplace(i + 1u);
        if (i != 0 && previous != reinterpret_cast<uintptr_t>(probe.Values().data())) {
            ++relocations;
            moved += i;
        }
    }
    state.counters["dense_relocations"] = static_cast<double>(relocations);
    state.counters["relocated_elements"] = static_cast<double>(moved);
}

template <class T>
void GrowthEdge(benchmark::State& state, bool& failed, bool grow) {
    const auto target = static_cast<uint32_t>(state.range(0));
    uint32_t prefix = 0;
    {
        SparseSet<T> probe;
        for (uint32_t i = 0; i <= target * 2u; ++i) {
            const auto previous = reinterpret_cast<uintptr_t>(probe.Values().data());
            probe.Emplace(i + 1u);
            if (i >= target && previous != reinterpret_cast<uintptr_t>(probe.Values().data())) {
                prefix = i;
                break;
            }
        }
    }
    if (!Check(state, failed, prefix != 0, "could not locate a dense growth boundary")) return;
    array<SparseSet<T>, 8> sets;
    array<SparseSetHandle, 8> inserted;
    auto prepare = [prefix](SparseSet<T>& set) {
        set = SparseSet<T>{};
        set.Reserve(prefix + 1u);
        for (uint32_t i = 0; i < prefix; ++i) set.Emplace(i + 1u);
    };
    if (!grow) {
        for (auto& set : sets) {
            prepare(set);
            set.Destroy(set.Emplace(prefix + 1u));
        }
    }
    benchmark::DoNotOptimize(sets.data());
    for (auto _ : state) {
        state.PauseTiming();
        for (size_t i = 0; i < sets.size(); ++i) {
            if (grow) {
                prepare(sets[i]);
            } else if (sets[i].Count() > prefix) {
                sets[i].Destroy(inserted[i]);
            }
        }
        state.ResumeTiming();
        for (size_t i = 0; i < sets.size(); ++i) inserted[i] = sets[i].Emplace(prefix + 1u);
        for (auto& set : sets) benchmark::DoNotOptimize(set.Values().data());
        benchmark::ClobberMemory();
    }
    for (size_t i = 0; i < sets.size(); ++i) {
        Nullable<const T*> value = std::as_const(sets[i]).TryGet(inserted[i]);
        Check(state, failed, sets[i].Count() == prefix + 1u && value && value.Get()->Key() == prefix + 1u,
              "growth boundary insertion failed");
    }
    state.counters["prefix"] = prefix;
    Report(state, sets.size(), prefix + 1u, prefix + 1u, sizeof(T));
}

template <class T>
void Read(benchmark::State& state, bool& failed, ReadMode mode, Order order, bool fragmented) {
    const auto count = static_cast<uint32_t>(state.range(0));
    const auto spread = static_cast<uint32_t>(state.range(1));
    const uint32_t peak = count * spread;
    SparseSet<T> set;
    vector<SparseSetHandle> all;
    Fill(set, all, peak);
    vector<SparseSetHandle> handles(count);
    for (uint32_t i = 0; i < count; ++i) handles[i] = all[i * spread];
    for (uint32_t i = peak; i > 0; --i) {
        if ((i - 1u) % spread != 0) set.Destroy(all[i - 1u]);
    }
    if (fragmented) {
        for (auto index : MakeOrder(count, Order::Random)) {
            set.Destroy(handles[index]);
            handles[index] = set.Emplace(index * spread + 1u);
        }
    }
    const auto indices = MakeOrder(count, order);
    vector<SparseSetHandle> queries(count);
    for (uint32_t i = 0; i < count; ++i) queries[i] = handles[indices[i]];
    all.clear();
    all.shrink_to_fit();
    uint64_t expected = 0;
    for (uint32_t i = 0; i < count; ++i) {
        Nullable<const T*> value = std::as_const(set).TryGet(handles[i]);
        if (!Check(state, failed, value && value.Get()->Key() == i * spread + 1u, "read fixture is invalid")) return;
        expected += i * spread + 1u;
    }
    uint64_t checksum = 0;
    benchmark::DoNotOptimize(&set);
    benchmark::DoNotOptimize(queries.data());
    for (auto _ : state) {
        benchmark::ClobberMemory();
        checksum = 0;
        switch (mode) {
            case ReadMode::Dense:
                for (const auto& value : set.Values()) checksum += value.Key();
                break;
            case ReadMode::Get:
                for (auto handle : queries) checksum += set.Get(handle).Key();
                break;
            case ReadMode::TryGet:
                for (auto handle : queries) {
                    Nullable<const T*> value = std::as_const(set).TryGet(handle);
                    if (value) checksum += value.Get()->Key();
                }
                break;
            case ReadMode::IsAlive:
                for (auto handle : queries) checksum += set.IsAlive(handle);
                break;
        }
        benchmark::DoNotOptimize(checksum);
    }
    Check(state, failed, checksum == (mode == ReadMode::IsAlive ? count : expected), "read checksum mismatch");
    Report(state, count, count, peak, sizeof(T));
}

void DependentRead(benchmark::State& state, bool& failed) {
    const auto count = static_cast<uint32_t>(state.range(0));
    SparseSet<Payload<8>> set;
    vector<SparseSetHandle> handles;
    Fill(set, handles, count);
    const auto order = MakeOrder(count, Order::Random);
    for (uint32_t i = 0; i < count; ++i) {
        const auto next = handles[order[(i + 1u) % count]];
        set.Get(handles[order[i]]).Words[0] = (static_cast<uint64_t>(next.Generation) << 32u) | next.Index;
    }
    auto current = handles[order[0]];
    uint64_t checksum = 0;
    benchmark::DoNotOptimize(&set);
    for (auto _ : state) {
        benchmark::ClobberMemory();
        checksum = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t packed = set.Get(current).Words[0];
            current = {static_cast<uint32_t>(packed), static_cast<uint32_t>(packed >> 32u)};
            checksum += current.Index;
        }
        benchmark::DoNotOptimize(checksum);
        benchmark::DoNotOptimize(current);
    }
    Check(state, failed, current == handles[order[0]] && checksum == static_cast<uint64_t>(count) * (count - 1u) / 2u,
          "dependent lookup did not traverse the complete cycle");
    Report(state, count, count, count, 8);
}

void Misses(benchmark::State& state, bool& failed, MissKind kind, bool shuffled) {
    const auto count = static_cast<uint32_t>(state.range(0));
    const auto percent = static_cast<uint32_t>(state.range(1));
    const uint32_t hits = count * percent / 100u;
    SparseSet<Payload<8>> set;
    vector<SparseSetHandle> handles;
    Fill(set, handles, count * 2u);
    for (uint32_t i = count; i < count * 2u; ++i) set.Destroy(handles[i]);
    if (kind == MissKind::Recycled) {
        for (uint32_t i = 0; i < count; ++i) set.Emplace(i + 1u);
    }
    vector<SparseSetHandle> queries(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (i < hits) {
            queries[i] = handles[i];
        } else {
            switch (kind) {
                case MissKind::Invalid: queries[i] = SparseSetHandle::Invalid(); break;
                case MissKind::OutOfRange: queries[i] = {count * 2u + i, 0}; break;
                case MissKind::Dead:
                case MissKind::Recycled: queries[i] = handles[count + i]; break;
            }
        }
    }
    if (shuffled) {
        const auto source = queries;
        const auto order = MakeOrder(count, Order::Random);
        for (uint32_t i = 0; i < count; ++i) queries[i] = source[order[i]];
    }
    const uint64_t expected = static_cast<uint64_t>(hits) * (hits + 1u) / 2u;
    uint64_t checksum = 0;
    uint32_t observedHits = 0;
    for (auto handle : queries) observedHits += set.IsAlive(handle);
    if (!Check(state, failed, observedHits == hits, "miss fixture has the wrong hit ratio")) return;
    benchmark::DoNotOptimize(&set);
    benchmark::DoNotOptimize(queries.data());
    for (auto _ : state) {
        benchmark::ClobberMemory();
        checksum = 0;
        for (auto handle : queries) {
            Nullable<const Payload<8>*> value = std::as_const(set).TryGet(handle);
            if (value) checksum += value.Get()->Key();
        }
        benchmark::DoNotOptimize(checksum);
    }
    Check(state, failed, checksum == expected, "mixed lookup checksum mismatch");
    state.counters["hit_percent"] = 100.0 * hits / count;
    Report(state, count, set.Count(), count * 2u, 8);
}

template <class T>
void Destroy(benchmark::State& state, bool& failed, Order order) {
    const auto count = static_cast<uint32_t>(state.range(0));
    const auto indices = MakeOrder(count, order);
    SparseSet<T> set;
    vector<SparseSetHandle> handles;
    Fill(set, handles, count);
    benchmark::DoNotOptimize(&set);
    for (auto _ : state) {
        state.PauseTiming();
        set.Clear();
        Fill(set, handles, count);
        state.ResumeTiming();
        for (auto index : indices) set.Destroy(handles[index]);
        benchmark::DoNotOptimize(set.Values().data());
        benchmark::ClobberMemory();
    }
    Check(state, failed, set.Empty(), "destroy did not empty the set");
    for (auto handle : handles) {
        if (!Check(state, failed, !set.IsAlive(handle), "destroy left a live handle")) break;
    }
    Report(state, count, count, count, sizeof(T));
}

template <class T>
void Churn(benchmark::State& state, bool& failed, ChurnMode mode) {
    const auto count = static_cast<uint32_t>(state.range(0));
    const auto batch = std::max(1u, count * static_cast<uint32_t>(state.range(1)) / 100u);
    const auto order = MakeOrder(count, Order::Random);
    SparseSet<T> set;
    vector<SparseSetHandle> handles;
    Fill(set, handles, count);
    uint64_t serial = count;
    uint32_t cursor = 0;
    benchmark::DoNotOptimize(&set);
    for (auto _ : state) {
        if (mode == ChurnMode::Burst) {
            for (uint32_t i = 0; i < batch; ++i) set.Destroy(handles[order[(cursor + i) % count]]);
        }
        for (uint32_t i = 0; i < batch; ++i) {
            const uint32_t index = mode == ChurnMode::Tail ? count - 1u : order[(cursor + i) % count];
            if (mode != ChurnMode::Burst) set.Destroy(handles[index]);
            handles[index] = set.Emplace(++serial);
        }
        cursor = (cursor + batch) % count;
        benchmark::DoNotOptimize(set.Values().data());
        benchmark::ClobberMemory();
    }
    Check(state, failed, set.Count() == count, "churn changed the live count");
    const uint64_t total = serial - count;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t index = mode == ChurnMode::Tail ? i : order[i];
        uint64_t expected = index + 1u;
        if (mode == ChurnMode::Tail) {
            if (index == count - 1u) expected = serial;
        } else if (total > i) {
            expected = count + i + 1u + ((total - 1u - i) / count) * count;
        }
        Nullable<const T*> value = std::as_const(set).TryGet(handles[index]);
        if (!Check(state, failed, value && value.Get()->Key() == expected, "churn lost a live value")) break;
    }
    Report(state, static_cast<int64_t>(batch) * 2, count, count, sizeof(T));
}

template <class T>
void Clear(benchmark::State& state, bool& failed, bool reserveOnly, bool refillTimed) {
    const auto peak = static_cast<uint32_t>(state.range(0));
    const auto live = static_cast<uint32_t>(state.range(1));
    SparseSet<T> set;
    vector<SparseSetHandle> handles;
    if (reserveOnly) {
        set.Reserve(peak);
    } else {
        Fill(set, handles, peak);
        for (uint32_t i = peak; i > 0; --i) set.Destroy(handles[i - 1u]);
        Fill(set, handles, live);
    }
    SparseSetHandle previous;
    benchmark::DoNotOptimize(&set);
    for (auto _ : state) {
        if (!refillTimed && live != 0) {
            state.PauseTiming();
            if (set.Empty()) Fill(set, handles, live);
            previous = handles[0];
            state.ResumeTiming();
        }
        set.Clear();
        if (refillTimed) Fill(set, handles, live);
        benchmark::DoNotOptimize(set.Values().data());
        benchmark::ClobberMemory();
    }
    Check(state, failed, refillTimed ? Validate(set, handles) : set.Empty(), "clear/refill produced invalid contents");
    if (!refillTimed && live != 0) Check(state, failed, !set.IsAlive(previous), "clear kept a stale handle alive");
    state.counters["live_before_clear"] = live;
    state.counters["reserved"] = reserveOnly ? peak : 0;
    Report(state, refillTimed ? live + 1u : 1, live, reserveOnly ? 0 : peak, sizeof(T));
}

template <class Function>
auto Register(const string& name, bool& failed, Function function) {
    return benchmark::RegisterBenchmark(name.c_str(), [&failed, function](benchmark::State& state) { function(state, failed); })->UseRealTime();
}

void ClearDistribution(benchmark::State& state, bool& failed, bool shuffled) {
    const auto peak = static_cast<uint32_t>(state.range(0));
    const uint32_t live = peak / 2u;
    const auto order = MakeOrder(peak, shuffled ? Order::Random : Order::Reverse);
    SparseSet<Payload<8>> set;
    vector<SparseSetHandle> handles;
    Fill(set, handles, peak);
    benchmark::DoNotOptimize(&set);
    for (auto _ : state) {
        state.PauseTiming();
        set.Clear();
        Fill(set, handles, peak);
        for (uint32_t i = 0; i < peak - live; ++i) set.Destroy(handles[order[i]]);
        state.ResumeTiming();
        set.Clear();
        benchmark::DoNotOptimize(set.Values().data());
        benchmark::ClobberMemory();
    }
    Check(state, failed, set.Empty(), "distributed clear did not empty the set");
    for (auto handle : handles) {
        if (!Check(state, failed, !set.IsAlive(handle), "distributed clear retained a live handle")) break;
    }
    Report(state, 1, live, peak, 8);
}

template <class T>
void RegisterPayload(bool& failed, std::string_view label, const vector<int64_t>& sizes) {
    for (auto mode : {InsertMode::Fresh, InsertMode::Reserved, InsertMode::ReuseOrdered, InsertMode::ReuseRandom}) {
        const char* name = mode == InsertMode::Fresh ? "Fresh" : mode == InsertMode::Reserved   ? "Reserved"
                                                             : mode == InsertMode::ReuseOrdered ? "ReuseOrdered"
                                                                                                : "ReuseRandom";
        auto* bench = Register(fmt::format("SparseSet/Insert/{}/{}", name, label), failed,
                               [mode](auto& state, bool& error) { Insert<T>(state, error, mode); });
        bench->ArgName("n");
        for (auto size : sizes) bench->Arg(size);
    }
    for (auto order : {Order::Reverse, Order::Forward, Order::Random}) {
        const char* name = order == Order::Reverse ? "Tail" : order == Order::Forward ? "Forward"
                                                                                      : "Random";
        auto* bench = Register(fmt::format("SparseSet/Destroy/{}/{}", name, label), failed,
                               [order](auto& state, bool& error) { Destroy<T>(state, error, order); });
        bench->ArgName("n");
        for (auto size : sizes) bench->Arg(size);
    }
    for (auto mode : {ChurnMode::Tail, ChurnMode::Random, ChurnMode::Burst}) {
        const char* name = mode == ChurnMode::Tail ? "Tail" : mode == ChurnMode::Random ? "Random"
                                                                                        : "Burst";
        auto* bench = Register(fmt::format("SparseSet/Churn/{}/{}", name, label), failed,
                               [mode](auto& state, bool& error) { Churn<T>(state, error, mode); });
        bench->ArgNames({"n", "percent"});
        for (auto size : sizes) {
            bench->Args({size, 1});
            bench->Args({size, 50});
        }
    }
}

template <class T>
void RegisterReads(bool& failed, std::string_view label, const vector<int64_t>& sizes) {
    for (bool fragmented : {false, true}) {
        const char* layout = fragmented ? "Fragmented" : "Compact";
        for (auto mode : {ReadMode::Dense, ReadMode::Get, ReadMode::TryGet, ReadMode::IsAlive}) {
            const char* api = mode == ReadMode::Dense ? "Values" : mode == ReadMode::Get  ? "Get"
                                                               : mode == ReadMode::TryGet ? "TryGet"
                                                                                          : "IsAlive";
            for (auto order : {Order::Forward, Order::Random, Order::Stride}) {
                if (mode == ReadMode::Dense && order != Order::Forward) continue;
                if (mode == ReadMode::IsAlive && order == Order::Stride) continue;
                const char* access = order == Order::Forward ? "Sequential" : order == Order::Random ? "Random"
                                                                                                     : "Stride257";
                auto* bench = Register(fmt::format("SparseSet/Read/{}/{}/{}/{}", api, layout, access, label), failed,
                                       [mode, order, fragmented](auto& state, bool& error) { Read<T>(state, error, mode, order, fragmented); });
                bench->ArgNames({"n", "spread"});
                for (auto size : sizes) bench->Args({size, 1});
                if (sizeof(T) == 8) bench->Args({16384, 64});
            }
        }
    }
}

void RegisterBenchmarks(bool& failed) {
    const vector<int64_t> sizes{256, 16384, 262144};
    RegisterPayload<Payload<8>>(failed, "P8", sizes);
    RegisterPayload<Payload<64>>(failed, "P64", sizes);
    RegisterPayload<Payload<256>>(failed, "P256", sizes);
    RegisterPayload<Payload<4096>>(failed, "P4096", {256, 16384});
    RegisterPayload<Owned64>(failed, "Owned64", {256, 16384});
    Register("SparseSet/Insert/ReserveEach/P8", failed, [](auto& state, bool& error) { Insert<Payload<8>>(state, error, InsertMode::ReserveEach); })
        ->ArgName("n")
        ->Arg(256)
        ->Arg(4096)
        ->Arg(16384);
    RegisterReads<Payload<8>>(failed, "P8", {256, 16384, 262144, 1048576});
    RegisterReads<Payload<256>>(failed, "P256", sizes);
    Register("SparseSet/Read/Get/Wide/Random/P8", failed, [](auto& state, bool& error) { Read<Payload<8>>(state, error, ReadMode::Get, Order::Random, true); })
        ->ArgNames({"n", "spread"})
        ->Args({4194304, 1});
    Register("SparseSet/DependentRead/P8", failed, DependentRead)->ArgName("n")->Arg(256)->Arg(16384)->Arg(1048576);
    for (bool grow : {false, true}) {
        const char* name = grow ? "Grow" : "Spare";
        Register(fmt::format("SparseSet/GrowthEdge/{}/P8", name), failed,
                 [grow](auto& state, bool& error) { GrowthEdge<Payload<8>>(state, error, grow); })
            ->ArgName("target")
            ->Arg(256)
            ->Arg(16384)
            ->Arg(262144);
        Register(fmt::format("SparseSet/GrowthEdge/{}/P256", name), failed,
                 [grow](auto& state, bool& error) { GrowthEdge<Payload<256>>(state, error, grow); })
            ->ArgName("target")
            ->Arg(256)
            ->Arg(16384);
    }
    for (auto kind : {MissKind::Invalid, MissKind::OutOfRange, MissKind::Dead, MissKind::Recycled}) {
        const char* name = kind == MissKind::Invalid ? "Invalid" : kind == MissKind::OutOfRange ? "OutOfRange"
                                                               : kind == MissKind::Dead         ? "Dead"
                                                                                                : "Recycled";
        for (bool shuffled : {false, true}) {
            auto* bench = Register(fmt::format("SparseSet/Misses/{}/{}", name, shuffled ? "Shuffled" : "Grouped"), failed,
                                   [kind, shuffled](auto& state, bool& error) { Misses(state, error, kind, shuffled); });
            bench->ArgNames({"n", "hit_percent"});
            for (auto size : {16384, 1048576}) {
                for (auto hits : {0, 50, 95, 100}) bench->Args({size, hits});
            }
        }
    }
    auto* clear = Register("SparseSet/Clear/P8", failed, [](auto& state, bool& error) { Clear<Payload<8>>(state, error, false, false); });
    clear->ArgNames({"peak", "live"})->Args({0, 0});
    auto* refill = Register("SparseSet/ClearRefill/P8", failed, [](auto& state, bool& error) { Clear<Payload<8>>(state, error, false, true); });
    refill->ArgNames({"peak", "live"});
    auto* reserved = Register("SparseSet/ClearReservedEmpty/P8", failed, [](auto& state, bool& error) { Clear<Payload<8>>(state, error, true, false); });
    reserved->ArgNames({"peak", "live"});
    for (auto peak : {1024, 65536, 1048576}) {
        for (auto live : {0, 1, 1024}) clear->Args({peak, live});
        if (peak != 1024) clear->Args({peak, peak});
        refill->Args({peak, 1})->Args({peak, 1024});
        reserved->Args({peak, 0});
    }
    Register("SparseSet/Clear/Owned64", failed, [](auto& state, bool& error) { Clear<Owned64>(state, error, false, false); })
        ->ArgNames({"peak", "live"})
        ->Args({16384, 1})
        ->Args({16384, 16384});
    for (bool shuffled : {false, true}) {
        Register(fmt::format("SparseSet/ClearDistribution/{}/P8", shuffled ? "Random" : "Clustered"), failed,
                 [shuffled](auto& state, bool& error) { ClearDistribution(state, error, shuffled); })
            ->ArgName("peak")
            ->Arg(16384)
            ->Arg(1048576);
    }
}

}  // namespace

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    bool failed = false;
    RegisterBenchmarks(failed);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return failed ? 1 : 0;
}
