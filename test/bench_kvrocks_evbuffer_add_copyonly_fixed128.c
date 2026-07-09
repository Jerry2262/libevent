/*
 * Benchmark fixed-size memcpy payloads without evbuffer bookkeeping.
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

static volatile unsigned int copy_sink;

static long
elapsed_usec(const struct timeval *start, const struct timeval *end)
{
	return (long)((end->tv_sec - start->tv_sec) * 1000000L +
	    (end->tv_usec - start->tv_usec));
}

static void
usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-n copy_calls] [-v value_size] "
	    "[-d seconds]\n", prog);
	exit(1);
}

static void
consume_buffer(const char *dst, size_t len)
{
	const volatile unsigned char *p = (const volatile unsigned char *)dst;
	unsigned int acc = copy_sink;

	if (len != 0) {
		acc += p[0];
		acc += p[len / 2];
		acc += p[len - 1];
	}
	copy_sink = acc;
}

int
main(int argc, char **argv)
{
	char *value;
	char *dst;
	size_t max_batch_bytes;
	long ops = 0;
	long value_size = DEFAULT_VALUE_SIZE;
	long duration = DEFAULT_DURATION;
	long target_usec;
	long i, done, todo;
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

	max_batch_bytes = (size_t)BATCH_OPS * (size_t)value_size;
	dst = malloc(max_batch_bytes);
	if (dst == NULL) {
		perror("malloc");
		free(value);
		return 1;
	}

	for (done = 0; ops == 0 || done < ops; done += todo) {
		struct timeval start, end;
		size_t off = 0;

		if (ops == 0)
			todo = BATCH_OPS;
		else {
			todo = ops - done;
			if (todo > BATCH_OPS)
				todo = BATCH_OPS;
		}

		evutil_gettimeofday(&start, NULL);
		for (i = 0; i < todo; ++i) {
			memcpy(dst + off, value, (size_t)value_size);
			off += (size_t)value_size;
		}
		evutil_gettimeofday(&end, NULL);
		total_usec += elapsed_usec(&start, &end);
		consume_buffer(dst, off);

		if (target_usec > 0 && total_usec >= target_usec)
			break;
	}

	printf("bench=evbuffer_add_copyonly_fixed128 ns_per_op=%.2f\n",
	    done ? (double)total_usec * 1000.0 / done : 0.0);

	free(dst);
	free(value);
	return 0;
}
