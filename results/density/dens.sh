#!/bin/bash
SRV=$(awk '{print $1}' d_srv_ip.txt); PRIV=$(awk '{print $2}' d_srv_ip.txt)
GENS=($(cat d_gen_ips.txt)); NG=${#GENS[@]}
K="-n -o StrictHostKeyChecking=no -o ConnectTimeout=15 -i dkey.pem"
mkdir -p den
W=$1; N=4096; PER=$((N/NG)); TAG="w$W"
ssh $K ec2-user@$SRV 'pkill -9 -x ge_arena_udp; true' >/dev/null 2>&1
for g in "${GENS[@]}"; do ssh $K ec2-user@$g 'pkill -9 -x ge_shooter_bots; true' >/dev/null 2>&1 & done; wait
# best placement: 6 worker threads on cores 2-7, NIC interrupts on cores 0-1
ssh $K ec2-user@$SRV "for i in \$(seq 56 63); do echo 3 | sudo tee /proc/irq/\$i/smp_affinity >/dev/null 2>&1; done" >/dev/null 2>&1
sleep 2
ssh $K ec2-user@$SRV "cd ge && (setsid nohup taskset -c 2-7 ./build/ge_arena_udp --port 9200 --world $W --aoi 64 --max 8192 --rx-threads 6 > /tmp/d-$TAG.log 2>&1 < /dev/null & echo \$! > /tmp/dp); sleep 3; cat /tmp/dp" >/dev/null 2>&1
sleep 2
P=$(ssh $K ec2-user@$SRV 'cat /tmp/dp' 2>/dev/null)
echo "$P" > den/pid-$TAG.txt
# process CPU, whole machine, and PER-THREAD
ssh $K ec2-user@$SRV "awk '{print \$14+\$15}' /proc/$P/stat; grep '^cpu ' /proc/stat; echo ---; for t in /proc/$P/task/*/stat; do awk '{print \$1, \$14+\$15}' \$t; done" 2>/dev/null > den/c0-$TAG.txt
T0=$(date +%s)
for g in "${GENS[@]}"; do ssh $K ec2-user@$g "cd ge && ./build/ge_shooter_bots --host $PRIV --port 9200 --udp --bots $PER --seconds 40 --json" 2>/dev/null > den/g-$TAG-$g.json & done
wait
T1=$(date +%s); echo $((T1-T0)) > den/el-$TAG.txt
ssh $K ec2-user@$SRV "awk '{print \$14+\$15}' /proc/$P/stat; grep '^cpu ' /proc/stat; echo ---; for t in /proc/$P/task/*/stat; do awk '{print \$1, \$14+\$15}' \$t; done" 2>/dev/null > den/c1-$TAG.txt
ssh $K ec2-user@$SRV "cp /tmp/d-$TAG.log /tmp/k-$TAG.log; pkill -9 -x ge_arena_udp; true" >/dev/null 2>&1
scp -o StrictHostKeyChecking=no -i dkey.pem ec2-user@$SRV:/tmp/k-$TAG.log den/srv-$TAG.log >/dev/null 2>&1
echo "  world $W done"
