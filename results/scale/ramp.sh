#!/bin/bash
SRV=$(awk '{print $1}' srv_ip.txt)
SRVPRIV=$(awk '{print $2}' srv_ip.txt)
GENS=($(tail -3 hosts.txt))
K="-n -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o BatchMode=yes -i loadkey.pem"
mkdir -p ramp
TOTAL=$1; PER=$((TOTAL/3))
echo "=== cell $TOTAL  ($PER per generator x3) ==="

ssh $K ec2-user@$SRV 'pkill -9 ge_arena_udp; true' >/dev/null 2>&1
for g in "${GENS[@]}"; do ssh $K ec2-user@$g 'pkill -9 ge_shooter_bots; true' >/dev/null 2>&1 & done; wait
sleep 1

ssh $K ec2-user@$SRV "cd ge && (setsid nohup taskset -c 2 ./build/ge_arena_udp --port 9200 --world 2000 --aoi 64 --max 8192 > /tmp/srv-$TOTAL.log 2>&1 < /dev/null & echo \$! > /tmp/srvpid) ; sleep 2; cat /tmp/srvpid" 2>/dev/null
sleep 2
SPID=$(ssh $K ec2-user@$SRV 'cat /tmp/srvpid' 2>/dev/null)
echo "  server pid=$SPID"

ssh $K ec2-user@$SRV "awk '{print \$14+\$15}' /proc/$SPID/stat" 2>/dev/null > ramp/cpu0-$TOTAL.txt
for g in "${GENS[@]}"; do ssh $K ec2-user@$g "grep '^cpu ' /proc/stat" 2>/dev/null > ramp/gen0-$TOTAL-$g.txt & done; wait
T0=$(date +%s)
for g in "${GENS[@]}"; do
  ssh $K ec2-user@$g "cd ge && ./build/ge_shooter_bots --host $SRVPRIV --port 9200 --udp --bots $PER --seconds 60 --json" 2>/dev/null > ramp/gen-$TOTAL-$g.json &
done
wait
T1=$(date +%s)
ssh $K ec2-user@$SRV "awk '{print \$14+\$15}' /proc/$SPID/stat" 2>/dev/null > ramp/cpu1-$TOTAL.txt
for g in "${GENS[@]}"; do ssh $K ec2-user@$g "grep '^cpu ' /proc/stat" 2>/dev/null > ramp/gen1-$TOTAL-$g.txt & done; wait
ssh $K ec2-user@$SRV "cp /tmp/srv-$TOTAL.log /tmp/keep-$TOTAL.log; pkill -9 ge_arena_udp; true" >/dev/null 2>&1
scp -o StrictHostKeyChecking=no -i loadkey.pem ec2-user@$SRV:/tmp/keep-$TOTAL.log ramp/srv-$TOTAL.log >/dev/null 2>&1
EL=$((T1-T0)); C0=$(cat ramp/cpu0-$TOTAL.txt 2>/dev/null || echo 0); C1=$(cat ramp/cpu1-$TOTAL.txt 2>/dev/null || echo 0)
echo "  elapsed ${EL}s  server_cpu=$(echo "scale=1; ($C1-$C0)/($EL*100)*100" | bc)% of one core"
