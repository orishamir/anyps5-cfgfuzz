#!/bin/bash
W=$(dirname "$(readlink -f "$0")"); cd "$W"   # expects corpus.graphs and cfgfuzz_<variant> here
for v in main pr2471 alxc20 both; do for w in 0 16; do
  ( ulimit -v 1000000; nice -n 10 ./cfgfuzz_$v run corpus.graphs results/${v}_w${w}.results $w 10 2> results/${v}_w${w}.log ) &
done; done
wait
echo done > results/DONE
