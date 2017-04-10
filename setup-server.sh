#!/bin/sh

# Should be run as root

ulimit -n 65536
ulimit -n 1048576

sysctl net.ipv4.tcp_syncookies=0
sysctl net.core.somaxconn=8192

iptables -t raw -A PREROUTING -p tcp -j NOTRACK
iptables -t raw -A OUTPUT -p tcp -j NOTRACK
