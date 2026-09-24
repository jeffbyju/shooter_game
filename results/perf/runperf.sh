#!/bin/bash
set -x
cd $HOME
mkdir -p ge out && tar xzf ge.tgz -C ge
cd ge
{ uname -a; lscpu | egrep 'Model name|^CPU\(s\)|Thread|Core|Socket'; perf --version; } > $HOME/out/00-env.txt 2>&1

rm -rf build; cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/gcc14-g++ -DCMAKE_C_COMPILER=/usr/bin/gcc14-gcc -DCMAKE_CXX_FLAGS="-g -fno-omit-frame-pointer" > $HOME/out/01-cmake.txt 2>&1
cmake --build build -j8 > $HOME/out/02-build.txt 2>&1
echo "BUILD_RC=$?" >> $HOME/out/02-build.txt
./build/ge_tests > $HOME/out/03-tests.txt 2>&1; echo "TESTS_RC=$?" >> $HOME/out/03-tests.txt
tail -2 $HOME/out/03-tests.txt

run_arm () {
  ARM=$1; FLAG=$2
  taskset -c 0 ./build/ge_arena_udp --port 9200 --world 2000 --aoi 64 --max 2048 $FLAG \
      > $HOME/out/$ARM-server.txt 2>&1 &
  SRV=$!
  sleep 3
  taskset -c 1-7 ./build/ge_shooter_bots --host 127.0.0.1 --port 9200 --udp \
      --bots 1024 --seconds 40 > $HOME/out/$ARM-bots.txt 2>&1 &
  BOTS=$!
  sleep 8
  perf record -F 997 -e cpu-clock -p $SRV -o $HOME/out/$ARM.data -- sleep 20 \
      > $HOME/out/$ARM-record.txt 2>&1
  wait $BOTS
  kill $SRV 2>/dev/null; sleep 2
  perf report -f -i $HOME/out/$ARM.data --sort symbol  --stdio > $HOME/out/$ARM-by-symbol.txt  2>&1
  perf report -f -i $HOME/out/$ARM.data --sort srcline --stdio > $HOME/out/$ARM-by-srcline.txt 2>&1
}

run_arm aoiscan --aoi-scan
sleep 5
run_arm grid ""

grep -o 'aoicand/tick [0-9]*' $HOME/out/aoiscan-server.txt | tail -5 >  $HOME/out/04-candidates.txt
grep -o 'aoicand/tick [0-9]*' $HOME/out/grid-server.txt    | tail -5 >> $HOME/out/04-candidates.txt
echo RUN_COMPLETE
