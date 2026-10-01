#!/bin/bash
SRV=$(awk '{print $1}' rp_srv_ip.txt); PRIV=$(awk '{print $2}' rp_srv_ip.txt)
GENS=($(cat rp_gen_ips.txt))
K="-n -o StrictHostKeyChecking=no -o ConnectTimeout=15 -i rpkey.pem"
mkdir -p rp
TOTAL=$1; RT=$2; PER=$((TOTAL/3)); TAG="$TOTAL-rt$RT"
echo "=== $TOTAL clients, --rx-threads $RT ==="
ssh $K ec2-user@$SRV 'pkill -9 -x ge_arena_udp; true' >/dev/null 2>&1
for g in "${GENS[@]}"; do ssh $K ec2-user@$g 'pkill -9 -x ge_shooter_bots; true' >/dev/null 2>&1 & done; wait
sleep 2
ssh $K ec2-user@$SRV "cd ge && (setsid nohup taskset -c 0-$((RT-1)) ./build/ge_arena_udp --port 9200 --world 2000 --aoi 64 --max 8192 --rx-threads $RT > /tmp/rp-$TAG.log 2>&1 < /dev/null & echo \$! > /tmp/rppid); sleep 3; cat /tmp/rppid" >/dev/null 2>&1
sleep 2
SPID=$(ssh $K ec2-user@$SRV 'cat /tmp/rppid' 2>/dev/null)
ssh $K ec2-user@$SRV "awk '{print \$14+\$15}' /proc/$SPID/stat" 2>/dev/null > rp/cpu0-$TAG.txt
for g in "${GENS[@]}"; do ssh $K ec2-user@$g "grep '^cpu ' /proc/stat" 2>/dev/null > rp/gen0-$TAG-$g.txt & done; wait
T0=$(date +%s)
for g in "${GENS[@]}"; do
  ssh $K ec2-user@$g "cd ge && ./build/ge_shooter_bots --host $PRIV --port 9200 --udp --bots $PER --seconds 45 --json" 2>/dev/null > rp/gen-$TAG-$g.json &
done
wait
T1=$(date +%s)
ssh $K ec2-user@$SRV "awk '{print \$14+\$15}' /proc/$SPID/stat" 2>/dev/null > rp/cpu1-$TAG.txt
for g in "${GENS[@]}"; do ssh $K ec2-user@$g "grep '^cpu ' /proc/stat" 2>/dev/null > rp/gen1-$TAG-$g.txt & done; wait
ssh $K ec2-user@$SRV "cp /tmp/rp-$TAG.log /tmp/k-$TAG.log; pkill -9 -x ge_arena_udp; true" >/dev/null 2>&1
scp -o StrictHostKeyChecking=no -i rpkey.pem ec2-user@$SRV:/tmp/k-$TAG.log rp/srv-$TAG.log >/dev/null 2>&1
EL=$((T1-T0)); C0=$(cat rp/cpu0-$TAG.txt 2>/dev/null||echo 0); C1=$(cat rp/cpu1-$TAG.txt 2>/dev/null||echo 0)
echo "  elapsed ${EL}s  server=$(echo "scale=2; ($C1-$C0)/($EL*100)" | bc) cores"
