// Random structured-program CFG fuzzer for ShaderRecompiler::Structurizer.
//
//   cfgfuzz gen <count> <seed> <out.graphs>
//   cfgfuzz run <in.graphs> <out.results> [words-per-block] [timeout-seconds]
//   cfgfuzz reduce <in.graphs> <graph-id> [words-per-block] [timeout-seconds]
//   cfgfuzz one "<succ lists>" [words-per-block]          e.g. "1;2,4;3,4;4,5;1,6;1;"
//
// Graph line format: <id>\t<succ lists>   where succ lists = block0;block1;...  each "a,b" or "a" or "" (return).
// Every graph is run in a forked child with an alarm, so hangs and crashes are reported, not fatal.
// Result classes: OK, BUDGET, REFUSED, INVARIANT, TIMEOUT, CRASH.
#include "ControlFlow/ControlFlowGraph.hpp"
#include "ControlFlow/Structurizer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

using namespace ShaderRecompiler;
using Succ = std::vector<std::vector<std::uint32_t>>;

// ---------------------------------------------------------------- invariants (copied from tests/Structurizer.cpp)
namespace {

const BasicBlock* innermostLoopHeader(const ControlFlowGraph& graph, std::uint32_t blockId) {
    const BasicBlock* innermost = nullptr;
    std::size_t innermostSize = 0;
    for (const auto& header : graph.blocks) {
        if (!header.terminator.loopHeader || !graph.Dominates(header.id, blockId) || graph.Dominates(header.terminator.mergeBlock, blockId) || blockId == header.terminator.mergeBlock) continue;
        const auto size = static_cast<std::size_t>(std::count_if(graph.blocks.begin(), graph.blocks.end(), [&](const BasicBlock& block) { return graph.Dominates(header.id, block.id) && !graph.Dominates(header.terminator.mergeBlock, block.id); }));
        if (innermost == nullptr || size < innermostSize) { innermost = &header; innermostSize = size; }
    }
    return innermost;
}

void requireStructuredBranches(const ControlFlowGraph& graph) {
    for (const auto& block : graph.blocks) {
        const auto& terminator = block.terminator;
        if (terminator.kind != TerminatorKind::ConditionalBranch || terminator.loopHeader || terminator.trueBlock == terminator.falseBlock) continue;
        const auto* loop = innermostLoopHeader(graph, block.id);
        if (terminator.mergeBlock != InvalidControlFlowId) {
            if (loop != nullptr && terminator.mergeBlock == loop->terminator.continueBlock)
                throw std::logic_error("block " + std::to_string(block.id) + " in the loop at block " + std::to_string(loop->id) + " merges at the loop's continue block");
            continue;
        }
        const auto exits = [&](std::uint32_t target) { return loop != nullptr && (target == loop->terminator.mergeBlock || target == loop->terminator.continueBlock); };
        if (!exits(terminator.trueBlock) && !exits(terminator.falseBlock))
            throw std::logic_error("block " + std::to_string(block.id) + " branches without a merge, and neither target is its loop's merge or continue");
    }
}

void requireExactPostDominators(const ControlFlowGraph& graph) {
    const auto count = static_cast<std::uint32_t>(graph.blocks.size());
    const auto reachesExitAvoiding = [&](std::uint32_t from, std::uint32_t avoided) {
        std::vector<bool> seen(count, false);
        std::vector<std::uint32_t> stack{from};
        seen[from] = true;
        while (!stack.empty()) {
            const auto id = stack.back(); stack.pop_back();
            if (graph.blocks[id].successors.empty()) return true;
            for (const auto successor : graph.blocks[id].successors) {
                if (successor == avoided || seen[successor]) continue;
                seen[successor] = true; stack.push_back(successor);
            }
        }
        return false;
    };
    for (const auto& block : graph.blocks) {
        std::vector<std::uint32_t> expected;
        for (std::uint32_t candidate = 0; candidate < count; ++candidate)
            if (candidate == block.id || !reachesExitAvoiding(block.id, candidate)) expected.push_back(candidate);
        if (block.postDominators != expected)
            throw std::logic_error("block " + std::to_string(block.id) + " has " + std::to_string(block.postDominators.size()) + " post-dominators, expected " + std::to_string(expected.size()));
    }
}

// Every original block must still be present (ids are preserved by the structurizer; clones get new ids).
void requireOriginalBlocksPresent(const ControlFlowGraph& graph, std::size_t originalCount) {
    for (std::uint32_t id = 0; id < originalCount; ++id) {
        bool found = false;
        for (const auto& b : graph.blocks) if (b.id == id) { found = true; break; }
        if (!found) throw std::logic_error("original block " + std::to_string(id) + " disappeared");
    }
}

// ---------------------------------------------------------------- graph <-> text
ControlFlowGraph makeGraph(const Succ& successors, std::uint32_t words, std::uint32_t bigWords = 0) {
    ControlFlowGraph graph;
    graph.entryBlock = 0;
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto& list : successors) { hash = (hash ^ (list.size() + 1)) * 1099511628211ull; for (auto t : list) hash = (hash ^ (t + 7)) * 1099511628211ull; }
    const std::uint32_t bigIndex = successors.empty() ? 0u : static_cast<std::uint32_t>(hash % successors.size());
    for (std::uint32_t id = 0; id < successors.size(); ++id) {
        BasicBlock block;
        block.id = id;
        block.startProgramCounter = id * 8;
        block.endProgramCounter = id * 8 + 8;
        block.instructionBegin = id * 2;
        block.instructionEnd = id * 2 + 2;
        block.estimatedSpirvWords = words;
        if (bigWords != 0 && !successors.empty() && id == bigIndex) block.estimatedSpirvWords = bigWords;
        block.successors = successors[id];
        auto& t = block.terminator;
        if (successors[id].empty()) t.kind = TerminatorKind::Return;
        else if (successors[id].size() == 1) { t.kind = TerminatorKind::Branch; t.trueBlock = successors[id][0]; }
        else { t.kind = TerminatorKind::ConditionalBranch; t.condition = BranchCondition::SccNonZero; t.trueBlock = successors[id][0]; t.falseBlock = successors[id][1]; }
        graph.blocks.push_back(std::move(block));
    }
    for (const auto& block : graph.blocks)
        for (const auto successor : block.successors) graph.blocks[successor].predecessors.push_back(block.id);
    return graph;
}

