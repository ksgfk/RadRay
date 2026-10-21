#include <fstream>
#include <iterator>

#include <benchmark/benchmark.h>

#include <radray/types.h>
#include <radray/wavefront_obj.h>

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    bool failed = false;
    benchmark::RegisterBenchmark("ReadObj", [&failed](benchmark::State& state) {
        std::ifstream file("assets/buddha1.obj", std::ios::binary);
        if (!file) {
            failed = true;
            state.SkipWithError("assets/buddha1.obj is unavailable; run from the repository root");
            return;
        }
        const radray::string data{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        if (data.empty() || file.bad()) {
            failed = true;
            state.SkipWithError("OBJ input is empty or could not be read");
            return;
        }
        for (auto _ : state) {
            radray::WavefrontObjReader reader{data};
            reader.Read();
            if (reader.HasError()) {
                failed = true;
                state.SkipWithError("OBJ parse failed");
                break;
            }
            benchmark::DoNotOptimize(reader.Faces().data());
            benchmark::ClobberMemory();
        }
        state.SetItemsProcessed(state.iterations());
        state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(data.size()));
    });
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return failed ? 1 : 0;
}
