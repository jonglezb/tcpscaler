CFLAGS = -Wall

all: tcpclient udpclient

tcpclient.o: tcpclient.c common.h utils.h

udpclient.o: udpclient.c common.h utils.h

tcpserver: tcpserver.o
	$(CC) -o $@ $< -levent

tcpclient: tcpclient.o poisson.o utils.o
	$(CC) -o $@ poisson.o utils.o $< -levent -levent_openssl -lssl -lm

udpclient: udpclient.o
	$(CC) -o $@ poisson.o utils.o $< -levent -lm

clean:
	rm -f *.o tcpserver tcpclient udpclient
