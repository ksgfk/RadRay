#include <radray/object_arena.h>

#include <cstdint>
#include <type_traits>
#include <vector>

#include <benchmark/benchmark.h>

namespace {

struct Trivial {
    Trivial(size_t value, size_t&) noexcept {
        for (auto& word : Words) word = value;
    }
    uint64_t Words[8];
};

struct Nontrivial {
    Nontrivial(size_t value, size_t& destroyed) noexcept : Destroyed(&destroyed) {
        for (auto& word : Words) word = value;
    }
    ~Nontrivial() noexcept { ++*Destroyed; }
    uint64_t Words[7];
    size_t* Destroyed;
};

template <class T>
void Populate(radray::ObjectArena& arena, size_t count, size_t& destroyed) {
    for (size_t i = 0; i < count; ++i) {
        auto* value = arena.Emplace<T>(i, destroyed);
        benchmark::DoNotOptimize(value);
    }
    benchmark::ClobberMemory();
}

template <class T>
void ArenaObjects(benchmark::State& state, bool cold, bool resetOnly, bool& failed) {
    const size_t count = static_cast<size_t>(state.range(0));
    size_t destroyed = 0;
    radray::ObjectArena arena{{64 * 1024, 8 * 1024 * 1024}};
    if (!cold) {
        Populate<T>(arena, count, destroyed);
        arena.Reset();
    }
    for (auto _ : state) {
        if (resetOnly) state.PauseTiming();
        destroyed = 0;
        Populate<T>(arena, count, destroyed);
        if (resetOnly) state.ResumeTiming();
        if (cold)
            arena.Release();
        else
            arena.Reset();
        if (destroyed != (std::is_trivially_destructible_v<T> ? 0 : count)) {
            state.SkipWithError("Incorrect destructor count");
            failed = true;
            break;
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(count));
    state.counters["retained_bytes"] = static_cast<double>(arena.GetReservedBytes());
    state.counters["payload_bytes"] = sizeof(T);
}

template <class T>
void HeapObjects(benchmark::State& state, bool& failed) {
    const size_t count = static_cast<size_t>(state.range(0));
    radray::vector<radray::Nullable<T*>> objects(count);
    size_t destroyed = 0;
    for (auto _ : state) {
        destroyed = 0;
        for (size_t i = 0; i < count; ++i) {
            objects[i] = new T{i, destroyed};
            benchmark::DoNotOptimize(objects[i].Get());
        }
        benchmark::ClobberMemory();
        for (auto object : objects) delete object.Get();
        if (destroyed != (std::is_trivially_destructible_v<T> ? 0 : count)) {
            state.SkipWithError("Incorrect destructor count");
            failed = true;
            break;
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(count));
    state.counters["payload_bytes"] = sizeof(T);
}

template <class Container>
bool FillVector(Container& values, size_t count, bool reserve) {
    if (reserve) values.reserve(count);
    for (size_t i = 0; i < count; ++i) values.push_back(i);
    benchmark::DoNotOptimize(values.data());
    benchmark::ClobberMemory();
    return values.size() == count && values.front() == 0 && values.back() == count - 1;
}

void VectorGrowth(benchmark::State& state, bool arenaStorage, bool reserve, bool& failed) {
    const size_t count = static_cast<size_t>(state.range(0));
    radray::ObjectArena arena{{64 * 1024, 8 * 1024 * 1024}};
    if (arenaStorage) {
        {
            std::vector<size_t, radray::ArenaAllocator<size_t>> values{radray::ArenaAllocator<size_t>{arena}};
            (void)FillVector(values, count, reserve);
        }
        arena.Reset();
    }
    for (auto _ : state) {
        bool valid;
        if (arenaStorage) {
            {
                std::vector<size_t, radray::ArenaAllocator<size_t>> values{radray::ArenaAllocator<size_t>{arena}};
                valid = FillVector(values, count, reserve);
            }
            arena.Reset();
        } else {
            radray::vector<size_t> values;
            valid = FillVector(values, count, reserve);
        }
        if (!valid) {
            state.SkipWithError("Incorrect vector contents");
            failed = true;
            break;
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(count));
    state.counters["retained_bytes"] = static_cast<double>(arena.GetReservedBytes());
}

void RegisterBenchmarks(bool& failed) {
    auto configure = [](auto* bench) { bench->ArgName("count")->Arg(256)->Arg(4096)->UseRealTime(); };
    configure(benchmark::RegisterBenchmark("ObjectArena/Cold/Trivial", [&failed](auto& state) { ArenaObjects<Trivial>(state, true, false, failed); }));
    configure(benchmark::RegisterBenchmark("ObjectArena/Cold/Nontrivial", [&failed](auto& state) { ArenaObjects<Nontrivial>(state, true, false, failed); }));
    configure(benchmark::RegisterBenchmark("ObjectArena/Reuse/Trivial", [&failed](auto& state) { ArenaObjects<Trivial>(state, false, false, failed); }));
    configure(benchmark::RegisterBenchmark("ObjectArena/Reuse/Nontrivial", [&failed](auto& state) { ArenaObjects<Nontrivial>(state, false, false, failed); }));
    configure(benchmark::RegisterBenchmark("ObjectArena/Reset/Trivial", [&failed](auto& state) { ArenaObjects<Trivial>(state, false, true, failed); }));
    configure(benchmark::RegisterBenchmark("ObjectArena/Reset/Nontrivial", [&failed](auto& state) { ArenaObjects<Nontrivial>(state, false, true, failed); }));
    configure(benchmark::RegisterBenchmark("ObjectArena/Heap/Trivial", [&failed](auto& state) { HeapObjects<Trivial>(state, failed); }));
    configure(benchmark::RegisterBenchmark("ObjectArena/Heap/Nontrivial", [&failed](auto& state) { HeapObjects<Nontrivial>(state, failed); }));
    configure(benchmark::RegisterBenchmark("ObjectArena/Vector/ArenaGrowth", [&failed](auto& state) { VectorGrowth(state, true, false, failed); }));
    configure(benchmark::RegisterBenchmark("ObjectArena/Vector/ArenaReserved", [&failed](auto& state) { VectorGrowth(state, true, true, failed); }));
    configure(benchmark::RegisterBenchmark("ObjectArena/Vector/HeapGrowth", [&failed](auto& state) { VectorGrowth(state, false, false, failed); }));
    configure(benchmark::RegisterBenchmark("ObjectArena/Vector/HeapReserved", [&failed](auto& state) { VectorGrowth(state, false, true, failed); }));
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
