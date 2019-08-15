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
ifeq ($(DEBUG),1)
	$(CC) $(WFLAGS) -DDEBUG -Og -g -c $(CFILES)
else
	$(CC) $(WFLAGS) -O2 -c $(CFILES)
endif

clean:
	rm *.o
