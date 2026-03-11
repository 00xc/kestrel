/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * High-performance HTTP server using io_uring and zero-copy APIs
 */

#define _GNU_SOURCE
#include "kestrel.h"
#include "args.h"
#include "decode.h"
#include "fcache.h"
#include "slab.h"

#include <err.h>
#include <grp.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define BACKLOG 512
#define QUEUE_DEPTH 4096
#define RECV_BUFFER_SIZE 4096UL
#define HDR_BUFFER_SIZE 128UL

#define READ_TIMEOUT_SEC 10
#define READ_TIMEOUT_NSEC 0

#define DEFAULT_PAGE "index.html"

#define ARRAY_SIZE(_arr) (sizeof(_arr) / sizeof(_arr[0]))

enum conn_state {
	CONN_READING,
	CONN_OPENING,
	CONN_SENDING,
	CONN_CLOSING,
	CONN_DONE,
};

static const char resp_404[] = "HTTP/1.1 404 Not Found\r\n"
							   "Server: kestrel\r\n"
							   "Content-Length: 0\r\n"
							   "Connection: close\r\n\r\n";
static const char resp_400[] = "HTTP/1.1 400 Bad Request\r\n"
							   "Server: kestrel\r\n"
							   "Content-Length: 0\r\n"
							   "Connection: close\r\n\r\n";
static const char resp_500[] = "HTTP/1.1 500 Internal Error\r\n"
							   "Server: kestrel\r\n"
							   "Content-Length: 0\r\n"
							   "Connection: close\r\n\r\n";
static const char resp_200[] = "HTTP/1.1 200 OK\r\n"
							   "Server: kestrel\r\n"
							   "Content-Length: %zu\r\n"
							   "%s\r\n";

static const char *state2str(enum conn_state st)
{
	switch (st) {
	case CONN_READING:
		return "CONN_READING";
	case CONN_OPENING:
		return "CONN_OPENING";
	case CONN_SENDING:
		return "CONN_SENDING";
	case CONN_CLOSING:
		return "CONN_CLOSING";
	case CONN_DONE:
		return "CONN_DONE";
	}
	return "??";
}

struct ks_hdr {
	int copied;
	unsigned int len;
	union {
		char buf[HDR_BUFFER_SIZE];
		const char *sbuf;
	};
};

struct ks_recv {
	char buf[RECV_BUFFER_SIZE];
	size_t off;
};

struct connection {
	/* Number of pending I/O requests */
	size_t pending;
	/* Socket */
	int fd;
	/* Whether to keep connection alive */
	int keepalive;
	/* State machine data */
	enum conn_state state;
	/* Reuse this hole for future fields */
	int _hole;
	/* Request receive buffer */
	struct ks_recv recv;
	/* Pointer into recv.buf with the file path */
	struct ks_path path;
	/* Header send buffer */
	struct ks_hdr hdr;
	/* File to send after header */
	struct ks_file *file;
	/* iovec to send header + file */
	struct iovec iov[2];
	struct msghdr msg;
};

/* Worker thread context */
struct worker_ctx {
	int id;
	int cpu;
	int opt;
	int listen;
	int ncpus;
	int port;
	struct io_uring ring;
	struct ks_slab conn_slab;
	struct ks_slab file_slab;
	struct ks_fcache fcache;
} __attribute__((aligned(64)));

static struct __kernel_timespec timeout = {
	.tv_sec = READ_TIMEOUT_SEC,
	.tv_nsec = READ_TIMEOUT_NSEC,
};

static int conn_parse_http(struct connection *conn)
{
	struct ks_recv *recv = &conn->recv;
	struct ks_path *path = &conn->path;
	char *start, *end, *hdrs;
	size_t len;

	if (recv->off < 4 || *(uint32_t *)recv->buf != *(uint32_t *)"GET ")
		return 1;

	start = recv->buf + 4;
	while (*start == '/')
		++start;

	end = memchr(start, ' ', recv->off - 4);
	if (!end)
		return 1;
	*end = '\0';

	if (uri_decode(start))
		return 1;

	len = (size_t)(end - start);
	if (len >= PATH_MAX)
		return 1;

	if (start[0] == '\0') {
		path->path = DEFAULT_PAGE;
		path->len = sizeof(DEFAULT_PAGE) - 1;
	} else {
		path->path = start;
		path->len = len;
	}

	hdrs = end + 1;
	conn->keepalive = memmem(hdrs, recv->off - (hdrs - recv->buf),
							 "Connection: close", 17) == NULL;
	return 0;
}