std::string toText(const Succ& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i) out += ';';
        for (std::size_t j = 0; j < s[i].size(); ++j) { if (j) out += ','; out += std::to_string(s[i][j]); }
    }
    return out;
}

std::string toLiteral(const Succ& s) {
    std::string out = "{";
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i) out += ", ";
        out += "{";
        for (std::size_t j = 0; j < s[i].size(); ++j) { if (j) out += ", "; out += std::to_string(s[i][j]); }
        out += "}";
    }
    return out + "}";
}

Succ fromText(const std::string& text) {
    Succ s;
    std::string block;
    std::stringstream ss(text);
    // split on ';' keeping empty fields
    std::size_t start = 0;
    while (true) {
        const auto semi = text.find(';', start);
        const std::string field = text.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
        std::vector<std::uint32_t> list;
        std::size_t p = 0;
        while (p < field.size()) {
            const auto comma = field.find(',', p);
            const std::string n = field.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
            if (!n.empty()) list.push_back(static_cast<std::uint32_t>(std::stoul(n)));
            if (comma == std::string::npos) break;
            p = comma + 1;
        }
        s.push_back(list);
        if (semi == std::string::npos) break;
        start = semi + 1;
    }
    return s;
}

// Drop blocks unreachable from 0, renumber in DFS order (keeps successor order). Also dedupe a 2-successor block whose targets coincide.
Succ normalize(const Succ& in) {
    std::vector<std::uint32_t> order;
    std::vector<std::int64_t> newId(in.size(), -1);
    std::vector<std::uint32_t> stack{0};
    if (in.empty()) return {};
    newId[0] = 0; order.push_back(0);
    while (!stack.empty()) {
        const auto b = stack.back(); stack.pop_back();
        for (auto s : in[b]) if (s < in.size() && newId[s] < 0) { newId[s] = static_cast<std::int64_t>(order.size()); order.push_back(s); stack.push_back(s); }
    }
    // BFS-ish relabel for stability: recompute with a queue
    std::fill(newId.begin(), newId.end(), -1);
    std::vector<std::uint32_t> queue{0}; newId[0] = 0; std::size_t head = 0; std::vector<std::uint32_t> bfs{0};
    while (head < queue.size()) {
        const auto b = queue[head++];
        for (auto s : in[b]) if (s < in.size() && newId[s] < 0) { newId[s] = static_cast<std::int64_t>(bfs.size()); bfs.push_back(s); queue.push_back(s); }
    }
    Succ out(bfs.size());
    for (std::size_t i = 0; i < bfs.size(); ++i) {
        std::vector<std::uint32_t> list;
        for (auto s : in[bfs[i]]) {
            const auto n = static_cast<std::uint32_t>(newId[s]);
            if (std::find(list.begin(), list.end(), n) == list.end()) list.push_back(n);
        }
        out[i] = list;
    }
    return out;
}


