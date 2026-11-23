#!/bin/bash
set -x
cd $HOME
rm -rf ge out; mkdir -p ge out && tar xzf ge.tgz -C ge
cd ge
{ uname -a; lscpu | egrep 'Model name|^CPU\(s\)|Thread|Core|Socket'; perf --version; } > $HOME/out/00-env.txt 2>&1

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/usr/bin/gcc14-g++ -DCMAKE_C_COMPILER=/usr/bin/gcc14-gcc \
  -DCMAKE_CXX_FLAGS="-g -fno-omit-frame-pointer" > $HOME/out/01-cmake.txt 2>&1
cmake --build build -j8 > $HOME/out/02-build.txt 2>&1
echo "BUILD_RC=$?" >> $HOME/out/02-build.txt
./build/ge_tests > $HOME/out/03-tests.txt 2>&1; echo "TESTS_RC=$?" >> $HOME/out/03-tests.txt
tail -2 $HOME/out/03-tests.txt
./build/ge_shooter_bots --help > $HOME/out/00-bots-help.txt 2>&1

run_arm () {
  ARM=$1; BOTS=$2; FLAG=$3
  taskset -c 0 ./build/ge_arena_udp --port 9200 --world 2000 --aoi 64 --max 2048 $FLAG \
      > $HOME/out/$ARM-server.txt 2>&1 &
  SRV=$!
  sleep 3
  taskset -c 1-7 ./build/ge_shooter_bots --host 127.0.0.1 --port 9200 --udp \
      --bots $BOTS --seconds 110 > $HOME/out/$ARM-bots.txt 2>&1 &
  BOTPID=$!
  sleep 12
  echo "--- STAT WINDOW A START $(date -u +%H:%M:%S) ---" >> $HOME/out/$ARM-server.txt
  perf stat -e instructions,cycles -p $SRV -- sleep 20 > $HOME/out/$ARM-stat-ipc.txt 2>&1
  echo "--- STAT WINDOW A END $(date -u +%H:%M:%S) ---" >> $HOME/out/$ARM-server.txt
  sleep 2
  echo "--- STAT WINDOW B START $(date -u +%H:%M:%S) ---" >> $HOME/out/$ARM-server.txt
  perf stat -e instructions:u,instructions:k -p $SRV -- sleep 20 > $HOME/out/$ARM-stat-uk.txt 2>&1
  echo "--- STAT WINDOW B END $(date -u +%H:%M:%S) ---" >> $HOME/out/$ARM-server.txt
  sleep 2
  perf record -F 997 -e cpu-clock -p $SRV -o $HOME/out/$ARM.data -- sleep 20 \
      > $HOME/out/$ARM-record.txt 2>&1
  wait $BOTPID
  kill $SRV 2>/dev/null; sleep 2
  perf report -f -i $HOME/out/$ARM.data --sort symbol  --stdio > $HOME/out/$ARM-by-symbol.txt  2>&1
  perf report -f -i $HOME/out/$ARM.data --sort srcline --stdio > $HOME/out/$ARM-by-srcline.txt 2>&1
  rm -f $HOME/out/$ARM.data
}

run_arm aoiscan-1024 1024 --aoi-scan ; sleep 5
run_arm grid-1024    1024 ""         ; sleep 5
run_arm aoiscan-2048 2048 --aoi-scan ; sleep 5
run_arm grid-2048    2048 ""

for A in aoiscan-1024 grid-1024 aoiscan-2048 grid-2048; do
  echo "== $A ==" >> $HOME/out/04-candidates.txt
  grep -o 'aoicand/tick [0-9]*' $HOME/out/$A-server.txt | tail -8 >> $HOME/out/04-candidates.txt
done
echo RUN_COMPLETE
