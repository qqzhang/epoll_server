CC     = g++
CFLAGS = -O2 -lpthread 
DEBUG  = -Wall -g -rdynamic

C_SRCS = epoll_server.c 

OBJECTS = $(C_SRCS:.c=.o) 

.PHONY: all clean

all: server 

server: $(OBJECTS)
	$(CC) $(DEBUG) $(CFLAGS) $(OBJECTS) -o $@

.c.o:
	$(CC) $(DEBUG) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o  

