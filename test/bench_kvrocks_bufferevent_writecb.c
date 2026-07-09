/*
 * Benchmark the bufferevent socket write callback path with Kvrocks-like
 * RESP output blocks.
 */

#include "util-internal.h"

#include <sys/types.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "event2/event.h"
#include "event2/bufferevent.h"
#include "event2/buffer.h"
#include "event2/util.h"

#ifdef _WIN32
#include <getopt.h>
#define LOCAL_SOCKETPAIR_AF AF_INET
#else
#define LOCAL_SOCKETPAIR_AF AF_UNIX
#endif

#define DEFAULT_DURATION 3
#define DEFAULT_CONNS 64
#define DEFAULT_REPLY_SIZE 256

#ifdef _WIN32
#define U64_FMT "%I64u"
typedef unsigned __int64 bench_u64;
#else
#define U64_FMT "%llu"
typedef unsigned long long bench_u64;
#endif

struct conn {
	struct bufferevent *writer;
	struct bufferevent *reader;
};

static struct event_base *base;
static struct conn *conns;
static char *reply;
static long target_writes = 0;
static long queued_writes;
static long completed_writes;
static long n_conns = DEFAULT_CONNS;
static long reply_size = DEFAULT_REPLY_SIZE;
static long duration = DEFAULT_DURATION;
static bench_u64 bytes_written;
static bench_u64 bytes_read;

static long
elapsed_usec(const struct timeval *start, const struct timeval *end)
{
	return (long)((end->tv_sec - start->tv_sec) * 1000000L +
	    (end->tv_usec - start->tv_usec));
}

static void
usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-n write_callbacks] [-c conns] "
	    "[-v reply_size] [-d seconds]\n", prog);
	exit(1);
}

static int
queue_one(struct conn *c)
{
	if (target_writes > 0 && queued_writes >= target_writes)
		return 0;
	if (bufferevent_write(c->writer, reply, (size_t)reply_size) < 0)
		return -1;
	++queued_writes;
	bytes_written += (bench_u64)reply_size;
	return 0;
}

static void
writecb(struct bufferevent *bev, void *arg)
{
	struct conn *c = arg;

	++completed_writes;
	if (target_writes > 0 && completed_writes >= target_writes) {
		event_base_loopexit(base, NULL);
		return;
	}

	if (queue_one(c) < 0)
		event_base_loopexit(base, NULL);
}

static void
readcb(struct bufferevent *bev, void *arg)
{
	struct evbuffer *input = bufferevent_get_input(bev);
	size_t len = evbuffer_get_length(input);

	bytes_read += (bench_u64)len;
	evbuffer_drain(input, len);
}

static void
eventcb(struct bufferevent *bev, short what, void *arg)
{
	if (what & BEV_EVENT_ERROR)
		fprintf(stderr, "bufferevent error\n");
	event_base_loopexit(base, NULL);
}

static int
make_conn(struct conn *c)
{
	evutil_socket_t pair[2];

	if (evutil_socketpair(LOCAL_SOCKETPAIR_AF, SOCK_STREAM, 0, pair) < 0)
		return -1;

	if (evutil_make_socket_nonblocking(pair[0]) < 0 ||
	    evutil_make_socket_nonblocking(pair[1]) < 0) {
		evutil_closesocket(pair[0]);
		evutil_closesocket(pair[1]);
		return -1;
	}

	c->writer = bufferevent_socket_new(base, pair[0], BEV_OPT_CLOSE_ON_FREE);
	c->reader = bufferevent_socket_new(base, pair[1], BEV_OPT_CLOSE_ON_FREE);
	if (c->writer == NULL || c->reader == NULL) {
		if (c->writer)
			bufferevent_free(c->writer);
		else
			evutil_closesocket(pair[0]);
		if (c->reader)
			bufferevent_free(c->reader);
		else
			evutil_closesocket(pair[1]);
		return -1;
	}

	bufferevent_setcb(c->writer, NULL, writecb, eventcb, c);
	bufferevent_setcb(c->reader, readcb, NULL, eventcb, c);
	bufferevent_enable(c->writer, EV_WRITE);
	bufferevent_enable(c->reader, EV_READ);
	return 0;
}

static void
fill_reply(void)
{
	const char *prefix = "$";
	const char *suffix = "\r\nxxxxxxxxxxxxxxxx\r\n";
	size_t off = 0;
	size_t n;

	memset(reply, 'y', (size_t)reply_size);
	n = strlen(prefix);
	if (n > (size_t)reply_size)
		n = (size_t)reply_size;
	memcpy(reply + off, prefix, n);
	off += n;
	if (off < (size_t)reply_size) {
		n = strlen(suffix);
		if (n > (size_t)reply_size - off)
			n = (size_t)reply_size - off;
		memcpy(reply + off, suffix, n);
	}
}

int
main(int argc, char **argv)
{
	struct timeval start, end;
	long i;
	int c;
	long usec;

#ifdef _WIN32
	WSADATA WSAData;
	WSAStartup(0x101, &WSAData);
#else
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		return 1;
#endif

	while ((c = getopt(argc, argv, "n:c:v:d:h")) != -1) {
		switch (c) {
		case 'n':
			target_writes = atol(optarg);
			break;
		case 'c':
			n_conns = atol(optarg);
			break;
		case 'v':
			reply_size = atol(optarg);
			break;
		case 'd':
			duration = atol(optarg);
			break;
		case 'h':
		default:
			usage(argv[0]);
		}
	}

	if (target_writes < 0 || n_conns <= 0 || reply_size <= 0 || duration < 0)
		usage(argv[0]);
	if (target_writes == 0 && duration == 0)
		usage(argv[0]);

	base = event_base_new();
	conns = calloc((size_t)n_conns, sizeof(*conns));
	reply = malloc((size_t)reply_size);
	if (base == NULL || conns == NULL || reply == NULL) {
		fprintf(stderr, "allocation failed\n");
		return 1;
	}
	fill_reply();

	for (i = 0; i < n_conns; ++i) {
		if (make_conn(&conns[i]) < 0) {
			fprintf(stderr, "make_conn failed\n");
			return 1;
		}
	}

	evutil_gettimeofday(&start, NULL);
	if (duration > 0) {
		struct timeval tv;
		tv.tv_sec = duration;
		tv.tv_usec = 0;
		event_base_loopexit(base, &tv);
	}
	for (i = 0; i < n_conns && (target_writes == 0 ||
	    queued_writes < target_writes); ++i) {
		if (queue_one(&conns[i]) < 0)
			return 1;
	}
	event_base_dispatch(base);
	evutil_gettimeofday(&end, NULL);

	usec = elapsed_usec(&start, &end);
	printf("bench=bufferevent_writecb ns_per_op=%.2f\n",
	    completed_writes ? (double)usec * 1000.0 / completed_writes :
	    0.0);

	for (i = 0; i < n_conns; ++i) {
		if (conns[i].writer)
			bufferevent_free(conns[i].writer);
		if (conns[i].reader)
			bufferevent_free(conns[i].reader);
	}
	free(reply);
	free(conns);
	event_base_free(base);

#ifdef _WIN32
	WSACleanup();
#endif
	return 0;
}
