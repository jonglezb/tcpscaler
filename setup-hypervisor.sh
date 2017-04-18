#!/bin/sh
# Should be run as root

for i in $(seq 20)
do
  create_tap
done

iptables -t raw -A PREROUTING -p tcp -j NOTRACK
iptables -t raw -A OUTPUT -p tcp -j NOTRACK
