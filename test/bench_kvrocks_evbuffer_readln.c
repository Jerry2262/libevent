/*
 * Benchmark evbuffer_readln() with Kvrocks-like RESP CRLF lines.
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

#define DEFAULT_DURATION 1
#define BATCH_LINES 4096

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
	fprintf(stderr, "Usage: %s [-n evbuffer_readln_calls] [-d seconds]\n",
	    prog);
	exit(1);
}

static int
add_line_reference(struct evbuffer *buf, long idx)
{
	static const char *whole[] = {
		"*2\r\n", "$3\r\n", "GET\r\n", "$16\r\n",
		"kvrocks:key:0001\r\n", "*3\r\n", "SET\r\n", "$128\r\n"
	};
	static const char *split_a[] = {
		"+OK\r", "$-1\r", ":1000\r", "-ERR busy\r"
	};
	static const char *split_b = "\n";
	const char *s;

	if ((idx % 8) == 7) {
		s = split_a[(idx / 8) % 4];
		if (evbuffer_add_reference(buf, s, strlen(s), NULL, NULL) < 0)
			return -1;
		return evbuffer_add_reference(buf, split_b, 1, NULL, NULL);
	}

	s = whole[idx % (long)(sizeof(whole) / sizeof(whole[0]))];
	return evbuffer_add_reference(buf, s, strlen(s), NULL, NULL);
}

int
main(int argc, char **argv)
{
	long ops = 0;
	long duration = DEFAULT_DURATION;
	long target_usec;
	long done, todo, i;
	bench_u64 bytes = 0;
	long total_usec = 0;
	int c;

	while ((c = getopt(argc, argv, "n:d:h")) != -1) {
		switch (c) {
		case 'n':
			ops = atol(optarg);
			break;
		case 'd':
			duration = atol(optarg);
			break;
		case 'h':
		default:
			usage(argv[0]);
		}
	}

	if (ops < 0 || duration < 0)
		usage(argv[0]);
	if (ops == 0 && duration == 0)
		usage(argv[0]);
	target_usec = duration * 1000000L;

	for (done = 0; ops == 0 || done < ops; done += todo) {
		struct evbuffer *buf;
		struct timeval start, end;

		if (ops == 0)
			todo = BATCH_LINES;
		else {
			todo = ops - done;
			if (todo > BATCH_LINES)
				todo = BATCH_LINES;
		}

		buf = evbuffer_new();
		if (buf == NULL) {
			fprintf(stderr, "evbuffer_new failed\n");
			return 1;
		}

		for (i = 0; i < todo; ++i) {
			if (add_line_reference(buf, done + i) < 0) {
				fprintf(stderr, "evbuffer_add_reference failed\n");
				evbuffer_free(buf);
				return 1;
			}
		}

		evutil_gettimeofday(&start, NULL);
		for (i = 0; i < todo; ++i) {
			size_t n_read = 0;
			char *line = evbuffer_readln(buf, &n_read,
			    EVBUFFER_EOL_CRLF_STRICT);
			if (line == NULL) {
				fprintf(stderr, "evbuffer_readln failed\n");
				evbuffer_free(buf);
				return 1;
			}
			bytes += (bench_u64)n_read;
			free(line);
		}
		evutil_gettimeofday(&end, NULL);
		total_usec += elapsed_usec(&start, &end);

		evbuffer_free(buf);

		if (target_usec > 0 && total_usec >= target_usec)
			break;
	}

	printf("bench=evbuffer_readln ops=%ld bytes=" U64_FMT
	    " usec=%ld ops_sec=%.2f mb_sec=%.2f\n",
	    done, bytes, total_usec,
	    total_usec ? (double)done * 1000000.0 / total_usec : 0.0,
	    total_usec ? (double)bytes / (1024.0 * 1024.0) * 1000000.0 /
		total_usec : 0.0);

	return 0;
}
