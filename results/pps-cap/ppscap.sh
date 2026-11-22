#!/bin/bash
SRV=$(awk '{print $1}' pps_srv_ip.txt); PRIV=$(awk '{print $2}' pps_srv_ip.txt)
GENS=($(cat pps_gen_ips.txt)); NG=${#GENS[@]}
K="-n -o StrictHostKeyChecking=no -o ConnectTimeout=15 -i ppskey.pem"
mkdir -p cap
N=$1; PER=$((N/NG)); TAG="$N"
ssh $K ec2-user@$SRV 'pkill -9 -x ge_arena_udp; true' >/dev/null 2>&1
for g in "${GENS[@]}"; do ssh $K ec2-user@$g 'pkill -9 -x ge_shooter_bots; true' >/dev/null 2>&1 & done; wait
sleep 2
ssh $K ec2-user@$SRV "cd ge && (setsid nohup ./build/ge_arena_udp --port 9200 --world 2000 --aoi 64 --max 16384 --rx-threads 6 > /tmp/c-$TAG.log 2>&1 < /dev/null &); sleep 3" >/dev/null 2>&1
ssh $K ec2-user@$SRV "/usr/sbin/ethtool -S ens5 | grep -E 'pps_allowance_exceeded|bw_out_allowance_exceeded'" > cap/e0-$TAG.txt 2>/dev/null
T0=$(date +%s)
for g in "${GENS[@]}"; do ssh $K ec2-user@$g "cd ge && ./build/ge_shooter_bots --host $PRIV --port 9200 --udp --bots $PER --seconds 40 --json" 2>/dev/null > cap/g-$TAG-$g.json & done
wait
T1=$(date +%s); echo $((T1-T0)) > cap/el-$TAG.txt
ssh $K ec2-user@$SRV "/usr/sbin/ethtool -S ens5 | grep -E 'pps_allowance_exceeded|bw_out_allowance_exceeded'" > cap/e1-$TAG.txt 2>/dev/null
ssh $K ec2-user@$SRV "cp /tmp/c-$TAG.log /tmp/k-$TAG.log; pkill -9 -x ge_arena_udp; true" >/dev/null 2>&1
scp -o StrictHostKeyChecking=no -i ppskey.pem ec2-user@$SRV:/tmp/k-$TAG.log cap/srv-$TAG.log >/dev/null 2>&1
echo "  $N done (${T1}s)"
