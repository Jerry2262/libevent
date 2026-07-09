/*
 * Microbenchmark the preallocated evbuffer_add() hot path using libevent's
 * real struct evbuffer and struct evbuffer_chain layout.
 *
 * This is not an evbuffer API benchmark.  It tests whether the real evbuffer
 * field layout and pointer chain are enough to reproduce the gap between
 * evbuffer_add() and the simplified shortpath microbenchmark.
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
#include "event2/buffer_compat.h"
#include "event2/util.h"
#include "evthread-internal.h"
#include "evbuffer-internal.h"

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
#ifndef BENCH_NAME
#define BENCH_NAME "evbuffer_add_fixed128_shortpath_realstruct"
#endif

static volatile unsigned int realstruct_sink;

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
	unsigned int acc = realstruct_sink;

	if (len != 0) {
		acc += p[0];
		acc += p[len / 2];
		acc += p[len - 1];
	}
	realstruct_sink = acc;
}

static BENCH_NOINLINE int
realstruct_add(struct evbuffer *buf, const void *data_in, size_t datlen)
{
	struct evbuffer_chain *chain;
	const unsigned char *data = data_in;
	size_t remain;
	int result = -1;

	EVBUFFER_LOCK(buf);

	if (buf->freeze_end)
		goto done;
	if (datlen > EV_SIZE_MAX - buf->total_len)
		goto done;

	if (*buf->last_with_datap == NULL)
		chain = buf->last;
	else
		chain = *buf->last_with_datap;

	if (chain == NULL)
		goto done;

	if ((chain->flags & EVBUFFER_IMMUTABLE) != 0)
		goto done;

	remain = chain->buffer_len - (size_t)chain->misalign - chain->off;
	if (remain < datlen)
		goto done;

	memcpy(chain->buffer + chain->misalign + chain->off, data, datlen);
	chain->off += datlen;
	buf->total_len += datlen;
	buf->n_add_for_cb += datlen;

#ifdef BENCH_USE_REAL_INVOKE
	evbuffer_invoke_callbacks_(buf);
#else
	if (LIST_EMPTY(&buf->callbacks))
		buf->n_add_for_cb = buf->n_del_for_cb = 0;
#endif

	result = 0;
done:
	EVBUFFER_UNLOCK(buf);
	return result;
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
		struct evbuffer_chain chain;
		struct evbuffer buf;

		if (ops == 0)
			todo = BATCH_OPS;
		else {
			todo = ops - done;
			if (todo > BATCH_OPS)
				todo = BATCH_OPS;
		}

		memset(&buf, 0, sizeof(buf));
		memset(&chain, 0, sizeof(chain));
		LIST_INIT(&buf.callbacks);
		buf.first = &chain;
		buf.last = &chain;
		buf.last_with_datap = &buf.first;
		buf.refcnt = 1;

		chain.buffer = storage;
		chain.buffer_len = (size_t)todo * (size_t)value_size;
		chain.refcnt = 1;

		evutil_gettimeofday(&start, NULL);
		for (i = 0; i < todo; ++i) {
			if (realstruct_add(&buf, value, (size_t)value_size) < 0) {
				fprintf(stderr, "realstruct_add failed\n");
				free(storage);
				free(value);
				return 1;
			}
		}
		evutil_gettimeofday(&end, NULL);
		total_usec += elapsed_usec(&start, &end);
		consume_buffer(storage, chain.off);

		if (target_usec > 0 && total_usec >= target_usec)
			break;
	}

	printf("bench=%s ns_per_op=%.2f\n", BENCH_NAME,
	    done ? (double)total_usec * 1000.0 / done : 0.0);

	free(storage);
	free(value);
	return 0;
}
