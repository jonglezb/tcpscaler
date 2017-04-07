# Description

These programs are designed to explore the limit of a TCP-based server with
a huge number of clients (tens of thousands, possibly millions).

You would typically run a single server instance, and then connect several
clients to the same server, from different computers on a network.  This is because
each client will be limited by the number of available ephemeral ports (a bit less
than 30k on Linux).

Both client and server use libevent to use an available high-performance I/O event
notification syscall.  Using select() would limit the server to 1024 open connections.

# Increasing the number of open files

By default, the maximum number of open files is quite low (often 1024).

You can increase it for the current shell using `ulimit`:

    ulimit -n 1048576

On recent Linux systems, the limit seems to be 1048576 for root, and 65536 for regular users.

# Running tcpserver

Usage:

    ./tcpserver 12345

The server will listen on :: on port 12345.  This will also accept IPv4 connections,
unless you have turned on `net.ipv6.bindv6only` (which is a bad idea for most cases).

# Running tcpclient

Usage:

    ./tcpclient <host> <port> <nb_connections>

The client will open `nb_connections` TCP connections to a server running at `host` and `port`.
