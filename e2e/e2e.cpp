// End-to-end oracle: real RDNA code words -> decoder -> GraphBuilder -> Structurizer -> the project's own
// structured-CFG check and execution-equivalence check (copied from tests/ControlFlow.cpp) -> Recompile
// (which validates the SPIR-V with SPIRV-Tools when built with ANYPS5_ENABLE_SPIRV_TOOLS).
//
//   e2e <in.words> <out.results> [timeout-seconds]
// in.words lines: <id>\t<hex words>\t<blocks>      out lines: <id>\t<stage>\t<OK|message>\t<blocks>
// Stages: DECODE BUILD STRUCTURIZE VERIFY_STRUCT VERIFY_EXEC RECOMPILE DONE, plus TIMEOUT/CRASH.
#include "ControlFlow/GraphBuilder.hpp"
#include "ControlFlow/Structurizer.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "Recompiler.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <map>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace ShaderRecompiler;
using RouteState = std::map<std::uint32_t, bool>;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::uint32_t> constructBlocks(const ControlFlowGraph& graph, std::uint32_t header, std::uint32_t exclude) {
    std::vector<std::uint32_t> blocks;
    for (const auto& block : graph.blocks)
        if (graph.Dominates(header, block.id) && (exclude == InvalidControlFlowId || !graph.Dominates(exclude, block.id))) blocks.push_back(block.id);
    return blocks;
}

bool inEnclosingContinueConstruct(const ControlFlowGraph& graph, std::uint32_t header, std::uint32_t member) {
    return std::any_of(graph.blocks.begin(), graph.blocks.end(), [&](const BasicBlock& loop) {
        const auto continueBlock = loop.terminator.continueBlock;
        return loop.terminator.loopHeader && graph.Dominates(loop.id, header) && !graph.Dominates(continueBlock, header) && graph.Dominates(continueBlock, member);
    });
}

void verifyStructured(const std::string& prefix, const ControlFlowGraph& graph) {
    std::map<std::uint32_t, std::uint32_t> mergeOwners;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> loopExits;
    for (const auto& block : graph.blocks) {
        const auto& terminator = block.terminator;
        if (terminator.mergeBlock == InvalidControlFlowId) continue;
        require(mergeOwners.emplace(terminator.mergeBlock, block.id).second, prefix + "block " + std::to_string(terminator.mergeBlock) + " merges two constructs");
        if (terminator.loopHeader) loopExits.emplace_back(terminator.mergeBlock, terminator.continueBlock);
    }
    for (const auto& block : graph.blocks) {
        const auto& terminator = block.terminator;
        if (terminator.loopHeader || terminator.mergeBlock == InvalidControlFlowId) continue;
        for (const auto member : constructBlocks(graph, block.id, terminator.mergeBlock)) {
            if (inEnclosingContinueConstruct(graph, block.id, member)) continue;
            for (const auto successor : graph.FindBlock(member).successors) {
                if (successor == terminator.mergeBlock || (graph.Dominates(block.id, successor) && !graph.Dominates(terminator.mergeBlock, successor))) continue;
                const bool loopExit = std::any_of(loopExits.begin(), loopExits.end(), [&](const auto& exits) { return successor == exits.first || successor == exits.second; });
                require(loopExit, prefix + "block " + std::to_string(member) + " of the selection at block " + std::to_string(block.id) + " branches to block " + std::to_string(successor) + " outside the construct");
            }
        }
    }
    for (const auto& loop : graph.blocks) {
        if (!loop.terminator.loopHeader) continue;
        const auto continueBlock = loop.terminator.continueBlock;
        for (const auto member : constructBlocks(graph, loop.id, loop.terminator.mergeBlock)) {
            const auto& terminator = graph.FindBlock(member).terminator;
            if (terminator.loopHeader || graph.Dominates(continueBlock, member)) continue;
            require(terminator.mergeBlock != continueBlock, prefix + "the selection at block " + std::to_string(member) + " in the loop at block " + std::to_string(loop.id) + " merges at the loop's continue block " + std::to_string(continueBlock));
        }
    }
}

