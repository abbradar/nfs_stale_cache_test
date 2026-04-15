#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * Reader + stat-hammer for the NFS/FUSE stale-cache test.
 *
 * The residual race we're trying to hit:
 *
 *   Reader (filemap_read)              Attribute update
 *   ─────────────────────              ────────────────
 *   check INVALID_DATA → clear
 *   invalidate_inode_pages2()
 *   generic_file_read_iter()
 *     filemap_get_read_batch()
 *       → gets folio (zero-fill
 *         beyond old EOF, marked
 *         uptodate)
 *                                      set INVALID_DATA
 *                                      i_size_write(new_size)
 *     i_size_read() → new_size
 *     → copies stale zeroes from
 *       folio up to new_size
 *
 * Strategy: each reader thread reads the file sequentially one byte at
 * a time.  When read() returns 0 (EOF), it retries — the file is still
 * growing on the server.  Any 0x00 byte is stale page-cache data.
 * The stat-hammer threads force GETATTR RPCs that update i_size
 * between filemap_get_read_batch and i_size_read.
 */

#define FILL_BYTE    0xAA
#define READ_THREADS 16
#define STAT_THREADS 4

static volatile sig_atomic_t stop;
static const char *filepath;

static void handle_sig(int sig) { (void)sig; stop = 1; }

struct reader_arg {
	int id;
	int fd;
};

static void *stat_thread(void *arg)
{
	(void)arg;
	struct stat st;
	while (!stop)
		stat(filepath, &st);
	return NULL;
}

static void *read_thread(void *arg)
{
	struct reader_arg *ra = arg;
	int id = ra->id;
	int fd = ra->fd;
	unsigned char byte;
	unsigned long checks = 0, hits = 0;
	off_t offset = 0;

	while (!stop) {
		ssize_t n = read(fd, &byte, 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("read");
			break;
		}
		if (n == 0) {
			/* EOF — file hasn't grown yet, retry */
			continue;
		}

		checks++;

		if (byte == 0x00) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			fprintf(stderr,
				"[%ld.%03ld] reader %d: STALE ZERO at "
				"offset %lld\n",
				(long)now.tv_sec,
				now.tv_nsec / 1000000,
				id,
				(long long)offset);
			hits++;
		}

		offset++;
	}

	fprintf(stderr, "reader %d: %lu checks, %lu stale hits\n",
		id, checks, hits);

	close(fd);
	return (void *)(unsigned long)hits;
}

int main(int argc, char *argv[])
{
	int duration = 60;
	pthread_t stat_tids[STAT_THREADS];
	pthread_t read_tids[READ_THREADS];
	struct reader_arg rargs[READ_THREADS];
	unsigned long total_hits = 0;
	int base_fd;

	setbuf(stderr, NULL);

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: %s FILE [DURATION_SECS]\n", argv[0]);
		return 1;
	}
	filepath = argv[1];
	if (argc == 3)
		duration = atoi(argv[2]);

	/* Open the file once in main — NFS v4 OPEN can be slow with noac */
	base_fd = open(filepath, O_RDONLY);
	if (base_fd < 0) {
		fprintf(stderr, "open(%s): %s\n", filepath, strerror(errno));
		return 1;
	}
	fprintf(stderr, "opened %s (fd %d), file ready\n", filepath, base_fd);

	signal(SIGINT, handle_sig);
	signal(SIGTERM, handle_sig);

	for (int i = 0; i < STAT_THREADS; i++)
		pthread_create(&stat_tids[i], NULL, stat_thread, NULL);
	for (int i = 0; i < READ_THREADS; i++) {
		rargs[i].id = i;
		rargs[i].fd = dup(base_fd);
		pthread_create(&read_tids[i], NULL, read_thread, &rargs[i]);
	}
	close(base_fd);

	fprintf(stderr, "running for %d seconds (%d readers, %d stat hammers)...\n",
		duration, READ_THREADS, STAT_THREADS);
	sleep(duration);
	stop = 1;

	for (int i = 0; i < STAT_THREADS; i++)
		pthread_join(stat_tids[i], NULL);
	for (int i = 0; i < READ_THREADS; i++) {
		void *ret;
		pthread_join(read_tids[i], &ret);
		total_hits += (unsigned long)ret;
	}

	if (total_hits) {
		fprintf(stderr, "\nBug reproduced: %lu stale-zero hit(s)\n",
			total_hits);
		return 1;
	}
	fprintf(stderr, "\nNo stale data detected.\n");
	return 0;
}
