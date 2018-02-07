CFLAGS = -Wall

all: tcpserver tcpclient

tcpserver: tcpserver.o
	$(CC) -levent -o $@ $<

tcpclient: tcpclient.o
	$(CC) -levent -lm -o $@ $<

clean:
	rm -f *.o tcpserver tcpclient