static struct io_uring_sqe *__get_sqe(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(ring);
	if (sqe)
		return sqe;

	io_uring_submit(ring);
	return io_uring_get_sqe(ring);
}

static int queue_accept(struct worker_ctx *ctx)
{
	struct io_uring_sqe *sqe = __get_sqe(&ctx->ring);

	if (!sqe)
		return -EAGAIN;

	io_uring_prep_multishot_accept_direct(sqe, ctx->listen, NULL, NULL,
										  0);
	io_uring_sqe_set_data(sqe, NULL);
	return io_uring_submit(&ctx->ring);
}

static int queue_read(struct worker_ctx *ctx, struct connection *conn,
					  struct io_uring_sqe *sqe)
{
	struct io_uring *ring = &ctx->ring;
	struct ks_recv *recv = &conn->recv;
	struct io_uring_sqe *sqe2;

	if (!sqe) {
		sqe = __get_sqe(ring);
		if (!sqe)
			return -EAGAIN;
	}

	conn->state = CONN_READING;

	io_uring_prep_recv(sqe, conn->fd, recv->buf + recv->off,
					   sizeof(recv->buf) - 1 - recv->off, 0);
	io_uring_sqe_set_data(sqe, conn);
	io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);

	sqe2 = io_uring_get_sqe(ring);
	if (sqe2) {
		sqe->flags |= IOSQE_IO_LINK;

		io_uring_prep_link_timeout(sqe2, &timeout, 0);
		io_uring_sqe_set_data(sqe2, conn);
		io_uring_sqe_set_flags(sqe2, IOSQE_CQE_SKIP_SUCCESS);
	}

	conn->pending++;
	return 0;
}

static int queue_send(struct worker_ctx *ctx, struct connection *conn)
{
	struct io_uring *ring = &ctx->ring;
	struct io_uring_sqe *sqe = __get_sqe(ring);
	struct msghdr *msg = &conn->msg;

	if (!sqe)
		return -EAGAIN;

	conn->state = CONN_SENDING;
	io_uring_prep_sendmsg_zc(sqe, conn->fd, msg, 0);
	io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
	io_uring_sqe_set_data(sqe, conn);

	conn->pending++;
	return 0;
}

static int queue_open(struct worker_ctx *ctx, struct connection *conn)
{
	struct io_uring *ring = &ctx->ring;
	struct io_uring_sqe *sqe = __get_sqe(ring);

	if (!sqe)
		return -EAGAIN;

	conn->state = CONN_OPENING;
	io_uring_prep_open(sqe, conn->path.path, O_RDONLY, 0);
	io_uring_sqe_set_data(sqe, conn);

	conn->pending++;
	return 0;
}

/*
 * Close a file descriptor without an associated connection. Used
 * if something goes wrong when accepting the connection.
 */
static int queue_close_accept(struct worker_ctx *ctx, int fd)
{
	struct io_uring *ring = &ctx->ring;
	struct io_uring_sqe *sqe = __get_sqe(ring);

	if (!sqe)
		return -EAGAIN;

	io_uring_prep_close_direct(sqe, fd);
	io_uring_sqe_set_data(sqe, MAP_FAILED);
	io_uring_sqe_set_flags(sqe, IOSQE_CQE_SKIP_SUCCESS);
	return 0;
}

static int queue_close(struct worker_ctx *ctx, struct connection *conn)
{
	struct io_uring *ring = &ctx->ring;
	struct io_uring_sqe *sqe = __get_sqe(ring);

	if (!sqe)
		return -EAGAIN;

	conn->state = CONN_CLOSING;
	io_uring_prep_close_direct(sqe, conn->fd);
	io_uring_sqe_set_data(sqe, conn);
	conn->pending++;
	return 0;
}

static size_t response_404(struct connection *conn)
{
	conn->file->len = 0;
	conn->hdr.copied = 0;
	conn->hdr.sbuf = resp_404;
	conn->hdr.len = sizeof(resp_404) - 1;
	return 404;
}

static size_t response_400(struct connection *conn)
{
	conn->file->len = 0;
	conn->hdr.copied = 0;
	conn->hdr.sbuf = resp_400;
	conn->hdr.len = sizeof(resp_400) - 1;
	return 400;
}
static size_t response_500(struct connection *conn)
{
	conn->file->len = 0;
	conn->hdr.copied = 0;
	conn->hdr.sbuf = resp_500;
	conn->hdr.len = sizeof(resp_500) - 1;
	return 500;
}