// True when every natural loop (DFS back edge target + body) has an edge leaving its body.
bool everyLoopHasExit(const Succ& s) {
    const auto n = s.size();
    std::vector<int> color(n, 0);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> backEdges;
    std::vector<std::pair<std::uint32_t, std::size_t>> stack{{0, 0}};
    color[0] = 1;
    while (!stack.empty()) {
        auto& [b, next] = stack.back();
        if (next < s[b].size()) {
            const auto t = s[b][next++];
            if (color[t] == 1) backEdges.emplace_back(b, t);
            else if (color[t] == 0) { color[t] = 1; stack.emplace_back(t, 0); }
        } else { color[b] = 2; stack.pop_back(); }
    }
    // predecessors
    std::vector<std::vector<std::uint32_t>> preds(n);
    for (std::uint32_t b = 0; b < n; ++b) for (auto t : s[b]) preds[t].push_back(b);
    for (const auto& [latch, header] : backEdges) {
        std::vector<bool> body(n, false);
        body[header] = true;
        std::vector<std::uint32_t> work{latch};
        if (!body[latch]) body[latch] = true;
        while (!work.empty()) {
            const auto b = work.back(); work.pop_back();
            if (b == header) continue;
            for (auto p : preds[b]) if (!body[p]) { body[p] = true; work.push_back(p); }
        }
        bool exits = false;
        for (std::uint32_t b = 0; b < n && !exits; ++b) if (body[b]) for (auto t : s[b]) if (!body[t]) { exits = true; break; }
        if (!exits) return false;
    }
    return true;
}

// ---------------------------------------------------------------- random structured programs
struct Generator {
    std::mt19937 rng;
    Succ succ;
    bool sharedReturn = false;
    std::uint32_t sharedReturnBlock = InvalidControlFlowId;
    int maxBlocks = 20;
    struct Loop { std::uint32_t header, exit; };

    explicit Generator(std::uint64_t seed) : rng(static_cast<std::uint32_t>(seed)) {}
    std::uint32_t pick(std::uint32_t n) { return std::uniform_int_distribution<std::uint32_t>(0, n - 1)(rng); }
    bool chance(int percent) { return pick(100) < static_cast<std::uint32_t>(percent); }
    std::uint32_t newBlock() { succ.emplace_back(); return static_cast<std::uint32_t>(succ.size() - 1); }
    void edge(std::uint32_t from, std::uint32_t to) { succ[from].push_back(to); }
    bool full() const { return static_cast<int>(succ.size()) >= maxBlocks; }

    std::uint32_t returnTarget(std::uint32_t cur) {
        if (sharedReturn) {
            if (sharedReturnBlock == InvalidControlFlowId) sharedReturnBlock = newBlock();
            edge(cur, sharedReturnBlock);
        }
        return InvalidControlFlowId;
    }

