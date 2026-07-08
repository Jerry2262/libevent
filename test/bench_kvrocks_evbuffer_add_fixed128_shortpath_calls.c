/*
 * Microbenchmark the hot mutable-chain path that remains after an
 * evbuffer_add() caller has preallocated enough room for fixed-size writes,
 * with memcpy and empty-callback handling forced through noinline helpers.
 *
 * This is not an evbuffer API benchmark.  It tests whether out-of-line helper
 * calls are enough to reproduce the gap between evbuffer_add() and the local
 * shortpath microbenchmark.
 */

#include <sys/types.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "event2/util.h"

#ifdef _WIN32
#include <getopt.h>
#endif

#if defined(_MSC_VER)
#define BENCH_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define BENCH_NOINLINE __attribute__((noinline))
#else
#define BENCH_NOINLINE
#endif

#define DEFAULT_DURATION 3
#define DEFAULT_VALUE_SIZE 128
#define BATCH_OPS 4096
#define SHORTPATH_IMMUTABLE 0x0008

#ifdef _WIN32
#define U64_FMT "%I64u"
typedef unsigned __int64 bench_u64;
#else
#define U64_FMT "%llu"
typedef unsigned long long bench_u64;
#endif

struct short_chain {
	unsigned char *buffer;
	size_t buffer_len;
	size_t misalign;
	size_t off;
	unsigned flags;
};

struct short_buffer {
	struct short_chain *last_with_data;
	size_t total_len;
	size_t n_add_for_cb;
	size_t n_del_for_cb;
	int freeze_end;
	int callbacks_empty;
};

static volatile unsigned int shortpath_sink;

static long
elapsed_usec(const struct timeval *start, const struct timeval *end)
{
	return (long)((end->tv_sec - start->tv_sec) * 1000000L +
	    (end->tv_usec - start->tv_usec));
}

static void
consume_buffer(const unsigned char *dst, size_t len)
{
	const volatile unsigned char *p = (const volatile unsigned char *)dst;
	unsigned int acc = shortpath_sink;

	if (len != 0) {
		acc += p[0];
		acc += p[len / 2];
		acc += p[len - 1];
	}
	shortpath_sink = acc;
}

static BENCH_NOINLINE void
shortpath_copy(unsigned char *dst, const unsigned char *src, size_t len)
{
	memcpy(dst, src, len);
}

static BENCH_NOINLINE void
shortpath_invoke_empty_callbacks(struct short_buffer *buf)
{
	if (buf->callbacks_empty)
		buf->n_add_for_cb = buf->n_del_for_cb = 0;
}

static BENCH_NOINLINE int
shortpath_add(struct short_buffer *buf, const void *data_in, size_t datlen)
{
	struct short_chain *chain;
	const unsigned char *data = data_in;
	size_t remain;

	if (buf->freeze_end)
		return -1;
	if (datlen > SIZE_MAX - buf->total_len)
		return -1;

	chain = buf->last_with_data;
	if ((chain->flags & SHORTPATH_IMMUTABLE) != 0)
		return -1;

	remain = chain->buffer_len - chain->misalign - chain->off;
	if (remain < datlen)
		return -1;

	shortpath_copy(chain->buffer + chain->misalign + chain->off,
	    data, datlen);
	chain->off += datlen;
	buf->total_len += datlen;
	buf->n_add_for_cb += datlen;
	shortpath_invoke_empty_callbacks(buf);

	return 0;
}

static void
usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-n add_calls] [-v value_size] "
	    "[-d seconds]\n", prog);
	exit(1);
}

int
main(int argc, char **argv)
{
	char *value;
	unsigned char *storage;
	size_t max_batch_bytes;
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

	max_batch_bytes = (size_t)BATCH_OPS * (size_t)value_size;
	storage = malloc(max_batch_bytes);
	if (storage == NULL) {
		perror("malloc");
		free(value);
		return 1;
	}

	for (done = 0; ops == 0 || done < ops; done += todo) {
		struct timeval start, end;
		struct short_chain chain;
		struct short_buffer buf;

		if (ops == 0)
			todo = BATCH_OPS;
		else {
			todo = ops - done;
			if (todo > BATCH_OPS)
				todo = BATCH_OPS;
		}

		chain.buffer = storage;
		chain.buffer_len = (size_t)todo * (size_t)value_size;
		chain.misalign = 0;
		chain.off = 0;
		chain.flags = 0;

		buf.last_with_data = &chain;
		buf.total_len = 0;
		buf.n_add_for_cb = 0;
		buf.n_del_for_cb = 0;
		buf.freeze_end = 0;
		buf.callbacks_empty = 1;

		evutil_gettimeofday(&start, NULL);
		for (i = 0; i < todo; ++i) {
			if (shortpath_add(&buf, value, (size_t)value_size) < 0) {
				fprintf(stderr, "shortpath_add failed\n");
				free(storage);
				free(value);
				return 1;
			}
			bytes += (bench_u64)value_size;
		}
		evutil_gettimeofday(&end, NULL);
		total_usec += elapsed_usec(&start, &end);
		consume_buffer(storage, chain.off);

		if (target_usec > 0 && total_usec >= target_usec)
			break;
	}

	printf("bench=evbuffer_add_fixed128_shortpath_calls ops=%ld bytes="
	    U64_FMT " usec=%ld ops_sec=%.2f mb_sec=%.2f sink=%u\n",
	    done, bytes, total_usec,
	    total_usec ? (double)done * 1000000.0 / total_usec : 0.0,
	    total_usec ? (double)bytes / (1024.0 * 1024.0) * 1000000.0 /
		total_usec : 0.0, shortpath_sink);

	free(storage);
	free(value);
	return 0;
}
