/*
 * Benchmark evbuffer_drain() with Kvrocks-like RESP payload consumption.
 */

#include <sys/types.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "event2/buffer.h"
#include "event2/util.h"

#ifdef _WIN32
#include <getopt.h>
#endif

#define DEFAULT_OPS 1000000
#define BATCH_DRAINS 4096
#define CHUNK_SIZE 4096

#ifdef _WIN32
#define U64_FMT "%I64u"
typedef unsigned __int64 bench_u64;
#else
#define U64_FMT "%llu"
typedef unsigned long long bench_u64;
#endif

static long
elapsed_usec(const struct timeval *start, const struct timeval *end)
{
	return (long)((end->tv_sec - start->tv_sec) * 1000000L +
	    (end->tv_usec - start->tv_usec));
}

static void
usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-n evbuffer_drain_calls]\n", prog);
	exit(1);
}

static size_t
drain_len_for_op(long idx)
{
	switch (idx % 12) {
	case 0:
	case 1:
	case 2:
	case 3:
		return 2;
	case 4:
	case 5:
		return 16;
	case 6:
	case 7:
		return 64;
	case 8:
	case 9:
		return 128;
	case 10:
		return 1024;
	default:
		return 16 * 1024;
	}
}

static int
prepare_buffer(struct evbuffer *buf, char *chunk, long first, long count)
{
	long i;

	for (i = 0; i < count; ++i) {
		size_t remain = drain_len_for_op(first + i);

		while (remain > 0) {
			size_t n = remain > CHUNK_SIZE ? CHUNK_SIZE : remain;
			if (evbuffer_add_reference(buf, chunk, n, NULL, NULL) < 0)
				return -1;
			remain -= n;
		}
	}

	return 0;
}

int
main(int argc, char **argv)
{
	char *chunk;
	long ops = DEFAULT_OPS;
	long done, todo, i;
	bench_u64 bytes = 0;
	long total_usec = 0;
	int c;

	while ((c = getopt(argc, argv, "n:h")) != -1) {
		switch (c) {
		case 'n':
			ops = atol(optarg);
			break;
		case 'h':
		default:
			usage(argv[0]);
		}
	}

	if (ops <= 0)
		usage(argv[0]);

	chunk = malloc(CHUNK_SIZE);
	if (chunk == NULL) {
		perror("malloc");
		return 1;
	}
	memset(chunk, 'd', CHUNK_SIZE);

	for (done = 0; done < ops; done += todo) {
		struct evbuffer *buf;
		struct timeval start, end;

		todo = ops - done;
		if (todo > BATCH_DRAINS)
			todo = BATCH_DRAINS;

		buf = evbuffer_new();
		if (buf == NULL) {
			fprintf(stderr, "evbuffer_new failed\n");
			free(chunk);
			return 1;
		}

		if (prepare_buffer(buf, chunk, done, todo) < 0) {
			fprintf(stderr, "evbuffer_add_reference failed\n");
			evbuffer_free(buf);
			free(chunk);
			return 1;
		}

		evutil_gettimeofday(&start, NULL);
		for (i = 0; i < todo; ++i) {
			size_t n = drain_len_for_op(done + i);
			if (evbuffer_drain(buf, n) < 0) {
				fprintf(stderr, "evbuffer_drain failed\n");
				evbuffer_free(buf);
				free(chunk);
				return 1;
			}
			bytes += (bench_u64)n;
		}
		evutil_gettimeofday(&end, NULL);
		total_usec += elapsed_usec(&start, &end);

		evbuffer_free(buf);
	}

	printf("bench=evbuffer_drain ops=%ld bytes=" U64_FMT
	    " usec=%ld ops_sec=%.2f mb_sec=%.2f\n",
	    ops, bytes, total_usec,
	    total_usec ? (double)ops * 1000000.0 / total_usec : 0.0,
	    total_usec ? (double)bytes / (1024.0 * 1024.0) * 1000000.0 /
		total_usec : 0.0);

	free(chunk);
	return 0;
}