    // Emits statements starting in block `cur`; returns the block where control continues, or Invalid if it ended with a jump.
    std::uint32_t emitSeq(std::uint32_t cur, int depth, std::vector<Loop>& loops) {
        const int statements = 1 + static_cast<int>(pick(3));
        for (int i = 0; i < statements && cur != InvalidControlFlowId; ++i) {
            if (full()) break;
            const auto r = pick(100);
            const bool inLoop = !loops.empty();
            if (r < 30 && depth < 4) {              // if / if-else
                const auto thenB = newBlock();
                std::uint32_t elseB = InvalidControlFlowId;
                const bool hasElse = chance(50);
                if (hasElse) elseB = newBlock();
                edge(cur, thenB);
                const auto tEnd = emitSeq(thenB, depth + 1, loops);
                std::uint32_t eEnd = InvalidControlFlowId;
                if (hasElse) eEnd = emitSeq(elseB, depth + 1, loops);
                std::uint32_t merge = InvalidControlFlowId;
                if (tEnd != InvalidControlFlowId || eEnd != InvalidControlFlowId || !hasElse) {
                    merge = newBlock();
                    if (tEnd != InvalidControlFlowId) edge(tEnd, merge);
                    if (eEnd != InvalidControlFlowId) edge(eEnd, merge);
                    if (!hasElse) edge(cur, merge); else edge(cur, elseB);
                } else {
                    edge(cur, elseB);
                }
                cur = merge;
            } else if (r < 50 && depth < 3) {       // loop
                const auto kind = pick(3);          // 0 while, 1 do-while, 2 infinite
                const auto exit = newBlock();
                if (kind == 0) {
                    const auto header = newBlock();
                    const auto body = newBlock();
                    edge(cur, header);
                    edge(header, body); edge(header, exit);
                    loops.push_back({header, exit});
                    const auto bEnd = emitSeq(body, depth + 1, loops);
                    loops.pop_back();
                    if (bEnd != InvalidControlFlowId) edge(bEnd, header);
                } else if (kind == 1) {
                    const auto body = newBlock();
                    edge(cur, body);
                    loops.push_back({body, exit});
                    const auto bEnd = emitSeq(body, depth + 1, loops);
                    loops.pop_back();
                    if (bEnd != InvalidControlFlowId) { edge(bEnd, body); edge(bEnd, exit); }
                } else {
                    const auto header = newBlock();
                    edge(cur, header);
                    loops.push_back({header, exit});
                    const auto bEnd = emitSeq(header, depth + 1, loops);
                    loops.pop_back();
                    if (bEnd != InvalidControlFlowId) edge(bEnd, header);
                }
                cur = exit;
            } else if (r < 62 && inLoop) {          // conditional break
                const auto cont = newBlock();
                edge(cur, loops.back().exit); edge(cur, cont);
                if (chance(30)) std::swap(succ[cur][0], succ[cur][1]);
                cur = cont;
            } else if (r < 72 && inLoop) {          // conditional continue
                const auto cont = newBlock();
                edge(cur, loops.back().header); edge(cur, cont);
                if (chance(30)) std::swap(succ[cur][0], succ[cur][1]);
                cur = cont;
            } else if (r < 80 && inLoop && loops.size() >= 2 && chance(50)) { // break out of the outer loop
                const auto cont = newBlock();
                edge(cur, loops[loops.size() - 2].exit); edge(cur, cont);
                cur = cont;
            } else if (r < 86) {                    // conditional return
                const auto cont = newBlock();
                const auto ret = newBlock();
                edge(cur, ret); edge(cur, cont);
                if (chance(30)) std::swap(succ[cur][0], succ[cur][1]);
                returnTarget(ret);
                cur = cont;
            } else if (r < 90 && inLoop) {          // unconditional break
                edge(cur, loops.back().exit);
                return InvalidControlFlowId;
            } else if (r < 93 && inLoop) {          // unconditional continue
                edge(cur, loops.back().header);
                return InvalidControlFlowId;
            } else if (r < 96) {                    // unconditional return
                return returnTarget(cur);
            } else {                                // straight-line block
                const auto nb = newBlock();
                edge(cur, nb);
                cur = nb;
            }
        }
        return cur;
    }

    Succ generate() {
        succ.clear();
        sharedReturn = chance(40);
        sharedReturnBlock = InvalidControlFlowId;
        maxBlocks = 6 + static_cast<int>(pick(16));
        const auto entry = newBlock();
        std::vector<Loop> loops;
        const auto end = emitSeq(entry, 0, loops);
        if (end != InvalidControlFlowId) returnTarget(end);
        return normalize(succ);
    }
};

// ---------------------------------------------------------------- running one graph in a child
struct Outcome { std::string cls; std::string message; };

