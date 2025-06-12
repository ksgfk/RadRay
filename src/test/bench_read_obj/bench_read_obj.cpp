#include <fstream>
#include <iostream>

#include <benchmark/benchmark.h>

#include <radray/wavefront_obj.h>

static radray::string obj_data;

static void ReadObjFileOnce() {
    static bool loaded = false;
    if (!loaded) {
        std::ifstream file("assets/buddha1.obj", std::ios::in | std::ios::binary);
        std::ostringstream ss;
        ss << file.rdbuf();
        obj_data = ss.str();
        loaded = true;

        radray::WavefrontObjReader reader{obj_data};
        reader.Read();
        std::cout << "=========================================" << std::endl;
        std::cout << "f:" << reader.Positions().size() << " f:" << reader.Faces().size() << std::endl;
        std::cout << "=========================================" << std::endl;
    }
}

static void BM_ReadObj(benchmark::State& state) {
    ReadObjFileOnce();
    for (auto e : state) {
        radray::WavefrontObjReader reader{obj_data};
        reader.Read();
        benchmark::DoNotOptimize(reader);
    }
}
BENCHMARK(BM_ReadObj);
BENCHMARK_MAIN();
