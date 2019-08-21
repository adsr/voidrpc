all: example

example: example.c voidrpc.h
	$(CC) -g -pthread example.c -o example

clean:
	rm -f example

.PHONY: all clean