std::uint32_t g_bigWords = 0;
Outcome runChild(const Succ& s, std::uint32_t words, unsigned timeoutSeconds) {
    int fds[2];
    if (pipe(fds) != 0) return {"CRASH", "pipe failed"};
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        alarm(timeoutSeconds);
        std::string result;
        try {
            auto graph = makeGraph(s, words, g_bigWords);
            const auto original = graph.blocks.size();
            Structurizer{}.Structurize(graph);
            try {
                requireOriginalBlocksPresent(graph, original);
                requireExactPostDominators(graph);
                requireStructuredBranches(graph);
                result = "OK\t";
            } catch (const std::logic_error& e) {
                result = std::string("INVARIANT\t") + e.what();
            }
        } catch (const std::exception& e) {
            const std::string what = e.what();
            const bool budget = what.find("exceeded") != std::string::npos && what.find("budget") != std::string::npos;
            result = (budget ? "BUDGET\t" : "REFUSED\t") + what;
        } catch (...) {
            result = "CRASH\tnon-std exception";
        }
        (void)!write(fds[1], result.data(), result.size());
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    std::string buffer;
    char chunk[4096];
    ssize_t n;
    while ((n = read(fds[0], chunk, sizeof(chunk))) > 0) buffer.append(chunk, static_cast<std::size_t>(n));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        if (WTERMSIG(status) == SIGALRM) return {"TIMEOUT", "no result within " + std::to_string(timeoutSeconds) + " s"};
        return {"CRASH", std::string("signal ") + std::to_string(WTERMSIG(status))};
    }
    const auto tab = buffer.find('\t');
    if (tab == std::string::npos) return {"CRASH", "no result from child"};
    return {buffer.substr(0, tab), buffer.substr(tab + 1)};
}

std::string classKey(const Outcome& o) {
    // Stable key for "same failure": class + message with numbers stripped.
    std::string m;
    for (char c : o.message) if (!std::isdigit(static_cast<unsigned char>(c))) m += c;
    return o.cls + "|" + m;
}

