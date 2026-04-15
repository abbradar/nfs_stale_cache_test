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
 * Strategy: read only the page straddling EOF so we exercise exactly
 * the folio that contains the stale zero-fill.  The stat-hammer
 * threads force GETATTR RPCs that update i_size between our
 * filemap_get_read_batch and i_size_read.
 */

#define FILL_BYTE    0xAA
#define READ_THREADS 16
#define STAT_THREADS 4
#define PAGE_SIZE    4096

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
	unsigned char buf[PAGE_SIZE];
	unsigned long checks = 0, hits = 0;

	while (!stop) {
		struct stat st;

		if (fstat(fd, &st) < 0)
			continue;

		off_t sz = st.st_size;
		if (sz <= 0)
			continue;

		/*
		 * Read only the page straddling EOF.  This is the page
		 * that contains zero-fill beyond the old EOF — exactly
		 * the data that becomes stale when i_size grows.
		 */
		off_t page_start = (sz - 1) & ~((off_t)PAGE_SIZE - 1);
		ssize_t n = pread(fd, buf, PAGE_SIZE, page_start);
		if (n <= 0)
			continue;

		checks++;

		/*
		 * Only check bytes within [page_start, sz).  Bytes
		 * beyond sz are legitimately zero (beyond EOF).
		 * But bytes in [page_start, sz) must all be FILL_BYTE.
		 */
		off_t valid = sz - page_start;
		if (valid > n)
			valid = n;

		for (ssize_t i = 0; i < valid; i++) {
			if (buf[i] == 0x00) {
				struct timespec now;
				clock_gettime(CLOCK_MONOTONIC, &now);
				fprintf(stderr,
					"[%ld.%03ld] reader %d: STALE ZERO at "
					"file offset %zd (page +%zd) "
					"sz=%lld n=%zd\n",
					(long)now.tv_sec,
					now.tv_nsec / 1000000,
					id,
					(ssize_t)(page_start + i), i,
					(long long)sz, n);
				hits++;
				break;  /* one hit per read is enough */
			}
		}

		/*
		 * Yield briefly to give the scheduler a chance to
		 * interleave an attribute update between our
		 * filemap_get_read_batch and i_size_read in the
		 * next iteration's pread.
		 */
		usleep(1);
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