static size_t response_200(struct connection *conn)
{
	conn->hdr.copied = 1;
	conn->hdr.len =
			snprintf(conn->hdr.buf, sizeof(conn->hdr.buf), resp_200,
					 conn->file->len,
					 conn->keepalive ? "" : "Connection: close\r\n");
	return 200;
}

static void file_raw_close(struct ks_file *file)
{
	if (file->map != MAP_FAILED && file->len)
		munmap(file->map, file->len);
	if (file->fd >= 0)
		close(file->fd);
}

static inline void conn_init(struct connection *conn)
{
	conn->recv.off = 0;
	conn->hdr.len = 0;
	conn->pending = 0;
	conn->keepalive = 0;
	conn->file->map = MAP_FAILED;
	conn->file->len = 0;
	conn->file->fd = -1;
}

static void conn_clear(struct worker_ctx *ctx, struct connection *conn)
{
	struct ks_file *file = conn->file;

	(void)ctx;

	if (!file->cached) {
		file_raw_close(file);
	} else {
		fcache_close(&ctx->fcache, conn->file);
		conn->file = slab_alloc(&ctx->file_slab);
	}

	conn_init(conn);
}

static void conn_free(struct worker_ctx *ctx, struct connection *conn)
{
	if (!conn)
		return;

	if (conn->fd >= 0)
		close(conn->fd);

	conn_clear(ctx, conn);
	slab_free(&ctx->file_slab, conn->file);
	slab_free(&ctx->conn_slab, conn);
}

static void conn_prepare_iov(struct connection *conn)
{
	struct msghdr *msg = &conn->msg;
	struct ks_hdr *hdr = &conn->hdr;
	struct ks_file *file = conn->file;

	msg->msg_name = NULL;
	msg->msg_namelen = 0;
	msg->msg_control = NULL;
	msg->msg_controllen = 0;

	msg->msg_iov = conn->iov;
	msg->msg_iov[0].iov_base = hdr->copied ? hdr->buf :
											 (char *)hdr->sbuf;
	msg->msg_iov[0].iov_len = hdr->len;
	msg->msg_iov[1].iov_base = file->map;
	msg->msg_iov[1].iov_len = file->len;
	msg->msg_iovlen = file->len ? 2 : 1;
}

static int conn_start_send(struct worker_ctx *ctx,
						   struct connection *conn, size_t code)
{
	switch (code) {
	case 404:
		response_404(conn);
		break;
	case 400:
		response_400(conn);
		break;
	case 200:
		response_200(conn);
		break;
	default:
		response_500(conn);
		break;
	}
	conn_prepare_iov(conn);
	return queue_send(ctx, conn);
}

static void conn_iov_advance(struct connection *conn, size_t sent)
{
	struct iovec *iov;

	while (sent && conn->msg.msg_iovlen) {
		iov = &conn->msg.msg_iov[0];

		if (sent < iov->iov_len) {
			iov->iov_base = (char *)iov->iov_base + sent;
			iov->iov_len -= sent;
			return;
		}

		sent -= iov->iov_len;

		conn->msg.msg_iov++;
		conn->msg.msg_iovlen--;
	}
}

static int conn_complete_send(struct worker_ctx *ctx,
							  struct connection *conn,
							  const struct io_uring_cqe *cqe)
{
	if (cqe->res < 0)
		return queue_close(ctx, conn);

	/* If it is an actual completion, process it */
	if (!(cqe->flags & IORING_CQE_F_NOTIF)) {
		if (cqe->flags & IORING_CQE_F_MORE)
			conn->pending++;

		conn_iov_advance(conn, cqe->res);
		if (conn->msg.msg_iovlen)
			return queue_send(ctx, conn);
	}

	if (conn->pending)
		return 0;

	/* File was fully transferred and there is no outstanding I/O,
	 * determine the next state for the connection. */
	if (conn->keepalive) {
		conn_clear(ctx, conn);
		return queue_read(ctx, conn, NULL);
	} else {
		return queue_close(ctx, conn);
	}
}

