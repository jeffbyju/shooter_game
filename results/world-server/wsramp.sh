#!/bin/bash
# $1=clients $2=shards $3=io-threads $4=backend $5=backlog $6=tag-suffix
SRV=$(awk '{print $1}' ws_srv_ip.txt); PRIV=$(awk '{print $2}' ws_srv_ip.txt)
GENS=($(cat ws_gen_ips.txt)); NG=${#GENS[@]}
mkdir -p ws
N=$1; SH=$2; IOT=$3; BE=${4:-poll}; BL=${5:-8192}; SUF=${6:-}
PER=$((N/NG)); TAG="$N-s$SH-io$IOT-$BE-bl$BL$SUF"
echo "=== $N clients | shards=$SH io=$IOT backend=$BE backlog=$BL ==="
ssh -n -o StrictHostKeyChecking=no -i wskey.pem ec2-user@$SRV 'pkill -9 -x ge_world_server; pkill -9 -x ge_arena_udp; true' >/dev/null 2>&1
for g in "${GENS[@]}"; do ssh -n -o StrictHostKeyChecking=no -i wskey.pem ec2-user@$g 'pkill -9 -x ge_shooter_bots; true' >/dev/null 2>&1 & done; wait
sleep 2
ssh -n -o StrictHostKeyChecking=no -i wskey.pem ec2-user@$SRV "cd ge && (setsid nohup ./build/ge_world_server --host 0.0.0.0 --port 7777 --mode arena --shards $SH --io-threads $IOT --max-conns 8192 --backlog $BL --io-backend $BE --world 2000 --aoi 64 > /tmp/ws-$TAG.log 2>&1 < /dev/null & echo \$! > /tmp/wspid); sleep 3; cat /tmp/wspid" >/dev/null 2>&1
sleep 2
SPID=$(ssh -n -o StrictHostKeyChecking=no -i wskey.pem ec2-user@$SRV 'cat /tmp/wspid' 2>/dev/null)
ssh -n -o StrictHostKeyChecking=no -i wskey.pem ec2-user@$SRV "awk '{print \$14+\$15}' /proc/$SPID/stat; grep '^cpu ' /proc/stat" 2>/dev/null > ws/c0-$TAG.txt
for g in "${GENS[@]}"; do ssh -n -o StrictHostKeyChecking=no -i wskey.pem ec2-user@$g "grep '^cpu ' /proc/stat" 2>/dev/null > ws/g0-$TAG-$g.txt & done; wait
T0=$(date +%s)
for g in "${GENS[@]}"; do
  ssh -n -o StrictHostKeyChecking=no -i wskey.pem ec2-user@$g "cd ge && ./build/ge_shooter_bots --host $PRIV --port 7777 --bots $PER --seconds 45 --json" 2>/dev/null > ws/gen-$TAG-$g.json &
done
wait
T1=$(date +%s); echo $((T1-T0)) > ws/el-$TAG.txt
ssh -n -o StrictHostKeyChecking=no -i wskey.pem ec2-user@$SRV "awk '{print \$14+\$15}' /proc/$SPID/stat; grep '^cpu ' /proc/stat" 2>/dev/null > ws/c1-$TAG.txt
ssh -n -o StrictHostKeyChecking=no -i wskey.pem ec2-user@$SRV "cp /tmp/ws-$TAG.log /tmp/kw-$TAG.log; pkill -9 -x ge_world_server; true" >/dev/null 2>&1
scp -o StrictHostKeyChecking=no -i wskey.pem ec2-user@$SRV:/tmp/kw-$TAG.log ws/srv-$TAG.log >/dev/null 2>&1
echo "  elapsed $((T1-T0))s"
