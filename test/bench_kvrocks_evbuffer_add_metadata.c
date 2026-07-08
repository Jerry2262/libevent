/*
 * Benchmark the evbuffer_add() fast-path metadata operations without copying.
 */

#include <sys/types.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "event2/util.h"

#ifdef _WIN32
#include <getopt.h>
#endif

#define DEFAULT_DURATION 3
#define DEFAULT_VALUE_SIZE 128
#define BATCH_OPS 4096
#define BENCH_EVBUFFER_IMMUTABLE 0x0008

#ifdef _WIN32
#define U64_FMT "%I64u"
typedef unsigned __int64 bench_u64;
#else
#define U64_FMT "%llu"
typedef unsigned long long bench_u64;
#endif

struct bench_chain {
	struct bench_chain *next;
	size_t buffer_len;
	size_t misalign;
	size_t off;
	unsigned flags;
	int refcnt;
	unsigned char *buffer;
};

struct bench_buffer {
	struct bench_chain *first;
	struct bench_chain *last;
	struct bench_chain **last_with_datap;
	size_t total_len;
	size_t n_add_for_cb;
	size_t n_del_for_cb;
	unsigned freeze_end;
	void *callbacks;
};

static volatile bench_u64 metadata_sink;

#if defined(__GNUC__) || defined(__clang__)
#define BENCH_NOINLINE __attribute__((noinline))
#else
#define BENCH_NOINLINE
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
	fprintf(stderr, "Usage: %s [-n metadata_add_calls] [-v value_size] "
	    "[-d seconds]\n", prog);
	exit(1);
}

static BENCH_NOINLINE int
metadata_add(struct bench_buffer *buf, size_t datlen)
{
	struct bench_chain *chain;
	size_t remain;

	if (buf->freeze_end)
		return -1;
	if (datlen > SIZE_MAX - buf->total_len)
		return -1;

	if (*buf->last_with_datap == NULL)
		chain = buf->last;
	else
		chain = *buf->last_with_datap;
	if (chain == NULL)
		return -1;

	if ((chain->flags & BENCH_EVBUFFER_IMMUTABLE) == 0) {
		remain = chain->buffer_len - chain->misalign - chain->off;
		if (remain >= datlen) {
			chain->off += datlen;
			buf->total_len += datlen;
			buf->n_add_for_cb += datlen;
			goto out;
		}
	}

	return -1;

out:
	if (buf->callbacks == NULL)
		buf->n_add_for_cb = buf->n_del_for_cb = 0;
	return 0;
}

static int
init_batch(struct bench_buffer *buf, struct bench_chain *chain,
    size_t batch_bytes)
{
	if (batch_bytes > SIZE_MAX - 1024)
		return -1;

	chain->next = NULL;
	chain->buffer_len = batch_bytes + 1024;
	chain->misalign = 0;
	chain->off = 0;
	chain->flags = 0;
	chain->refcnt = 1;
	chain->buffer = NULL;

	buf->first = chain;
	buf->last = chain;
	buf->last_with_datap = &buf->first;
	buf->total_len = 0;
	buf->n_add_for_cb = 0;
	buf->n_del_for_cb = 0;
	buf->freeze_end = 0;
	buf->callbacks = NULL;
	return 0;
}

int
main(int argc, char **argv)
{
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

	for (done = 0; ops == 0 || done < ops; done += todo) {
		struct bench_buffer buf;
		struct bench_chain chain;
		struct timeval start, end;
		size_t batch_bytes;

		if (ops == 0)
			todo = BATCH_OPS;
		else {
			todo = ops - done;
			if (todo > BATCH_OPS)
				todo = BATCH_OPS;
		}

		batch_bytes = (size_t)todo * (size_t)value_size;
		if (init_batch(&buf, &chain, batch_bytes) < 0) {
			fprintf(stderr, "init_batch failed\n");
			return 1;
		}

		evutil_gettimeofday(&start, NULL);
		for (i = 0; i < todo; ++i) {
			if (metadata_add(&buf, (size_t)value_size) < 0) {
				fprintf(stderr, "metadata_add failed\n");
				return 1;
			}
			bytes += (bench_u64)value_size;
		}
		evutil_gettimeofday(&end, NULL);
		total_usec += elapsed_usec(&start, &end);
		metadata_sink += (bench_u64)(buf.total_len + chain.off +
		    buf.n_add_for_cb + buf.n_del_for_cb);

		if (target_usec > 0 && total_usec >= target_usec)
			break;
	}

	printf("bench=evbuffer_add_metadata ops=%ld bytes=" U64_FMT
	    " usec=%ld ops_sec=%.2f mb_sec=%.2f sink=" U64_FMT "\n",
	    done, bytes, total_usec,
	    total_usec ? (double)done * 1000000.0 / total_usec : 0.0,
	    total_usec ? (double)bytes / (1024.0 * 1024.0) * 1000000.0 /
		total_usec : 0.0, (bench_u64)metadata_sink);

	return 0;
}