static int conn_complete_read(struct worker_ctx *ctx,
							  struct connection *conn,
							  const struct io_uring_cqe *cqe)
{
	struct ks_recv *recv = &conn->recv;
	struct ks_file *file;
	char *end;

	if (cqe->res <= 0)
		return queue_close(ctx, conn);

	recv->off += cqe->res;
	recv->buf[recv->off] = '\0';

	end = memmem(recv->buf, recv->off, "\r\n\r\n", 4);
	if (!end) {
		if (recv->off < sizeof(recv->buf) - 1)
			return queue_read(ctx, conn, NULL);

		return conn_start_send(ctx, conn, 400);
	}

	recv->off = end - recv->buf;
	if (conn_parse_http(conn))
		return conn_start_send(ctx, conn, 400);

	file = fcache_open(&ctx->fcache, &conn->path);
	if (file) {
		slab_free(&ctx->file_slab, conn->file);
		conn->file = file;
		return conn_start_send(ctx, conn, 200);
	}

	return queue_open(ctx, conn);
}

static int conn_complete_open(struct worker_ctx *ctx,
							  struct connection *conn,
							  struct io_uring_cqe *cqe)
{
	struct ks_file *file = conn->file;
	struct stat st;
	int ret;

	file->len = 0;
	file->cached = 0;

	if (cqe->res < 0)
		return conn_start_send(ctx, conn, 404);

	file->fd = cqe->res;
	ret = fstat(file->fd, &st);
	if (ret)
		return conn_start_send(ctx, conn, 500);

	file->len = st.st_size;
	if (!file->len)
		return conn_start_send(ctx, conn, 200);

	file->map =
			mmap(NULL, file->len, PROT_READ, MAP_PRIVATE, file->fd, 0);
	if (file->map == MAP_FAILED)
		return conn_start_send(ctx, conn, 500);

	file = fcache_insert(&ctx->fcache, &conn->path, file);
	if (file) {
		/* Close and free the evicted file */
		file_raw_close(file);
		slab_free(&ctx->file_slab, file);
	}

	return conn_start_send(ctx, conn, 200);
}

static int conn_handle_close(struct connection *conn,
							 struct io_uring_cqe *cqe)
{
	conn->state = CONN_DONE;
	return cqe->res;
}

static void conn_complete_io(struct worker_ctx *ctx,
							 struct connection *conn)
{
	if (conn->state == CONN_DONE && !conn->pending)
		conn_free(ctx, conn);
}

static int conn_handle_io(struct worker_ctx *ctx,
						  struct connection *conn,
						  struct io_uring_cqe *cqe)
{
	switch (conn->state) {
	case CONN_READING:
		return conn_complete_read(ctx, conn, cqe);
	case CONN_OPENING:
		return conn_complete_open(ctx, conn, cqe);
	case CONN_SENDING:
		return conn_complete_send(ctx, conn, cqe);
	case CONN_CLOSING:
		return conn_handle_close(conn, cqe);
	case CONN_DONE:
		warnx("got completion for connection in state DONE (%s)",
			  cqe->res < 0 ? strerror(-cqe->res) : "success");
		return 0;
	default:
		return -ENOTSUP;
	}
}

static inline int conn_start_accept(struct worker_ctx *ctx,
									struct connection *conn)
{
	struct sock_opt_req {
		struct io_uring_sqe *sqe;
		int level;
		int name;
		int *val;
		int len;
	} opt[] = {
		{ NULL, IPPROTO_TCP, TCP_NODELAY, &ctx->opt, sizeof(int) },
		{ NULL, SOL_SOCKET, SO_INCOMING_CPU, &ctx->cpu, sizeof(int) },
		{ 0 },
	};
	size_t i, j, cnt = 1;

	/* __get_sqe() flushes pending submissions if necessary to get a new
	 * SQE. This is fine for the first SQE, but not later, as it would
	 * flush the SQE chain we are building. */
	opt[0].sqe = __get_sqe(&ctx->ring);
	if (!opt[0].sqe)
		return -EAGAIN;

	/* Try to acquire as much SQEs as we need for all options we want
	 * to set + recv() */
	for (i = 1; i < ARRAY_SIZE(opt); ++i) {
		opt[i].sqe = io_uring_get_sqe(&ctx->ring);
		if (!opt[i].sqe)
			break;
		++cnt;
	}

