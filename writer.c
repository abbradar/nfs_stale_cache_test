#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Writer: runs on the NFS server, writes directly to the exported
 * filesystem.  Appends small chunks of 0xAA so the file grows through
 * page boundaries.  Uses O_SYNC + fdatasync so GETATTR from the client
 * sees the new size immediately.
 */

#define FILL_BYTE 0xAA
#define CHUNK_SIZE 1

int main(int argc, char *argv[])
{
	const char *path;
	unsigned char buf[CHUNK_SIZE];
	int fd;

	if (argc != 2) {
		fprintf(stderr, "usage: %s FILE\n", argv[0]);
		return 1;
	}
	path = argv[1];
	memset(buf, FILL_BYTE, sizeof(buf));

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0644);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	/* Seed with data straddling the first page boundary */
	for (;;) {
		ssize_t n = write(fd, buf, CHUNK_SIZE);
		if (n < 0) {
			perror("write");
			close(fd);
			return 1;
		}
	}

	close(fd);
	return 0;
}
