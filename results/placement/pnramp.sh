#!/bin/bash
# $1=clients $2=arm   arm: all8 | rsv7 | all4 | rsv4
SRV=$(awk '{print $1}' pn_srv_ip.txt); PRIV=$(awk '{print $2}' pn_srv_ip.txt)
GENS=($(cat pn_gen_ips.txt)); NG=${#GENS[@]}
K="-n -o StrictHostKeyChecking=no -o ConnectTimeout=15 -i pnkey.pem"
mkdir -p pn
N=$1; ARM=$2; PER=$((N/NG)); TAG="$N-$ARM"
case $ARM in
  all8) RT=8; CPUS="0-7"; IRQ="default" ;;
  rsv7) RT=7; CPUS="1-7"; IRQ="0" ;;
  all4) RT=4; CPUS="0-3"; IRQ="default" ;;
  rsv4) RT=4; CPUS="1-4"; IRQ="0" ;;
  rsv6) RT=6; CPUS="2-7"; IRQ="01" ;;
esac
echo "=== $N clients, arm=$ARM (threads=$RT cpus=$CPUS irq=$IRQ) ==="
ssh $K ec2-user@$SRV 'pkill -9 -x ge_arena_udp; true' >/dev/null 2>&1
for g in "${GENS[@]}"; do ssh $K ec2-user@$g 'pkill -9 -x ge_shooter_bots; true' >/dev/null 2>&1 & done; wait
# interrupt affinity
if [ "$IRQ" = "0" ]; then
  ssh $K ec2-user@$SRV 'for i in $(seq 56 63); do echo 1 | sudo tee /proc/irq/$i/smp_affinity >/dev/null 2>&1; done; echo pinned' >/dev/null 2>&1
elif [ "$IRQ" = "01" ]; then
  ssh $K ec2-user@$SRV 'for i in $(seq 56 63); do echo 3 | sudo tee /proc/irq/$i/smp_affinity >/dev/null 2>&1; done; echo pinned2' >/dev/null 2>&1
else
  ssh $K ec2-user@$SRV 'for i in $(seq 56 63); do echo ff | sudo tee /proc/irq/$i/smp_affinity >/dev/null 2>&1; done; echo spread' >/dev/null 2>&1
fi
sleep 2
ssh $K ec2-user@$SRV "cd ge && (setsid nohup taskset -c $CPUS ./build/ge_arena_udp --port 9200 --world 2000 --aoi 64 --max 16384 --rx-threads $RT > /tmp/pn-$TAG.log 2>&1 < /dev/null & echo \$! > /tmp/pnpid); sleep 3; cat /tmp/pnpid" >/dev/null 2>&1
sleep 2
SPID=$(ssh $K ec2-user@$SRV 'cat /tmp/pnpid' 2>/dev/null)
echo "$SPID" > pn/pid-$TAG.txt
# whole-machine + per-thread, before
ssh $K ec2-user@$SRV "grep '^cpu' /proc/stat; echo ---; for t in /proc/$SPID/task/*/stat; do awk '{print \$1, \$14+\$15}' \$t; done" 2>/dev/null > pn/m0-$TAG.txt
for g in "${GENS[@]}"; do ssh $K ec2-user@$g "grep '^cpu ' /proc/stat" 2>/dev/null > pn/g0-$TAG-$g.txt & done; wait
T0=$(date +%s)
for g in "${GENS[@]}"; do
  ssh $K ec2-user@$g "cd ge && ./build/ge_shooter_bots --host $PRIV --port 9200 --udp --bots $PER --seconds 45 --json" 2>/dev/null > pn/gen-$TAG-$g.json &
done
wait
T1=$(date +%s); echo $((T1-T0)) > pn/el-$TAG.txt
ssh $K ec2-user@$SRV "grep '^cpu' /proc/stat; echo ---; for t in /proc/$SPID/task/*/stat; do awk '{print \$1, \$14+\$15}' \$t; done" 2>/dev/null > pn/m1-$TAG.txt
for g in "${GENS[@]}"; do ssh $K ec2-user@$g "grep '^cpu ' /proc/stat" 2>/dev/null > pn/g1-$TAG-$g.txt & done; wait
ssh $K ec2-user@$SRV "cp /tmp/pn-$TAG.log /tmp/kp-$TAG.log; pkill -9 -x ge_arena_udp; true" >/dev/null 2>&1
scp -o StrictHostKeyChecking=no -i pnkey.pem ec2-user@$SRV:/tmp/kp-$TAG.log pn/srv-$TAG.log >/dev/null 2>&1
echo "  elapsed $((T1-T0))s"
