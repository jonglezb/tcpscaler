CFLAGS = -Wall

all: tcpclient udpclient

tcpclient.o: tcpclient.c common.h utils.h

udpclient.o: udpclient.c common.h utils.h

tcpserver: tcpserver.o
	$(CC) -levent -o $@ $<

tcpclient: tcpclient.o poisson.o utils.o
	$(CC) -levent -levent_openssl -lssl -lm -o $@ poisson.o utils.o $<

udpclient: udpclient.o
	$(CC) -levent -lm -o $@ poisson.o utils.o $<

clean:
	rm -f *.o tcpserver tcpclient udpclient
