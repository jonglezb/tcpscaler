CFLAGS = -Wall

all: tcpserver tcpclient udpclient

tcpserver: tcpserver.o
	$(CC) -levent -o $@ $<

tcpclient: tcpclient.o
	$(CC) -levent -lm -o $@ $<

udpclient: udpclient.o
	$(CC) -levent -lm -o $@ $<

clean:
	rm -f *.o tcpserver tcpclient udpclient
