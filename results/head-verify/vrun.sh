#!/bin/bash
SRV=$(awk '{print $1}' v_srv_ip.txt); PRIV=$(awk '{print $2}' v_srv_ip.txt)
GENS=($(cat v_gen_ips.txt)); NG=${#GENS[@]}
K="-n -o StrictHostKeyChecking=no -o ConnectTimeout=15 -i vkey.pem"
mkdir -p v
N=$1; PER=$((N/NG)); TAG="$N"
ssh $K ec2-user@$SRV 'pkill -9 -x ge_arena_udp; true' >/dev/null 2>&1
for g in "${GENS[@]}"; do ssh $K ec2-user@$g 'pkill -9 -x ge_shooter_bots; true' >/dev/null 2>&1 & done; wait
ssh $K ec2-user@$SRV "for i in \$(seq 56 63); do echo 3 | sudo tee /proc/irq/\$i/smp_affinity >/dev/null 2>&1; done" >/dev/null 2>&1
sleep 2
ssh $K ec2-user@$SRV "cd ge && (setsid nohup taskset -c 2-7 ./build/ge_arena_udp --port 9200 --world 2000 --aoi 64 --max 16384 --rx-threads 6 > /tmp/v-$TAG.log 2>&1 < /dev/null & echo \$! > /tmp/vp); sleep 3; cat /tmp/vp" >/dev/null 2>&1
sleep 2
P=$(ssh $K ec2-user@$SRV 'cat /tmp/vp' 2>/dev/null)
ssh $K ec2-user@$SRV "awk '{print \$14+\$15}' /proc/$P/stat; grep '^cpu ' /proc/stat" 2>/dev/null > v/c0-$TAG.txt
T0=$(date +%s)
for g in "${GENS[@]}"; do ssh $K ec2-user@$g "cd ge && ./build/ge_shooter_bots --host $PRIV --port 9200 --udp --bots $PER --seconds 45 --json" 2>/dev/null > v/g-$TAG-$g.json & done
wait
T1=$(date +%s); echo $((T1-T0)) > v/el-$TAG.txt
ssh $K ec2-user@$SRV "awk '{print \$14+\$15}' /proc/$P/stat; grep '^cpu ' /proc/stat" 2>/dev/null > v/c1-$TAG.txt
ssh $K ec2-user@$SRV "cp /tmp/v-$TAG.log /tmp/k-$TAG.log; pkill -9 -x ge_arena_udp; true" >/dev/null 2>&1
scp -o StrictHostKeyChecking=no -i vkey.pem ec2-user@$SRV:/tmp/k-$TAG.log v/srv-$TAG.log >/dev/null 2>&1
echo "  $N done"
