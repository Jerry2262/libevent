/*
 * Benchmark evbuffer_add() with Kvrocks-like fragments after preallocating
 * each batch's destination chain.
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
usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-n evbuffer_add_calls] [-v value_size] "
	    "[-d seconds]\n", prog);
	exit(1);
}

static size_t
len_for_slot(long slot, size_t value_size, size_t header_len)
{
	slot %= 10;
	if (slot < 3)
		return 5;
	if (slot < 5)
		return 5;
	if (slot < 7)
		return 4;
	if (slot == 7)
		return header_len;
	if (slot == 8)
		return value_size;
	return 2;
}

static size_t
batch_bytes(long first, long count, size_t value_size, size_t header_len)
{
	size_t total = 0;
	long i;

	for (i = 0; i < count; ++i)
		total += len_for_slot(first + i, value_size, header_len);

	return total;
}

int
main(int argc, char **argv)
{
	struct evbuffer *buf;
	char *value;
	char header[64];
	size_t header_len;
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
	snprintf(header, sizeof(header), "$%ld\r\n", value_size);
	header_len = strlen(header);

	for (done = 0; ops == 0 || done < ops; done += todo) {
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
		reserve = batch_bytes(done, todo, (size_t)value_size, header_len);
		if (evbuffer_expand(buf, reserve) < 0) {
			fprintf(stderr, "evbuffer_expand failed\n");
			evbuffer_free(buf);
			free(value);
			return 1;
		}

		evutil_gettimeofday(&start, NULL);
		for (i = 0; i < todo; ++i) {
			const char *data;
			size_t len;
			long slot = (done + i) % 10;

			if (slot < 3) {
				data = "+OK\r\n";
				len = 5;
			} else if (slot < 5) {
				data = "$-1\r\n";
				len = 5;
			} else if (slot < 7) {
				data = ":1\r\n";
				len = 4;
			} else if (slot == 7) {
				data = header;
				len = header_len;
			} else if (slot == 8) {
				data = value;
				len = (size_t)value_size;
			} else {
				data = "\r\n";
				len = 2;
			}

			if (evbuffer_add(buf, data, len) < 0) {
				fprintf(stderr, "evbuffer_add failed\n");
				evbuffer_free(buf);
				free(value);
				return 1;
			}
			bytes += (bench_u64)len;
		}
		evutil_gettimeofday(&end, NULL);
		total_usec += elapsed_usec(&start, &end);

		evbuffer_free(buf);

		if (target_usec > 0 && total_usec >= target_usec)
			break;
	}

	printf("bench=evbuffer_add_prealloc ops=%ld bytes=" U64_FMT
	    " usec=%ld ops_sec=%.2f mb_sec=%.2f\n",
	    done, bytes, total_usec,
	    total_usec ? (double)done * 1000000.0 / total_usec : 0.0,
	    total_usec ? (double)bytes / (1024.0 * 1024.0) * 1000000.0 /
		total_usec : 0.0);

	free(value);
	return 0;
}
