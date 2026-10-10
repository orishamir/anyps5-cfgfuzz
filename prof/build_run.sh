#!/bin/bash
# build_run.sh <variant>...: -pg builds of prof.cpp + variant Structurizer + ControlFlowGraph, run on first500 at 1000 words, gprof flat profile.
set -e
W=$(cd "$(dirname "$0")/.." && pwd); A=${ANYPS5:-/home/ori/dev/AnyPS5}; R=$A/core/shader/recompiler; cd $W/prof
INC="-I$R/RdnaDecoder/include -I$R -I$R/ControlFlow/include -I$A/core/libs"
[ -f ControlFlowGraph.o ] || g++ -std=c++20 -O2 -DNDEBUG -pg $INC -c $R/ControlFlow/src/ControlFlowGraph.cpp -o ControlFlowGraph.o
for v in "$@"; do
  g++ -std=c++20 -O2 -DNDEBUG -pg -I$W/variants/$v/inc $INC -c $W/variants/$v/Structurizer.cpp -o Structurizer_$v.o
  g++ -std=c++20 -O2 -DNDEBUG -pg -I$W/variants/$v/inc $INC -c prof.cpp -o prof_$v.o
  g++ -pg prof_$v.o Structurizer_$v.o ControlFlowGraph.o -o prof_$v
  rm -f gmon.out; echo "== $v: $(./prof_$v $W/prof/first500.graphs 1000)"; gprof -b -p prof_$v gmon.out > gprof_flat_$v.txt; gprof -b -q prof_$v gmon.out > gprof_call_$v.txt
done
