#!/bin/bash
# $1 = padded|unpadded   $2 = rep
SRV=$(awk '{print $1}' ab_srv_ip.txt); PRIV=$(awk '{print $2}' ab_srv_ip.txt)
GENS=($(cat ab_gen_ips.txt)); NG=${#GENS[@]}
K="-n -o StrictHostKeyChecking=no -o ConnectTimeout=15 -i abkey.pem"
mkdir -p ab
V=$1; REP=$2; N=8192; PER=$((N/NG)); TAG="$V-r$REP"
ssh $K ec2-user@$SRV 'pkill -9 -x ge_padded; pkill -9 -x ge_unpadded; true' >/dev/null 2>&1
for g in "${GENS[@]}"; do ssh $K ec2-user@$g 'pkill -9 -x ge_shooter_bots; true' >/dev/null 2>&1 & done; wait
ssh $K ec2-user@$SRV "for i in \$(seq 56 63); do echo 3 | sudo tee /proc/irq/\$i/smp_affinity >/dev/null 2>&1; done" >/dev/null 2>&1
sleep 2
ssh $K ec2-user@$SRV "(setsid nohup taskset -c 2-7 ~/ge_$V --port 9200 --world 2000 --aoi 64 --max 16384 --rx-threads 6 > /tmp/ab-$TAG.log 2>&1 < /dev/null & echo \$! > /tmp/abp); sleep 3; cat /tmp/abp" >/dev/null 2>&1
sleep 2
P=$(ssh $K ec2-user@$SRV 'cat /tmp/abp' 2>/dev/null)
ssh $K ec2-user@$SRV "awk '{print \$14+\$15}' /proc/$P/stat" 2>/dev/null > ab/c0-$TAG.txt
T0=$(date +%s)
for g in "${GENS[@]}"; do ssh $K ec2-user@$g "cd ge && ./build/ge_shooter_bots --host $PRIV --port 9200 --udp --bots $PER --seconds 40 --json" 2>/dev/null > ab/g-$TAG-$g.json & done
wait
T1=$(date +%s); echo $((T1-T0)) > ab/el-$TAG.txt
ssh $K ec2-user@$SRV "awk '{print \$14+\$15}' /proc/$P/stat" 2>/dev/null > ab/c1-$TAG.txt
ssh $K ec2-user@$SRV "cp /tmp/ab-$TAG.log /tmp/k-$TAG.log; pkill -9 -x ge_$V; true" >/dev/null 2>&1
scp -o StrictHostKeyChecking=no -i abkey.pem ec2-user@$SRV:/tmp/k-$TAG.log ab/srv-$TAG.log >/dev/null 2>&1
echo "  $V rep$REP done"
