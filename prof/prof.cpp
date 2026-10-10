// In-process driver for gprof: structurize every graph of a .graphs file (no fork), report wall-clock and outcome counts.
#define main cfgfuzz_main
#include "../cfgfuzz.cpp"  // same tree as this file: prof/ next to cfgfuzz.cpp
#undef main
#include <chrono>
int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "prof <in.graphs> <words-per-block>\n"); return 2; }
    std::ifstream in(argv[1]);
    const auto words = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10));
    std::string line; std::size_t n = 0, ok = 0, failed = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::getline(in, line)) {
        const auto tab = line.find('\t'); if (tab == std::string::npos) continue;
        const auto s = fromText(line.substr(tab + 1));
        const auto g0 = std::chrono::steady_clock::now(); std::size_t outBlocks = 0; const char* outcome = "ok";
        try { auto graph = makeGraph(s, words); Structurizer{}.Structurize(graph); ++ok; outBlocks = graph.blocks.size(); } catch (...) { ++failed; outcome = "fail"; }
        if (argc >= 4) std::fprintf(stderr, "%s\t%lld\t%s\t%zu\t%zu\n", line.substr(0, tab).c_str(), static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g0).count()), outcome, s.size(), outBlocks);
        ++n;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%zu graphs, %zu structurized, %zu refused/budget, %lld ms\n", n, ok, failed, static_cast<long long>(ms));
    return 0;
}