std::uint32_t followEmptyBlocks(const std::string& prefix, const ControlFlowGraph& graph, std::uint32_t blockId, RouteState& routes) {
    for (std::size_t steps = 0; steps <= graph.blocks.size(); ++steps) {
        const auto& block = graph.FindBlock(blockId);
        const auto& terminator = block.terminator;
        const bool empty = block.instructionBegin == block.instructionEnd;
        require(terminator.gotoValue < 0 || empty, prefix + "block " + std::to_string(block.id) + " sets a route variable after guest instructions");
        if (terminator.gotoValue >= 0) routes[terminator.gotoVariable] = terminator.gotoValue != 0;
        if (!empty) return blockId;
        if (terminator.kind == TerminatorKind::Branch) {
            blockId = terminator.trueBlock;
        } else if (terminator.kind == TerminatorKind::ConditionalBranch && terminator.condition == BranchCondition::GotoVariable) {
            const auto route = routes.find(terminator.gotoVariable);
            require(route != routes.end(), prefix + "block " + std::to_string(block.id) + " reads route variable " + std::to_string(terminator.gotoVariable) + " before any path sets it");
            blockId = route->second ? terminator.trueBlock : terminator.falseBlock;
        } else {
            return blockId;
        }
    }
    throw std::runtime_error(prefix + "a cycle of empty blocks");
}

void verifySameExecutions(const std::string& prefix, const ControlFlowGraph& original, const ControlFlowGraph& structured) {
    std::vector<std::tuple<std::uint32_t, std::uint32_t, RouteState>> pending{{structured.blocks.front().id, original.entryBlock, {}}};
    std::vector<std::tuple<std::uint32_t, std::uint32_t, RouteState>> visited;
    RouteState none;
    while (!pending.empty()) {
        auto [structuredId, originalId, routes] = pending.back();
        pending.pop_back();
        const auto copyId = followEmptyBlocks(prefix, structured, structuredId, routes);
        const auto sourceId = followEmptyBlocks(prefix, original, originalId, none);
        auto state = std::make_tuple(copyId, sourceId, routes);
        if (std::find(visited.begin(), visited.end(), state) != visited.end()) continue;
        visited.push_back(state);
        const auto& copy = structured.FindBlock(copyId);
        const auto& source = original.FindBlock(sourceId);
        require(copy.instructionBegin == source.instructionBegin && copy.instructionEnd == source.instructionEnd, prefix + "block " + std::to_string(copy.id) + " runs other instructions than block " + std::to_string(source.id));
        require(copy.terminator.kind == source.terminator.kind && copy.terminator.condition == source.terminator.condition, prefix + "block " + std::to_string(copy.id) + " branches differently from block " + std::to_string(source.id));
        if (source.terminator.kind == TerminatorKind::Branch || source.terminator.kind == TerminatorKind::ConditionalBranch) pending.emplace_back(copy.terminator.trueBlock, source.terminator.trueBlock, routes);
        if (source.terminator.kind == TerminatorKind::ConditionalBranch) pending.emplace_back(copy.terminator.falseBlock, source.terminator.falseBlock, routes);
    }
}


