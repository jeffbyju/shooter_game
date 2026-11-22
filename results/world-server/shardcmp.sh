#!/bin/bash
# $1 = udp | tcp ; 4096 clients across 4 worlds, 8 threads either way
SRV=$(awk '{print $1}' pps_srv_ip.txt); PRIV=$(awk '{print $2}' pps_srv_ip.txt)
GENS=($(cat pps_gen_ips.txt)); NG=${#GENS[@]}
K="-n -o StrictHostKeyChecking=no -o ConnectTimeout=15 -i ppskey.pem"
mkdir -p sh
MODE=$1; N=4096; PERW=$((N/NG/4)); TAG="$MODE"
echo "=== $MODE: 4096 clients, 4 worlds, 8 threads ==="
ssh $K ec2-user@$SRV 'pkill -9 -x ge_arena_udp; pkill -9 -x ge_world_server; true' >/dev/null 2>&1
for g in "${GENS[@]}"; do ssh $K ec2-user@$g 'pkill -9 -x ge_shooter_bots; true' >/dev/null 2>&1 & done; wait
sleep 2
if [ "$MODE" = udp ]; then
  ssh $K ec2-user@$SRV "cd ge && for p in 9201 9202 9203 9204; do (setsid nohup ./build/ge_arena_udp --port \$p --world 2000 --aoi 64 --max 4096 --rx-threads 2 > /tmp/s-\$p.log 2>&1 < /dev/null &); done; sleep 4; pgrep -x ge_arena_udp | tr '\n' ' ' > /tmp/sp" >/dev/null 2>&1
else
  ssh $K ec2-user@$SRV "cd ge && (setsid nohup ./build/ge_world_server --host 0.0.0.0 --port 7777 --mode arena --shards 4 --io-threads 4 --max-conns 8192 --backlog 8192 --world 2000 --aoi 64 > /tmp/s-7777.log 2>&1 < /dev/null &); sleep 4; pgrep -x ge_world_server | tr '\n' ' ' > /tmp/sp" >/dev/null 2>&1
fi
PIDS=$(ssh $K ec2-user@$SRV 'cat /tmp/sp' 2>/dev/null)
ssh $K ec2-user@$SRV "for p in $PIDS; do awk '{print \$14+\$15}' /proc/\$p/stat; done" 2>/dev/null > sh/c0-$TAG.txt
T0=$(date +%s)
for g in "${GENS[@]}"; do
  if [ "$MODE" = udp ]; then
    for p in 9201 9202 9203 9204; do
      ssh $K ec2-user@$g "cd ge && ./build/ge_shooter_bots --host $PRIV --port $p --udp --bots $PERW --seconds 40 --json" 2>/dev/null > sh/g-$TAG-$g-$p.json &
    done
  else
    ssh $K ec2-user@$g "cd ge && ./build/ge_shooter_bots --host $PRIV --port 7777 --bots $((PERW*4)) --seconds 40 --json" 2>/dev/null > sh/g-$TAG-$g-7777.json &
  fi
done
wait
T1=$(date +%s); echo $((T1-T0)) > sh/el-$TAG.txt
ssh $K ec2-user@$SRV "for p in $PIDS; do awk '{print \$14+\$15}' /proc/\$p/stat; done" 2>/dev/null > sh/c1-$TAG.txt
ssh $K ec2-user@$SRV "mkdir -p /tmp/kp && cp /tmp/s-*.log /tmp/kp/ 2>/dev/null; pkill -9 -x ge_arena_udp; pkill -9 -x ge_world_server; true" >/dev/null 2>&1
rm -f sh/srv-$TAG-*.log; for f in $(ssh $K ec2-user@$SRV 'ls /tmp/kp/' 2>/dev/null); do scp -o StrictHostKeyChecking=no -i ppskey.pem "ec2-user@$SRV:/tmp/kp/$f" "sh/srv-$TAG-$f" >/dev/null 2>&1; done
echo "  elapsed $((T1-T0))s"
