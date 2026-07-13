/*
 * Benchmark evbuffer_drain() with Kvrocks-like RESP payload consumption.
 *
 * Linux perf profiling mode prepares all chains before perf starts sampling.
 * It preallocates the complete -n workload, so choose -n with available memory
 * in mind.  The duration limit is disabled in this mode.
 *
 * Terminal 1:
 *   ./build/bin/bench_kvrocks_evbuffer_drain -p -n 1000000
 *   # Wait for: profile_ready pid=<pid>
 *
 * Terminal 2:
 *   perf record -g -p <pid>
 *
 * After perf reports that it is attached, resume the benchmark in terminal 1:
 *   kill -CONT <pid>
 *
 * perf exits when the benchmark exits.  Inspect the result with perf report or
 * perf annotate evbuffer_drain.
 */

#include <sys/types.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif
#ifdef __linux__
#include <signal.h>
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
	fprintf(stderr,
	    "Usage: %s [-n evbuffer_drain_calls] [-d seconds] [-p]\n",
	    prog);
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

#ifdef __linux__
static int
run_profile(struct evbuffer *buf, long ops, long *total_usec)
{
	struct timeval start, end;
	long i;

	fprintf(stderr, "profile_ready pid=%ld\n", (long)getpid());
	fflush(stderr);
	if (raise(SIGSTOP) != 0) {
		perror("raise(SIGSTOP)");
		return -1;
	}

	evutil_gettimeofday(&start, NULL);
	for (i = 0; i < ops; ++i) {
		size_t n = drain_len_for_op(i);
		if (evbuffer_drain(buf, n) < 0) {
			fprintf(stderr, "evbuffer_drain failed\n");
			return -1;
		}
	}
	evutil_gettimeofday(&end, NULL);
	*total_usec = elapsed_usec(&start, &end);
	return 0;
}
#endif

int
main(int argc, char **argv)
{
	char *chunk;
	long ops = 0;
	long duration = DEFAULT_DURATION;
	long target_usec;
	long done, todo, i;
	long total_usec = 0;
	int profile = 0;
	int c;

	while ((c = getopt(argc, argv, "n:d:ph")) != -1) {
		switch (c) {
		case 'n':
			ops = atol(optarg);
			break;
		case 'd':
			duration = atol(optarg);
			break;
		case 'p':
			profile = 1;
			break;
		case 'h':
		default:
			usage(argv[0]);
		}
	}

	if (ops < 0 || duration < 0)
		usage(argv[0]);
	if (profile) {
#ifdef __linux__
		if (ops <= 0)
			usage(argv[0]);
#else
		fprintf(stderr, "-p is supported only on Linux\n");
		return 1;
#endif
	} else if (ops == 0 && duration == 0) {
		usage(argv[0]);
	}
	target_usec = profile ? 0 : duration * 1000000L;

	chunk = malloc(CHUNK_SIZE);
	if (chunk == NULL) {
		perror("malloc");
		return 1;
	}
	memset(chunk, 'd', CHUNK_SIZE);

	if (profile) {
#ifdef __linux__
		struct evbuffer *buf = evbuffer_new();

		if (buf == NULL) {
			fprintf(stderr, "evbuffer_new failed\n");
			free(chunk);
			return 1;
		}
		if (prepare_buffer(buf, chunk, 0, ops) < 0) {
			fprintf(stderr, "evbuffer_add_reference failed\n");
			evbuffer_free(buf);
			free(chunk);
			return 1;
		}
		if (run_profile(buf, ops, &total_usec) < 0) {
			evbuffer_free(buf);
			free(chunk);
			return 1;
		}
		evbuffer_free(buf);
		done = ops;
#endif
	} else {
		for (done = 0; ops == 0 || done < ops; done += todo) {
			struct evbuffer *buf;
			struct timeval start, end;

			if (ops == 0)
				todo = BATCH_DRAINS;
			else {
				todo = ops - done;
				if (todo > BATCH_DRAINS)
					todo = BATCH_DRAINS;
			}

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
			}
			evutil_gettimeofday(&end, NULL);
			total_usec += elapsed_usec(&start, &end);

			evbuffer_free(buf);

			if (target_usec > 0 && total_usec >= target_usec)
				break;
		}
	}

	printf("bench=evbuffer_drain ns_per_op=%.2f\n",
	    done ? (double)total_usec * 1000.0 / done : 0.0);

	free(chunk);
	return 0;
}