const BasicBlock* innermostLoopHeader(const ControlFlowGraph& graph, std::uint32_t blockId) {
    const BasicBlock* innermost = nullptr; std::size_t innermostSize = 0;
    for (const auto& header : graph.blocks) {
        if (!header.terminator.loopHeader || !graph.Dominates(header.id, blockId) || graph.Dominates(header.terminator.mergeBlock, blockId) || blockId == header.terminator.mergeBlock) continue;
        const auto size = static_cast<std::size_t>(std::count_if(graph.blocks.begin(), graph.blocks.end(), [&](const BasicBlock& block) { return graph.Dominates(header.id, block.id) && !graph.Dominates(header.terminator.mergeBlock, block.id); }));
        if (innermost == nullptr || size < innermostSize) { innermost = &header; innermostSize = size; }
    }
    return innermost;
}
std::string structuredBranchViolation(const ControlFlowGraph& graph) {
    for (const auto& block : graph.blocks) {
        const auto& t = block.terminator;
        if (t.kind != TerminatorKind::ConditionalBranch || t.loopHeader || t.trueBlock == t.falseBlock) continue;
        const auto* loop = innermostLoopHeader(graph, block.id);
        if (t.mergeBlock != InvalidControlFlowId) {
            if (loop != nullptr && t.mergeBlock == loop->terminator.continueBlock) return "block " + std::to_string(block.id) + " merges at its loop's continue block";
            continue;
        }
        const auto exits = [&](std::uint32_t target) { return loop != nullptr && (target == loop->terminator.mergeBlock || target == loop->terminator.continueBlock); };
        if (!exits(t.trueBlock) && !exits(t.falseBlock)) return "block " + std::to_string(block.id) + " branches to " + std::to_string(t.trueBlock) + "/" + std::to_string(t.falseBlock) + " without a merge, and neither target is its loop's merge or continue" + (loop ? " (innermost loop " + std::to_string(loop->id) + ", merge " + std::to_string(loop->terminator.mergeBlock) + ", continue " + std::to_string(loop->terminator.continueBlock) + ")" : " (not in a loop)");
    }
    return "";
}
void dumpGraph(const ControlFlowGraph& graph, std::size_t originalCount) {
    const char* kinds[] = {"Branch", "CondBranch", "IndirectBranch", "Return", "Unsupported"};
    for (const auto& b : graph.blocks) {
        const auto& t = b.terminator;
        std::printf("  block %2u  succ=[", b.id);
        for (std::size_t i = 0; i < b.successors.size(); ++i) std::printf("%s%u", i ? "," : "", b.successors[i]);
        std::printf("]  %s", kinds[static_cast<int>(t.kind)]);
        if (t.kind == TerminatorKind::ConditionalBranch) std::printf("(true=%u false=%u)", t.trueBlock, t.falseBlock);
        if (t.loopHeader) std::printf("  LOOP merge=%u continue=%u", t.mergeBlock, t.continueBlock);
        else if (t.mergeBlock != InvalidControlFlowId) std::printf("  merge=%u", t.mergeBlock);
        if (t.gotoVariable != InvalidControlFlowId) std::printf("  goto var=%u value=%d", t.gotoVariable, t.gotoValue);
        std::printf("  instr=[%u,%u) words=%u pc=0x%x%s\n", b.instructionBegin, b.instructionEnd, b.estimatedSpirvWords, b.startProgramCounter, b.id >= originalCount ? " NEW" : "");
    }
}