	/* Use all SQEs but 1 for setsockopt() */
	for (j = 0; j < cnt - 1; ++j) {
		struct sock_opt_req *rq = &opt[j];

		io_uring_prep_cmd_sock(rq->sqe, SOCKET_URING_OP_SETSOCKOPT,
							   conn->fd, rq->level, rq->name, rq->val,
							   rq->len);
		io_uring_sqe_set_data(rq->sqe, conn);
		io_uring_sqe_set_flags(rq->sqe, IOSQE_FIXED_FILE |
												IOSQE_CQE_SKIP_SUCCESS |
												IOSQE_IO_LINK);
	}

	/* Always use last SQE for recv() */
	return queue_read(ctx, conn, opt[cnt - 1].sqe);
}

static int conn_handle_accept(struct worker_ctx *ctx,
							  struct io_uring_cqe *cqe)
{
	struct connection *conn;

	if (!(cqe->flags & IORING_CQE_F_MORE))
		queue_accept(ctx);

	if (cqe->res < 0)
		return cqe->res;

	conn = slab_alloc(&ctx->conn_slab);
	if (!conn)
		return queue_close_accept(ctx, cqe->res);

	conn->file = slab_alloc(&ctx->file_slab);
	if (!conn->file) {
		slab_free(&ctx->conn_slab, conn);
		return queue_close_accept(ctx, cqe->res);
	}

	/* We've successfully allocated everything, now queue setsockopt()
	 * and recv() */
	conn->fd = cqe->res;
	conn_init(conn);
	return conn_start_accept(ctx, conn);
}

static void handle_completions(struct worker_ctx *ctx)
{
	struct io_uring_cqe *cqe;
	struct connection *conn;
	enum conn_state st;
	unsigned count = 0;
	unsigned head;
	int ret;

	io_uring_for_each_cqe(&ctx->ring, head, cqe)
	{
		++count;

		conn = io_uring_cqe_get_data(cqe);
		if (conn == MAP_FAILED)
			continue;

		/* Check if this was a completed accept() */
		if (!conn) {
			ret = conn_handle_accept(ctx, cqe);
			if (ret)
				warnx("io_uring: accept: %s", strerror(-ret));
			continue;
		}

		conn->pending--;
		st = conn->state;
		ret = conn_handle_io(ctx, conn, cqe);
		if (ret && ret != -ECONNRESET)
			warnx("I/O request failed: %s (state = %s)", strerror(-ret),
				  state2str(st));

		conn_complete_io(ctx, conn);
	}

	io_uring_cq_advance(&ctx->ring, count);
}

static void pin_to_core(struct worker_ctx *ctx)
{
	cpu_set_t set;
	int ret;

	CPU_ZERO(&set);
	CPU_SET(ctx->cpu, &set);
	if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set))
		err(EXIT_FAILURE, "pthread_setaffinity_np");

	ret = io_uring_register_iowq_aff(&ctx->ring, sizeof(set), &set);
	if (ret)
		errx(EXIT_FAILURE, "io_uring_register_iowq_aff: %s",
			 strerror(-ret));

	ret = io_uring_register_ring_fd(&ctx->ring);
	if (ret != 1)
		errx(EXIT_FAILURE, "io_uring_register_ring_fd: %s",
			 strerror(-ret));
}

static int create_listener(struct worker_ctx *ctx)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = INADDR_ANY,
		.sin_port = htons(ctx->port),
	};
	int fd, opt = 1, timeout = 4, cpu = ctx->cpu;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0)
		err(EXIT_FAILURE, "socket");

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
		err(EXIT_FAILURE, "setsockopt(SO_REUSEADDR)");

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)))
		err(EXIT_FAILURE, "setsockopt(SO_REUSEPORT)");

	if (setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &timeout,
				   sizeof(timeout)))
		err(EXIT_FAILURE, "setsockopt(TCP_DEFER_ACCEPT)");

	if (setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &opt, sizeof(opt)))
		err(EXIT_FAILURE, "setsockopt(TCP_QUICKACK)");

	if (setsockopt(fd, SOL_SOCKET, SO_INCOMING_CPU, &cpu, sizeof(cpu)))
		err(EXIT_FAILURE, "setsockopt(SO_INCOMING_CPU)");

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)))
		err(EXIT_FAILURE, "bind");

	if (listen(fd, BACKLOG))
		err(EXIT_FAILURE, "listen");

	return fd;
}

