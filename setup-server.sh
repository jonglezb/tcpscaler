#!/bin/sh
# Should be run as root

sysctl net.ipv4.tcp_syncookies=0
sysctl net.core.somaxconn=8192
sysctl fs.file-max=12582912

iptables -t raw -A PREROUTING -p tcp -j NOTRACK
iptables -t raw -A OUTPUT -p tcp -j NOTRACK
