DEBUG:=0
CC=gcc
WFLAGS=-Wall -Werror
CFILES=ftext.c
OBJS=ftext.o
LIBS=-lpthread -lm
BUILD=1.0.1

.PHONY: clean

ftext: $(OBJS)
	$(CC) $(WFLAGS) -o ftext $(OBJS) $(LIBS)

$(OBJS): $(CFILES)
	$(CC) $(WFLAGS) -c $(CFILES)

clean:
	rm *.o