static void worker_init(struct worker_ctx *ctx)
{
	struct io_uring_params params = {
		.flags = IORING_SETUP_SINGLE_ISSUER |
				 IORING_SETUP_COOP_TASKRUN | IORING_SETUP_DEFER_TASKRUN,
	};
	int ret;

	ctx->opt = 1;
	ctx->cpu = ctx->id % ctx->ncpus;
	ctx->listen = create_listener(ctx);

	slab_init(&ctx->conn_slab, sizeof(struct connection));
	slab_init(&ctx->file_slab, sizeof(struct ks_file));
	fcache_init(&ctx->fcache);

	ret = io_uring_queue_init_params(QUEUE_DEPTH, &ctx->ring, &params);
	if (ret)
		errx(EXIT_FAILURE, "io_uring_queue_init_params: %s",
			 strerror(-ret));

	if (!(params.features & IORING_FEAT_CQE_SKIP))
		errx(EXIT_FAILURE, "io_uring: IORING_FEAT_CQE_SKIP required");

	ret = io_uring_register_files_sparse(&ctx->ring, QUEUE_DEPTH);
	if (ret)
		errx(EXIT_FAILURE, "io_uring_register_files_sparse: %s",
			 strerror(-ret));

	pin_to_core(ctx);
}

static void worker_destroy(struct worker_ctx *ctx)
{
	struct ks_file *file;

	io_uring_queue_exit(&ctx->ring);
	while (1) {
		file = fcache_pop(&ctx->fcache);
		if (!file)
			break;
		file_raw_close(file);
		slab_free(&ctx->file_slab, file);
	};
	slab_destroy(&ctx->conn_slab);
	slab_destroy(&ctx->file_slab);
	close(ctx->listen);
}

static void *worker_thread(void *arg)
{
	struct worker_ctx *ctx = arg;
	struct io_uring_cqe *tmp;

	int ret;

	if (unshare(CLONE_FILES))
		err(EXIT_FAILURE, "unshare");

	worker_init(ctx);
	setup_seccomp();

	if (queue_accept(ctx) <= 0)
		errx(EXIT_FAILURE, "queue_accept");

	while (1) {
		ret = io_uring_wait_cqe(&ctx->ring, &tmp);
		if (ret < 0) {
			if (ret == -EINTR)
				continue;
			warnx("io_uring_wait_cqe: %s", strerror(-ret));
			break;
		}

		handle_completions(ctx);
		io_uring_submit(&ctx->ring);
	}

	worker_destroy(ctx);
	return NULL;
}

static void ks_main(struct args *args, size_t ncpus)
{
	struct worker_ctx *workers;
	pthread_t *threads;
	size_t i;

	threads = calloc(args->nthreads, sizeof(*threads));
	if (!threads)
		err(EXIT_FAILURE, "calloc threads");

	workers = aligned_alloc(64, args->nthreads * sizeof(*workers));
	if (!workers)
		err(EXIT_FAILURE, "alligned_alloc() workers");

	for (i = 0; i < args->nthreads; i++) {
		workers[i].id = i;
		workers[i].ncpus = ncpus;
		workers[i].port = args->port;

		if (pthread_create(&threads[i], NULL, worker_thread,
						   &workers[i]))
			err(EXIT_FAILURE, "pthread_create");
	}

	for (i = 0; i < args->nthreads; i++) {
		if (pthread_join(threads[i], NULL))
			warn("pthread_join");
	}

	free(workers);
	free(threads);
}

static void drop_privs(struct passwd *pw)
{
	if (setgroups(0, NULL))
		err(EXIT_FAILURE, "setgroups");

	if (setgid(pw->pw_gid))
		err(EXIT_FAILURE, "setgid");

	if (setuid(pw->pw_uid))
		err(EXIT_FAILURE, "setuid");

	if (setuid(0) != -1)
		errx(EXIT_FAILURE, "failed to drop privileges");
}

int main(int argc, char **argv)
{
	struct passwd *pw;
	struct args args;
	int ncpus;

	parse_args(argc, argv, &args);

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus <= 0)
		err(EXIT_FAILURE, "sysconf(_SC_NPROCESSORS_ONLN)");

	pw = getpwnam(args.user);
	if (!pw)
		err(EXIT_FAILURE, "getpwnam(%s)", args.user);

	if (chroot(args.root))
		err(EXIT_FAILURE, "chroot(%s)", args.root);

	if (chdir("/"))
		err(EXIT_FAILURE, "chdir /");

	drop_privs(pw);

	printf("HTTP server listening on port %d\n", args.port);
	printf("Threads: %lu, root: %s\n", args.nthreads, args.root);

	ks_main(&args, ncpus);

	return EXIT_SUCCESS;
}
