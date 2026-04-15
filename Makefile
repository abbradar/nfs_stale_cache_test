CC = gcc
CFLAGS = -O2 -Wall -pthread

all: writer reader

writer: writer.c
	$(CC) $(CFLAGS) -o $@ $<

reader: reader.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f writer reader
