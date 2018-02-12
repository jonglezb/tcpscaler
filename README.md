# Description

`tcpclient` and `tcpserver` are designed to explore the limit of a TCP-based server with
a huge number of clients (tens of thousands, possibly millions):

- `tcpclient` opens a large number of TCP connections to a single target, and then
  sends queries at a predefined rate, spread over the TCP connections.  The queries
  are formatted as DNS-over-TCP queries (query of type A for "example.com"), so that
  it is possible to connect `tcpclient` directly to a DNS server.
  `tcpclient` can optionally print the response time of every single query (option -R),
  assuming that the server replies with a DNS answer or just echoes back the query.

- `tcpserver` is a simple TCP server that accepts incoming connections, and echoes back
  anything sent to it.

You would typically run a single instance of `tcpserver`, and then connect several
clients to the same server, from different computers on a network.  This is because
each client will be limited by the number of available ephemeral ports (by default,
a bit less than 30k on Linux, but it is possible to increase it to around 60k, see below).
The server has no such limit, and can easily handle millions of concurrent connections
(provided the OS limits are high enough, see the performance tweaks below).

It is also possible to use `tcpclient` with a real DNS server (recursive resolver or
authoritative server), but note that most DNS servers do not expect to receive lots of
concurrent and long-running TCP connections.

See <https://github.com/jonglezb/dns-server-experiment> for a script that runs `tcpclient`
concurrently on a large number of virtual machines.  It also contains configuration for
`unbound` that allows it to handle millions of concurrent TCP clients.

Both client and server use `libevent` to benefit from high-performance I/O event
notification syscalls.  Using `select()` would limit the server to 1024 open connections.

# Performance

On a fairly recent laptop (i7-6600U, 2.60GHz), `tcpclient` has been tested to accurately generate
up to 300k queries per second, spread equally over 5 TCP connections.  That's around 220 Mbps of query traffic.
At this rate, we found that `unbound` had a really hard time replying fast enough, so we didn't try higher
rates, but `tcpclient` can probably generate queries even a bit faster than 300k qps.

With 2k TCP connections, `tcpclient` is somewhat less consistent, but it could still send around
150k to 200k queries per second.

# Server-side performance tweaks

See `setup-server.sh` script that does everything for you.

## Increase the number of open files

By default, the maximum number of open files is quite low (often 1024).

You can increase it for the current shell using `ulimit`:

    ulimit -n 1048576

On recent Linux systems, the limit seems to be 1048576 for root, and 65536 for regular users.

## Disable SYN cookies

With such a high number of TCP connections, Linux will start considering it is under
a DoS attack and send SYN cookies, with messages like this:

    TCP: TCP: Possible SYN flooding on port 4242. Sending cookies.  Check SNMP counters.

To disable SYN cookies on the server:

    sudo sysctl net.ipv4.tcp_syncookies=0

## Disable connection tracking or increase conntrack table size

If you cannot completely disable conntrack, you can insert a rule
to disable tracking for your experiment:

    sudo iptables -t raw -A PREROUTING -p tcp --dport <port> -j NOTRACK

Or you could instead increase the conntrack table size:

    sudo sysctl net.netfilter.nf_conntrack_max=1000000


# Client-side performance tweaks

See `setup-client.sh` script that does everything for you.

You can start by also increasing the maximum number of open files.

## Increase the number of ephemeral ports

By default, Linux uses port 32768 to 61000 for outgoing connections.

To be able to open more connections, change the port range on the client:

    sudo sysctl net.ipv4.ip_local_port_range="1024 65535"

## Decrease the timeout of TIME_WAIT state

When a client closes a TCP connection, the kernel keeps the connection in the
`TIME_WAIT` state for quite a long time (minutes).  It means that if you used
all ephemeral ports and then closed the connections, you can no longer open new
TCP connections for some time.

To work around this issue, enable reuse of `TIME_WAIT` connections on the client:

    sudo sysctl net.ipv4.tcp_tw_reuse=1

See this article <https://vincent.bernat.im/en/blog/2014-tcp-time-wait-state-linux>
for more details.


# Running tcpserver

Usage:

    ./tcpserver 12345

The server will listen on :: on port 12345.  This will also accept IPv4 connections,
unless you have turned on `net.ipv6.bindv6only` (which is a bad idea for most cases).

# Running tcpclient

Run `./tcpclient --help` for usage.
