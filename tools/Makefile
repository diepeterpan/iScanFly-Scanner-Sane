CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -I../include

all: s400w

s400w: scanner.o ../src/s400w.o
	$(CC) $(CFLAGS) -o $@ $^

scanner.o: scanner.c
	$(CC) $(CFLAGS) -c $< -o $@

../src/s400w.o: ../src/s400w.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f s400w *.o ../src/*.o

.PHONY: all clean
