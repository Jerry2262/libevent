/*
 * Benchmark the bufferevent socket read callback path with Kvrocks-like
 * RESP input blocks.
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

#define DEFAULT_EVENTS 100000
#define DEFAULT_CONNS 64
#define DEFAULT_BLOCK_SIZE 256

#ifdef _WIN32
#define U64_FMT "%I64u"
typedef unsigned __int64 bench_u64;
#else
#define U64_FMT "%llu"
typedef unsigned long long bench_u64;
#endif

struct conn {
	struct bufferevent *bev;
	evutil_socket_t peer;
};

static struct event_base *base;
static struct conn *conns;
static char *block;
static long target_events = DEFAULT_EVENTS;
static long sent_blocks;
static long read_events;
static long n_conns = DEFAULT_CONNS;
static long block_size = DEFAULT_BLOCK_SIZE;
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
	fprintf(stderr, "Usage: %s [-n read_callbacks] [-c conns] "
	    "[-v block_size]\n", prog);
	exit(1);
}

static int
send_one(struct conn *c)
{
	ev_ssize_t n;

	if (sent_blocks >= target_events)
		return 0;

	n = send(c->peer, block, (int)block_size, 0);
	if (n == block_size) {
		++sent_blocks;
		return 0;
	}
	if (n < 0) {
		int err = evutil_socket_geterror(c->peer);
		if (EVUTIL_ERR_RW_RETRIABLE(err))
			return 0;
		fprintf(stderr, "send failed: %s\n",
		    evutil_socket_error_to_string(err));
		return -1;
	}

	fprintf(stderr, "short send\n");
	return -1;
}

static void
readcb(struct bufferevent *bev, void *arg)
{
	struct conn *c = arg;
	struct evbuffer *input = bufferevent_get_input(bev);
	size_t len = evbuffer_get_length(input);

	bytes_read += (bench_u64)len;
	evbuffer_drain(input, len);
	++read_events;

	if (read_events >= target_events) {
		event_base_loopexit(base, NULL);
		return;
	}

	if (send_one(c) < 0)
		event_base_loopexit(base, NULL);
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

	c->bev = bufferevent_socket_new(base, pair[0], BEV_OPT_CLOSE_ON_FREE);
	if (c->bev == NULL) {
		evutil_closesocket(pair[0]);
		evutil_closesocket(pair[1]);
		return -1;
	}
	c->peer = pair[1];

	bufferevent_setcb(c->bev, readcb, NULL, eventcb, c);
	bufferevent_enable(c->bev, EV_READ);
	return 0;
}

static void
fill_block(void)
{
	const char *prefix = "*3\r\n$3\r\nSET\r\n$16\r\nkvrocks:key:0001\r\n$";
	const char *suffix = "\r\nxxxxxxxxxxxxxxxx\r\n";
	size_t off = 0;
	size_t n;

	memset(block, 'x', (size_t)block_size);
	n = strlen(prefix);
	if (n > (size_t)block_size)
		n = (size_t)block_size;
	memcpy(block + off, prefix, n);
	off += n;
	if (off < (size_t)block_size) {
		n = strlen(suffix);
		if (n > (size_t)block_size - off)
			n = (size_t)block_size - off;
		memcpy(block + off, suffix, n);
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

	while ((c = getopt(argc, argv, "n:c:v:h")) != -1) {
		switch (c) {
		case 'n':
			target_events = atol(optarg);
			break;
		case 'c':
			n_conns = atol(optarg);
			break;
		case 'v':
			block_size = atol(optarg);
			break;
		case 'h':
		default:
			usage(argv[0]);
		}
	}

	if (target_events <= 0 || n_conns <= 0 || block_size <= 0)
		usage(argv[0]);

	base = event_base_new();
	conns = calloc((size_t)n_conns, sizeof(*conns));
	block = malloc((size_t)block_size);
	if (base == NULL || conns == NULL || block == NULL) {
		fprintf(stderr, "allocation failed\n");
		return 1;
	}
	fill_block();

	for (i = 0; i < n_conns; ++i) {
		if (make_conn(&conns[i]) < 0) {
			fprintf(stderr, "make_conn failed\n");
			return 1;
		}
	}

	evutil_gettimeofday(&start, NULL);
	for (i = 0; i < n_conns && sent_blocks < target_events; ++i) {
		if (send_one(&conns[i]) < 0)
			return 1;
	}
	event_base_dispatch(base);
	evutil_gettimeofday(&end, NULL);

	usec = elapsed_usec(&start, &end);
	printf("bench=bufferevent_readcb ops=%ld bytes=" U64_FMT
	    " conns=%ld block_size=%ld usec=%ld ops_sec=%.2f mb_sec=%.2f\n",
	    read_events, bytes_read, n_conns, block_size, usec,
	    usec ? (double)read_events * 1000000.0 / usec : 0.0,
	    usec ? (double)bytes_read / (1024.0 * 1024.0) * 1000000.0 /
		usec : 0.0);

	for (i = 0; i < n_conns; ++i) {
		if (conns[i].bev)
			bufferevent_free(conns[i].bev);
		if (conns[i].peer != EVUTIL_INVALID_SOCKET)
			evutil_closesocket(conns[i].peer);
	}
	free(block);
	free(conns);
	event_base_free(base);

#ifdef _WIN32
	WSACleanup();
#endif
	return 0;
}
