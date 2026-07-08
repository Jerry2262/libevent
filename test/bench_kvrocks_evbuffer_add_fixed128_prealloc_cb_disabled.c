/*
 * Benchmark evbuffer_add() with fixed 128-byte payloads after preallocating
 * each batch's destination chain and installing one disabled callback entry.
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

#define DEFAULT_DURATION 3
#define DEFAULT_VALUE_SIZE 128
#define BATCH_OPS 4096

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
disabled_cb(struct evbuffer *buffer, const struct evbuffer_cb_info *info,
    void *arg)
{
	(void)buffer;
	(void)info;
	(void)arg;
}

static void
usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-n evbuffer_add_calls] [-v value_size] "
	    "[-d seconds]\n", prog);
	exit(1);
}

int
main(int argc, char **argv)
{
	struct evbuffer *buf;
	char *value;
	long ops = 0;
	long value_size = DEFAULT_VALUE_SIZE;
	long duration = DEFAULT_DURATION;
	long target_usec;
	long i, done, todo;
	bench_u64 bytes = 0;
	long total_usec = 0;
	int c;

	while ((c = getopt(argc, argv, "n:v:d:h")) != -1) {
		switch (c) {
		case 'n':
			ops = atol(optarg);
			break;
		case 'v':
			value_size = atol(optarg);
			break;
		case 'd':
			duration = atol(optarg);
			break;
		case 'h':
		default:
			usage(argv[0]);
		}
	}

	if (value_size <= 0 || ops < 0 || duration < 0)
		usage(argv[0]);
	if (ops == 0 && duration == 0)
		usage(argv[0]);
	target_usec = duration * 1000000L;

	value = malloc((size_t)value_size);
	if (value == NULL) {
		perror("malloc");
		return 1;
	}
	memset(value, 'x', (size_t)value_size);

	for (done = 0; ops == 0 || done < ops; done += todo) {
		struct evbuffer_cb_entry *cb;
		struct timeval start, end;
		size_t reserve;

		if (ops == 0)
			todo = BATCH_OPS;
		else {
			todo = ops - done;
			if (todo > BATCH_OPS)
				todo = BATCH_OPS;
		}

		buf = evbuffer_new();
		if (buf == NULL) {
			fprintf(stderr, "evbuffer_new failed\n");
			free(value);
			return 1;
		}

		reserve = (size_t)todo * (size_t)value_size;
		if (evbuffer_expand(buf, reserve) < 0) {
			fprintf(stderr, "evbuffer_expand failed\n");
			evbuffer_free(buf);
			free(value);
			return 1;
		}
		cb = evbuffer_add_cb(buf, disabled_cb, NULL);
		if (cb == NULL) {
			fprintf(stderr, "evbuffer_add_cb failed\n");
			evbuffer_free(buf);
			free(value);
			return 1;
		}
		evbuffer_cb_clear_flags(buf, cb, EVBUFFER_CB_ENABLED);

		evutil_gettimeofday(&start, NULL);
		for (i = 0; i < todo; ++i) {
			if (evbuffer_add(buf, value, (size_t)value_size) < 0) {
				fprintf(stderr, "evbuffer_add failed\n");
				evbuffer_free(buf);
				free(value);
				return 1;
			}
			bytes += (bench_u64)value_size;
		}
		evutil_gettimeofday(&end, NULL);
		total_usec += elapsed_usec(&start, &end);

		evbuffer_free(buf);

		if (target_usec > 0 && total_usec >= target_usec)
			break;
	}

	printf("bench=evbuffer_add_fixed128_prealloc_cb_disabled ops=%ld "
	    "bytes=" U64_FMT " usec=%ld ops_sec=%.2f mb_sec=%.2f\n",
	    done, bytes, total_usec,
	    total_usec ? (double)done * 1000000.0 / total_usec : 0.0,
	    total_usec ? (double)bytes / (1024.0 * 1024.0) * 1000000.0 /
		total_usec : 0.0);

	free(value);
	return 0;
}
