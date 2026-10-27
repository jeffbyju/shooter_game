#!/bin/bash
# cross-continent: $1=clients-per-generator-group $2=arm
SRV=$(awk '{print $1}' pn_srv_ip.txt); PUB=$SRV; PRIV=$(awk '{print $2}' pn_srv_ip.txt)
K="-n -o StrictHostKeyChecking=no -o ConnectTimeout=15"
mkdir -p xc
PER=$1; ARM=$2; TAG="$PER-$ARM"
case $ARM in
  all8) RT=8; CPUS="0-7"; IRQ="ff" ;;
  rsv6) RT=6; CPUS="2-7"; IRQ="3"  ;;
esac
TOT=$((PER*8))
echo "=== cross-continent: $PER/gen x8 gens = $TOT clients, arm=$ARM ==="
ssh $K -i pnkey.pem ec2-user@$SRV 'pkill -9 -x ge_arena_udp; true' >/dev/null 2>&1
for g in $(cat pn_gen_ips.txt); do ssh $K -i pnkey.pem ec2-user@$g 'pkill -9 -x ge_shooter_bots; true' >/dev/null 2>&1 & done
for ip in $(cat pn_xall.txt); do RG=$(grep -l "$ip" pn_xips-*.txt|head -1|sed 's/pn_xips-//;s/.txt//'); ssh $K -i pnkey-$RG.pem ec2-user@$ip 'pkill -9 -x ge_shooter_bots; true' >/dev/null 2>&1 & done
wait
ssh $K -i pnkey.pem ec2-user@$SRV "for i in \$(seq 56 63); do echo $IRQ | sudo tee /proc/irq/\$i/smp_affinity >/dev/null 2>&1; done" >/dev/null 2>&1
sleep 2
ssh $K -i pnkey.pem ec2-user@$SRV "cd ge && (setsid nohup taskset -c $CPUS ./build/ge_arena_udp --port 9200 --world 2000 --aoi 64 --max 16384 --rx-threads $RT > /tmp/xc-$TAG.log 2>&1 < /dev/null & echo \$! > /tmp/xcpid); sleep 3; cat /tmp/xcpid" >/dev/null 2>&1
sleep 2
SPID=$(ssh $K -i pnkey.pem ec2-user@$SRV 'cat /tmp/xcpid' 2>/dev/null)
ssh $K -i pnkey.pem ec2-user@$SRV "grep '^cpu' /proc/stat" 2>/dev/null > xc/m0-$TAG.txt
T0=$(date +%s)
# 4 local generators on the private address
for g in $(cat pn_gen_ips.txt); do
  ssh $K -i pnkey.pem ec2-user@$g "cd ge && ./build/ge_shooter_bots --host $PRIV --port 9200 --udp --bots $PER --seconds 45 --json" 2>/dev/null > xc/loc-$TAG-$g.json &
done
# 4 remote generators on the public address
for ip in $(cat pn_xall.txt); do
  RG=$(grep -l "$ip" pn_xips-*.txt|head -1|sed 's/pn_xips-//;s/.txt//')
  ssh $K -i pnkey-$RG.pem ec2-user@$ip "cd ge && ./build/ge_shooter_bots --host $PUB --port 9200 --udp --bots $PER --seconds 45 --json" 2>/dev/null > xc/rem-$TAG-$RG-$ip.json &
done
wait
T1=$(date +%s)
ssh $K -i pnkey.pem ec2-user@$SRV "grep '^cpu' /proc/stat" 2>/dev/null > xc/m1-$TAG.txt
ssh $K -i pnkey.pem ec2-user@$SRV "cp /tmp/xc-$TAG.log /tmp/kx-$TAG.log; pkill -9 -x ge_arena_udp; true" >/dev/null 2>&1
scp -o StrictHostKeyChecking=no -i pnkey.pem ec2-user@$SRV:/tmp/kx-$TAG.log xc/srv-$TAG.log >/dev/null 2>&1
echo "  elapsed $((T1-T0))s"
