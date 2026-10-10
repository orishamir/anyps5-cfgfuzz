# cfgfuzz: control-flow fuzzing for the AnyPS5 shader structurizer

Random structured programs (if/else, while and do-while loops, break, continue, return, shared returns)
run through `ShaderRecompiler::Structurizer`, plus an end-to-end path that assembles each shape into a real
wave64 compute shader and pushes it through `Recompile`, so SPIRV-Tools validation is the final oracle.

Everything below was run on AnyPS5 `main` a29ef592 (Ubuntu on WSL2, GCC 15.2.0, Release,
`ANYPS5_ENABLE_SPIRV_TOOLS=ON`, LLVM 21). Set `ANYPS5` to your checkout, built with `BUILD_TESTING=ON`.

## 1. Structurizer-only fuzzer (`cfgfuzz.cpp`)

Links the project's `ControlFlowGraph.cpp` and a `Structurizer.cpp` of your choice. Oracles per graph, each run
in a forked child with a 10 s alarm: structurizer exceptions (`BUDGET` for `... exceeded ... budget`, `REFUSED`
otherwise), then `requireStructuredBranches` and `requireExactPostDominators` copied from
`core/libs/prx/libSceAgcDriver/tests/Structurizer.cpp` (`INVARIANT`), `TIMEOUT`, `CRASH`.

```sh
ANYPS5=/path/to/AnyPS5
R=$ANYPS5/core/shader/recompiler
INC="-I$R/RdnaDecoder/include -I$R -I$R/ControlFlow/include -I$ANYPS5/core/libs"
g++ -std=c++20 -O2 $INC -c $R/ControlFlow/src/ControlFlowGraph.cpp -o ControlFlowGraph.o
g++ -std=c++20 -O2 $INC -c cfgfuzz.cpp -o cfgfuzz.o
# one binary per structurizer under test: main, a PR head, a fork branch, or a merge of them
g++ -std=c++20 -O3 -DNDEBUG $INC -c $R/ControlFlow/src/Structurizer.cpp -o Structurizer_main.o
g++ -O2 cfgfuzz.o Structurizer_main.o ControlFlowGraph.o -o cfgfuzz_main
```

For another branch, `git show <ref>:core/shader/recompiler/ControlFlow/src/Structurizer.cpp > Structurizer_x.cpp`,
compile it the same way and link it into `cfgfuzz_x`. (`git merge-file` on two such files gives the combined variant.)

```sh
./cfgfuzz_main gen 30000 3 corpus.graphs            # deterministic corpus; seed 3 is the one in the reports
./cfgfuzz_main run corpus.graphs main_w16.results 16 10   # <words per block> <timeout s>; 16 and 0 were both used
./cfgfuzz_main run corpus.graphs main_big.results 16 10 1000  # 7th arg: one pseudo-random block per graph gets 1000 words,
                                                    # which reaches privatizeSharedReturns (only taken above 256 words)
./cfgfuzz_main one "1;2,3;6;2,4;5,6;1;" 0           # one graph: successor lists per block, ';' separated
./cfgfuzz_main dump "1;2,3;6;2,4;5,6;1;" 0          # structurized blocks, merges, continue blocks, the rule's verdict
./cfgfuzz_main reduce corpus.graphs 442 16 10       # delta-reduce graph id 442 keeping its failure class
python3 reduce2.py ./cfgfuzz_main OK ./cfgfuzz_x BUDGET "<graph text>" 1000   # reduce keeping A's and B's classes (regressions)
```

`run_all.sh` runs the four variants at 0 and 16 words; `compare.py <results dir> <corpus>` prints per-variant class counts,
fixed/regressed/changed-class counts against `main`, and the smallest graph per failure class.
Graph text: `1;2,3;6;2,4;5,6;1;` means block 0 -> 1, block 1 -> {2,3}, block 2 -> 6, ..., block 6 returns.
`one` and `dump` print the graph as a `makeGraph` literal for `tests/Structurizer.cpp`, with blocks renumbered in breadth-first order from the entry (isomorphic to the input, ids may differ).

## 2. End to end (`e2e/`)

`cfg2shader.py` turns graph lines into shader words with `llvm-mc --triple=amdgcn --mcpu=gfx1013 --mattr=+wavefrontsize64`
(each block adds a constant to v1, conditionals compare v0, return blocks store v1 and `s_endpgm`; a return block is laid
out last because `DecodeRdnaProgram` requires the program to end in `s_endpgm`). `e2e.cpp` mirrors `verifyGraph` and
`recompile` from `tests/ControlFlow.cpp`: decode, `GraphBuilder`, `Structurize`, `verifyStructured`,
`verifySameExecutions`, `Recompile` (SPIRV-Tools validation), each shader in a forked child.

Build it with the compile and link lines of `agc_shader_control_flow_tests`, replacing the test object:

```sh
cd $ANYPS5/build
ninja -t commands tests/agc_shader_control_flow_tests | grep -- '-c .*tests/ControlFlow.cpp' | tail -1 > compile.txt
ninja -t commands tests/agc_shader_control_flow_tests | grep -- '-o tests/agc_shader_control_flow_tests' | tail -1 > link.txt
# compile: same flags, source e2e/e2e.cpp, output e2e.o (drop the -MD/-MT/-MF part)
# link: same line, replace .../tests/ControlFlow.cpp.o with e2e.o and the output name; to test a branch's structurizer,
#       put its Structurizer.o right after e2e.o so the archive's copy is not pulled in
```

```sh
python3 e2e/cfg2shader.py graphs.txt words.txt        # graphs.txt lines: <id>\t<graph text>
python3 e2e/cfg2shader.py graphs.txt words.txt --pad 17   # 17 extra v_add per block: ~1100 estimated words per block
./e2e_main words.txt results.txt 20                    # per line: id, DONE | INVALID_SPIRV | VALID_BUT_VERIFY_FAILED | STRUCTURIZE | ..., details
./e2e_main dump "<hex words>"                           # GraphBuilder CFG as a makeGraph literal, structurized blocks, verdicts
python3 e2e/reduce_e2e.py ./e2e_main "<graph text>" INVALID_SPIRV   # delta-reduce with the validator as oracle
python3 e2e/tabulate2.py                               # summarizes sample_<variant>.results per sample class
```


Environment knobs: `CFGFUZZ_BIG_INDEX=<block>` forces which block gets the large word count in `run`/`one`; `CFG2SHADER_PAD_BLOCK=<block>:<n>` pads one block instead of all.

## 3. Caveats

- Shapes are abstract; the real CFG that `GraphBuilder` builds from the assembled program can differ (an `s_branch`
  trampoline when a conditional's false target is not laid out next). Only end-to-end results were reported as findings;
  `dump` prints the GraphBuilder form so the structurizer-only test uses the same graph.
- The generator skips loops with no exit. Programs that the structurizer refuses (`REFUSED`) were not reported: the smallest
  such abstract shape compiled fine as real code.
- Resource guardrails used: at most 12 parallel runs, `ulimit -v`, outputs on disk.