// ---------------------------------------------------------------- delta reduction
Succ reduce(Succ graph, const std::string& key, std::uint32_t words, unsigned timeoutSeconds, int& evaluations) {
    bool changed = true;
    auto same = [&](const Succ& candidate) {
        if (candidate.size() < 2) return false;
        ++evaluations;
        return classKey(runChild(candidate, words, timeoutSeconds)) == key;
    };
    while (changed) {
        changed = false;
        // 1. drop one successor edge of a 2-successor block
        for (std::size_t b = 0; b < graph.size() && !changed; ++b) {
            if (graph[b].size() != 2) continue;
            for (int k = 0; k < 2 && !changed; ++k) {
                Succ c = graph; c[b].erase(c[b].begin() + k); c = normalize(c);
                if (c.size() < graph.size() || true) { if (same(c)) { graph = c; changed = true; } }
            }
        }
        // 2. bypass a single-successor block (predecessors go straight to its successor)
        for (std::size_t b = 1; b < graph.size() && !changed; ++b) {
            if (graph[b].size() != 1) continue;
            const auto target = graph[b][0];
            if (target == b) continue;
            Succ c = graph;
            for (auto& list : c) for (auto& s : list) if (s == b) s = target;
            c[b].clear();
            // b is now only reachable if something still points at it (nothing does), normalize drops it
            c = normalize(c);
            if (same(c)) { graph = c; changed = true; }
        }
        // 3. turn a block into a return
        for (std::size_t b = 1; b < graph.size() && !changed; ++b) {
            if (graph[b].empty()) continue;
            Succ c = graph; c[b].clear(); c = normalize(c);
            if (same(c)) { graph = c; changed = true; }
        }
    }
    return graph;
}

}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: see header\n"); return 2; }
    const std::string mode = argv[1];
    if (mode == "gen" && argc >= 5) {
        const auto count = std::strtoull(argv[2], nullptr, 10);
        const auto seed = std::strtoull(argv[3], nullptr, 10);
        std::ofstream out(argv[4]);
        std::set<std::string> seen;
        std::uint64_t id = 0;
        for (std::uint64_t i = 0; id < count && i < count * 20; ++i) {
            Generator g(seed * 1000003ull + i);
            const auto s = g.generate();
            if (s.size() < 3 || !everyLoopHasExit(s)) continue;
            const auto text = toText(s);
            if (!seen.insert(text).second) continue;
            out << id++ << '\t' << text << '\n';
        }
        std::fprintf(stderr, "generated %llu unique graphs\n", static_cast<unsigned long long>(id));
        return 0;
    }
    if (mode == "run" && argc >= 4) {
        const std::uint32_t words = argc >= 5 ? static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 10)) : 0u;
        const unsigned timeoutSeconds = argc >= 6 ? static_cast<unsigned>(std::strtoul(argv[5], nullptr, 10)) : 10u;
        g_bigWords = argc >= 7 ? static_cast<std::uint32_t>(std::strtoul(argv[6], nullptr, 10)) : 0u;
        std::ifstream in(argv[2]);
        std::ofstream out(argv[3]);
        std::string line;
        std::uint64_t n = 0;
        while (std::getline(in, line)) {
            const auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            const auto s = fromText(line.substr(tab + 1));
            const auto o = runChild(s, words, timeoutSeconds);
            out << line.substr(0, tab) << '\t' << o.cls << '\t' << o.message << '\t' << s.size() << '\n';
            ++n;
        }
        std::fprintf(stderr, "ran %llu graphs\n", static_cast<unsigned long long>(n));
        return 0;
    }

    if (mode == "dump" && argc >= 3) {
        const std::uint32_t words = argc >= 4 ? static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10)) : 16u;
        const auto s = normalize(fromText(argv[2]));
        auto graph = makeGraph(s, words);
        const auto originalCount = graph.blocks.size();
        try { Structurizer{}.Structurize(graph); } catch (const std::exception& e) { std::printf("structurizer threw: %s\n", e.what()); return 1; }
        std::printf("structurized: %zu -> %zu blocks (ids >= %zu are new)\n", originalCount, graph.blocks.size(), originalCount);
        const char* kinds[] = {"Branch", "CondBranch", "IndirectBranch", "Return", "Unsupported"};
        for (const auto& b : graph.blocks) {
            const auto& t = b.terminator;
            std::printf("  block %2u  succ=[", b.id);
            for (std::size_t i = 0; i < b.successors.size(); ++i) std::printf("%s%u", i ? "," : "", b.successors[i]);
            std::printf("]  %s", kinds[static_cast<int>(t.kind)]);
            if (t.kind == TerminatorKind::ConditionalBranch) std::printf("(true=%u false=%u cond=%d)", t.trueBlock, t.falseBlock, static_cast<int>(t.condition));
            if (t.loopHeader) std::printf("  LOOP merge=%u continue=%u", t.mergeBlock, t.continueBlock);
            else if (t.mergeBlock != InvalidControlFlowId) std::printf("  merge=%u", t.mergeBlock);
            if (t.gotoVariable != InvalidControlFlowId) std::printf("  goto var=%u value=%d", t.gotoVariable, t.gotoValue);
            std::printf("  instr=[%u,%u)%s\n", b.instructionBegin, b.instructionEnd, b.instructionBegin == b.instructionEnd ? " empty" : "");
        }
        try { requireStructuredBranches(graph); std::printf("requireStructuredBranches: OK\n"); } catch (const std::exception& e) { std::printf("requireStructuredBranches: %s\n", e.what()); }
        return 0;
    }
    if ((mode == "reduce" && argc >= 4) || (mode == "one" && argc >= 3)) {
        const std::uint32_t words = argc >= 5 ? static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 10)) : 0u;
        const unsigned timeoutSeconds = argc >= 6 ? static_cast<unsigned>(std::strtoul(argv[5], nullptr, 10)) : 10u;
        Succ s;
        if (mode == "one") {
            s = normalize(fromText(argv[2]));
            const auto o = runChild(s, argc >= 4 ? static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10)) : 0u, 10);
            std::printf("%s\t%s\n%s\n", o.cls.c_str(), o.message.c_str(), toLiteral(s).c_str());
            return 0;
        }
        std::ifstream in(argv[2]);
        std::string line;
        const std::string wanted = argv[3];
        while (std::getline(in, line)) {
            const auto tab = line.find('\t');
            if (tab != std::string::npos && line.substr(0, tab) == wanted) { s = fromText(line.substr(tab + 1)); break; }
        }
        if (s.empty()) { std::fprintf(stderr, "graph %s not found\n", wanted.c_str()); return 1; }
        const auto original = runChild(s, words, timeoutSeconds);
        const auto key = classKey(original);
        int evaluations = 0;
        const auto minimal = reduce(s, key, words, timeoutSeconds, evaluations);
        const auto check = runChild(minimal, words, timeoutSeconds);
        std::printf("original: %zu blocks  %s\t%s\nreduced:  %zu blocks (%d evaluations)  %s\t%s\n%s\n%s\n", s.size(), original.cls.c_str(), original.message.c_str(),
                    minimal.size(), evaluations, check.cls.c_str(), check.message.c_str(), toText(minimal).c_str(), toLiteral(minimal).c_str());
        return 0;
    }
    std::fprintf(stderr, "bad arguments\n");
    return 2;
}
