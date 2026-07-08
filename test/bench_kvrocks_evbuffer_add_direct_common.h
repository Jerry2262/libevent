/*
 * Shared direct evbuffer_add() fast-path benchmark implementation.
 *
 * Include this file from a tiny wrapper that defines BENCH_NAME and optionally
 * BENCH_NO_CALLBACK or BENCH_INLINE_COPY128.
 */

#include "util-internal.h"
#include "event2/buffer.h"
#include "event2/buffer_compat.h"
#include "evbuffer-internal.h"

#include <sys/types.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif
#include <stdlib.h>
#include <stdint.h>
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

#if defined(__GNUC__) || defined(__clang__)
#define BENCH_NOINLINE __attribute__((noinline))
#else
#define BENCH_NOINLINE
#endif

static volatile bench_u64 direct_sink;

static long
elapsed_usec(const struct timeval *start, const struct timeval *end)
{
	return (long)((end->tv_sec - start->tv_sec) * 1000000L +
	    (end->tv_usec - start->tv_usec));
}

static void
usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-n add_calls] [-v value_size] "
	    "[-d seconds]\n", prog);
	exit(1);
}

#ifndef BENCH_NO_CALLBACK
static BENCH_NOINLINE void
bench_invoke_callbacks(struct evbuffer *buf)
{
	if (LIST_EMPTY(&buf->callbacks)) {
		buf->n_add_for_cb = buf->n_del_for_cb = 0;
		return;
	}
	abort();
}
#endif

static inline void
bench_copy(void *dst, const void *src, size_t len)
{
#ifdef BENCH_INLINE_COPY128
	if (len == 128) {
		uint64_t *d = dst;
		const uint64_t *s = src;

		d[0] = s[0];
		d[1] = s[1];
		d[2] = s[2];
		d[3] = s[3];
		d[4] = s[4];
		d[5] = s[5];
		d[6] = s[6];
		d[7] = s[7];
		d[8] = s[8];
		d[9] = s[9];
		d[10] = s[10];
		d[11] = s[11];
		d[12] = s[12];
		d[13] = s[13];
		d[14] = s[14];
		d[15] = s[15];
		return;
	}
#endif
	memcpy(dst, src, len);
}

static BENCH_NOINLINE int
bench_evbuffer_add_direct(struct evbuffer *buf, const void *data_in,
    size_t datlen)
{
	struct evbuffer_chain *chain;
	const unsigned char *data = data_in;
	size_t remain;

	if (buf->freeze_end)
		return -1;
	if (datlen > EV_SIZE_MAX - buf->total_len)
		return -1;

	if (*buf->last_with_datap == NULL)
		chain = buf->last;
	else
		chain = *buf->last_with_datap;
	if (chain == NULL)
		return -1;

	if ((chain->flags & EVBUFFER_IMMUTABLE) == 0) {
		remain = chain->buffer_len - (size_t)chain->misalign -
		    chain->off;
		if (remain >= datlen) {
			bench_copy(chain->buffer + chain->misalign + chain->off,
			    data, datlen);
			chain->off += datlen;
			buf->total_len += datlen;
			buf->n_add_for_cb += datlen;
#ifndef BENCH_NO_CALLBACK
			bench_invoke_callbacks(buf);
#endif
			return 0;
		}
	}

	return -1;
}

static int
prepare_batch(struct evbuffer *buf, size_t batch_bytes)
{
	if (evbuffer_expand(buf, batch_bytes) < 0)
		return -1;
	return 0;
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
		struct timeval start, end;
		size_t batch_bytes;

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
		batch_bytes = (size_t)todo * (size_t)value_size;
		if (prepare_batch(buf, batch_bytes) < 0) {
			fprintf(stderr, "prepare_batch failed\n");
			evbuffer_free(buf);
			free(value);
			return 1;
		}

		evutil_gettimeofday(&start, NULL);
		for (i = 0; i < todo; ++i) {
			if (bench_evbuffer_add_direct(buf, value,
				(size_t)value_size) < 0) {
				fprintf(stderr, "bench_evbuffer_add_direct failed\n");
				evbuffer_free(buf);
				free(value);
				return 1;
			}
			bytes += (bench_u64)value_size;
		}
		evutil_gettimeofday(&end, NULL);
		total_usec += elapsed_usec(&start, &end);
		direct_sink += (bench_u64)evbuffer_get_length(buf);

		evbuffer_free(buf);

		if (target_usec > 0 && total_usec >= target_usec)
			break;
	}

	printf("bench=%s ops=%ld bytes=" U64_FMT
	    " usec=%ld ops_sec=%.2f mb_sec=%.2f sink=" U64_FMT "\n",
	    BENCH_NAME, done, bytes, total_usec,
	    total_usec ? (double)done * 1000000.0 / total_usec : 0.0,
	    total_usec ? (double)bytes / (1024.0 * 1024.0) * 1000000.0 /
		total_usec : 0.0, (bench_u64)direct_sink);

	free(value);
	return 0;
}