std::size_t recompile(std::span<const std::uint32_t> code) {
    const std::array<std::uint32_t, 4> userData{0x10000000u, 0x00100000u, 0x40u, 0x00027facu};
    const std::array<std::uint32_t, 2> capabilities{1u, 61u};
    const std::array<std::string_view, 1> extensions{"SPV_KHR_storage_buffer_storage_class"};
    RecompileRequest request{};
    request.shader = {ShaderStage::Compute, 0x20000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.userDataBaseRegister = 0;
    request.context.userData = userData;
    request.context.compute = ShaderComputeStageInfo{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    request.target.vulkanVersion = 0x00403000u;
    request.target.spirvVersion = 0x00010600u;
    request.target.subgroupSize = 64u;
    request.target.bdaAbiVersion = 1;
    request.target.supportedCapabilities = capabilities;
    request.target.supportedExtensions = extensions;
    request.target.maxWorkgroupSize = {1024u, 1024u, 64u};
    request.target.maxWorkgroupInvocations = 1024;
    request.target.maxWorkgroupSharedMemoryBytes = 49152;
    request.layout = {0, 0, 0, 128};
    request.useCache = false;
    const auto result = Recompile(request);
    require(!result.spirv.empty(), "no SPIR-V");
    return result.spirv.size();
}

std::string runOne(const std::vector<std::uint32_t>& code) {
    // Runs every stage; a failing verify stage is recorded but recompilation still runs so the SPIR-V validator
    // (the strongest oracle) is always consulted. Output: <summary>\t<details>
    std::string notes;
    try {
        const auto decoded = RdnaInstructionDecoder{}.Decode(code);
        const auto original = GraphBuilder{}.Build(decoded);
        auto graph = original;
        try { Structurizer{}.Structurize(graph); } catch (const std::exception& e) { return std::string("STRUCTURIZE\t") + e.what(); }
        try { verifyStructured("", graph); } catch (const std::exception& e) { notes += "[verifyStructured: " + std::string(e.what()) + "] "; }
        try { verifySameExecutions("", original, graph); } catch (const std::exception& e) { notes += "[verifySameExecutions: " + std::string(e.what()) + "] "; }
        std::string spirv;
        try { spirv = "valid " + std::to_string(recompile(code)) + " words"; } catch (const std::exception& e) {
            const std::string what = e.what();
            const bool validation = what.find("validation") != std::string::npos;
            return (validation ? "INVALID_SPIRV\t" : "RECOMPILE_ERROR\t") + notes + what.substr(0, 220);
        }
        if (!notes.empty()) return "VALID_BUT_VERIFY_FAILED\t" + notes + spirv;
        return "DONE\t" + spirv + ", " + std::to_string(original.blocks.size()) + "->" + std::to_string(graph.blocks.size()) + " blocks";
    } catch (const std::exception& e) {
        return std::string("DECODE_OR_BUILD\t") + e.what();
    }
}

std::string runChild(const std::vector<std::uint32_t>& code, unsigned timeoutSeconds) {
    int fds[2];
    if (pipe(fds) != 0) return "CRASH\tpipe";
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        alarm(timeoutSeconds);
        auto r = runOne(code);
        for (char& c : r) if (c == '\n') c = ' ';
        (void)!write(fds[1], r.data(), r.size());
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    std::string buffer; char chunk[4096]; ssize_t n;
    while ((n = read(fds[0], chunk, sizeof(chunk))) > 0) buffer.append(chunk, static_cast<std::size_t>(n));
    close(fds[0]);
    int status = 0; waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return WTERMSIG(status) == SIGALRM ? "TIMEOUT\t" + std::to_string(timeoutSeconds) + " s" : "CRASH\tsignal " + std::to_string(WTERMSIG(status));
    if (buffer.find('\t') == std::string::npos) return "CRASH\tno result";
    return buffer;
}
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: e2e <in.words> <out.results> [timeout]\n"); return 2; }
    if (std::string(argv[1]) == "dump" && argc >= 3) {
        std::vector<std::uint32_t> code; std::stringstream hs(argv[2]); std::string w;
        while (hs >> w) code.push_back(static_cast<std::uint32_t>(std::strtoul(w.c_str(), nullptr, 16)));
        const auto decoded = RdnaInstructionDecoder{}.Decode(code);
        const auto original = GraphBuilder{}.Build(decoded);
        std::printf("GraphBuilder CFG (%zu blocks), makeGraph form: {", original.blocks.size());
        for (std::size_t i = 0; i < original.blocks.size(); ++i) { std::printf("%s{", i ? ", " : ""); for (std::size_t j = 0; j < original.blocks[i].successors.size(); ++j) std::printf("%s%u", j ? ", " : "", original.blocks[i].successors[j]); std::printf("}"); }
        std::printf("}\n"); dumpGraph(original, original.blocks.size());
        auto graph = original;
        try { Structurizer{}.Structurize(graph); } catch (const std::exception& e) { std::printf("structurizer threw: %s\n", e.what()); return 1; }
        std::printf("structurized (%zu blocks):\n", graph.blocks.size()); dumpGraph(graph, original.blocks.size());
        const auto violation = structuredBranchViolation(graph);
        std::printf("structured-branch rule: %s\n", violation.empty() ? "OK" : violation.c_str());
        try { verifyStructured("", graph); std::printf("verifyStructured: OK\n"); } catch (const std::exception& e) { std::printf("verifyStructured: %s\n", e.what()); }
        try { std::printf("recompile: valid, %zu words\n", recompile(code)); } catch (const std::exception& e) { std::string m = e.what(); const auto r = m.find("RecompileRequest:"); if (r != std::string::npos) m.resize(r); std::printf("recompile: %s\n", m.c_str()); }
        return 0;
    }

    const unsigned timeoutSeconds = argc >= 4 ? static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10)) : 20u;
    std::ifstream in(argv[1]);
    std::ofstream out(argv[2]);
    std::string line; std::size_t count = 0;
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string id, hex, blocks;
        std::getline(ss, id, '\t'); std::getline(ss, hex, '\t'); std::getline(ss, blocks, '\t');
        std::vector<std::uint32_t> code;
        std::stringstream hs(hex); std::string w;
        while (hs >> w) code.push_back(static_cast<std::uint32_t>(std::strtoul(w.c_str(), nullptr, 16)));
        out << id << '\t' << runChild(code, timeoutSeconds) << '\t' << blocks << '\n';
        ++count;
    }
    std::fprintf(stderr, "e2e: %zu shaders\n", count);
    return 0;
}
