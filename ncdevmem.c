// SPDX-License-Identifier: GPL-2.0
/*
 * tcpdevmem netcat. Works similarly to netcat but does device memory TCP
 * instead of regular TCP. Uses udmabuf to mock a dmabuf provider.
 *
 * Usage:
 *
 *     On server:
 *     ncdevmem -s <server IP> [-c <client IP>] -f eth1 -l -p 5201
 *
 *     On client:
 *     echo -n "hello\nworld" | \
 *		ncdevmem -s <server IP> [-c <client IP>] -p 5201 -f eth1
 *
 * Note this is compatible with regular netcat. i.e. the sender or receiver can
 * be replaced with regular netcat to test the RX or TX path in isolation.
 *
 * Test data validation (devmem TCP on RX only):
 *
 *     On server:
 *     ncdevmem -s <server IP> [-c <client IP>] -f eth1 -l -p 5201 -v 7
 *
 *     On client:
 *     yes $(echo -e \\x01\\x02\\x03\\x04\\x05\\x06) | \
 *             head -c 1G | \
 *             nc <server IP> 5201 -p 5201
 *
 * Test data validation (devmem TCP on RX and TX, validation happens on RX):
 *
 *	On server:
 *	ncdevmem -s <server IP> [-c <client IP>] -l -p 5201 -v 8 -f eth1
 *
 *	On client:
 *	yes $(echo -e \\x01\\x02\\x03\\x04\\x05\\x06\\x07) | \
 *		head -c 1M | \
 *		ncdevmem -s <server IP> [-c <client IP>] -p 5201 -f eth1
 */
#define _GNU_SOURCE
#define __EXPORTED_HEADERS__

#include <linux/uio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#define __iovec_defined
#include <fcntl.h>
#include <malloc.h>
#include <error.h>
#include <poll.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/time.h>

#include <linux/memfd.h>
#include <linux/dma-buf.h>
#include <linux/errqueue.h>
#include <linux/types.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>
#include <linux/netdev.h>
#include <linux/ethtool_netlink.h>
#include <time.h>
#include <net/if.h>

#include "netdev-user.h"
#include "ethtool-user.h"
#include <ynl.h>

#define __HIP_PLATFORM_AMD__
#include <hip/hip_runtime.h>
#include <hsa/amd_hsa_common.h>
#include <hsa/hsa_ext_amd.h>

#define PAGE_SHIFT 12
#define TEST_PREFIX "ncdevmem"
#define NUM_PAGES 16000

#ifndef MSG_SOCK_DEVMEM
#define MSG_SOCK_DEVMEM 0x2000000
#endif

#define MAX_IOV 1024

static size_t max_chunk;
static char *server_ip;
static char *client_ip;
static char *port;
static size_t do_validation;
static int start_queue = -1;
static int num_queues = -1;
static char *ifname;
static unsigned int ifindex;
static unsigned int dmabuf_id;
static uint32_t tx_dmabuf_id;
static int waittime_ms = 500;

struct memory_buffer {
	int fd;
	size_t offset;
	size_t size;

	int devfd;
	int memfd;
	void *buf_mem;
};

static void print_nonzero_bytes(void *ptr, size_t size)
{
	unsigned char *p = ptr;
	unsigned int i;

	for (i = 0; i < size; i++)
		putchar('0' + p[i]);

	putchar('\n');
}

void validate_buffer(void *line, size_t size, size_t seed)
{
	unsigned char *ptr = line;
	static int errors;
	size_t i;

	seed = seed % do_validation;

	for (i = 0; i < size; i++) {
		if (ptr[i] != seed) {
			fprintf(stderr,
				"Failed validation: expected=%u, actual=%u, index=%lu\n",
				seed, ptr[i], i);
			errors++;
			if (errors > 20)
				error(1, 0, "validation failed.");
		}
		seed++;
		if (seed == do_validation)
			seed = 0;
	}

	// fprintf(stdout, "Validated buffer\n");
}

static int rxq_num(int ifindex)
{
	struct ethtool_channels_get_req *req;
	struct ethtool_channels_get_rsp *rsp;
	struct ynl_error yerr;
	struct ynl_sock *ys;
	int num = -1;

	ys = ynl_sock_create(&ynl_ethtool_family, &yerr);
	if (!ys) {
		fprintf(stderr, "YNL: %s\n", yerr.msg);
		return -1;
	}

	req = ethtool_channels_get_req_alloc();
	ethtool_channels_get_req_set_header_dev_index(req, ifindex);
	rsp = ethtool_channels_get(ys, req);
	if (rsp)
		num = rsp->rx_count + rsp->combined_count;
	ethtool_channels_get_req_free(req);
	ethtool_channels_get_rsp_free(rsp);

	ynl_sock_destroy(ys);

	return num;
}

#define run_command(cmd, ...)                                           \
	({                                                              \
		char command[256];                                      \
		memset(command, 0, sizeof(command));                    \
		snprintf(command, sizeof(command), cmd, ##__VA_ARGS__); \
		fprintf(stderr, "Running: %s\n", command);                       \
		system(command);                                        \
	})

static int reset_flow_steering(void)
{
	/* Depending on the NIC, toggling ntuple off and on might not
	 * be allowed. Additionally, attempting to delete existing filters
	 * will fail if no filters are present. Therefore, do not enforce
	 * the exit status.
	 */

	run_command("sudo ethtool -K %s ntuple off >&2", ifname);
	run_command("sudo ethtool -K %s ntuple on >&2", ifname);
	run_command(
		"sudo ethtool -n %s | grep 'Filter:' | awk '{print $2}' | xargs -n1 ethtool -N %s delete >&2",
		ifname, ifname);
	return 0;
}

static const char *tcp_data_split_str(int val)
{
	switch (val) {
	case 0:
		return "off";
	case 1:
		return "auto";
	case 2:
		return "on";
	default:
		return "?";
	}
}

static int configure_headersplit(bool on)
{
	struct ethtool_rings_get_req *get_req;
	struct ethtool_rings_get_rsp *get_rsp;
	struct ethtool_rings_set_req *req;
	struct ynl_error yerr;
	struct ynl_sock *ys;
	int ret;

	ys = ynl_sock_create(&ynl_ethtool_family, &yerr);
	if (!ys) {
		fprintf(stderr, "YNL: %s\n", yerr.msg);
		return -1;
	}

	req = ethtool_rings_set_req_alloc();
	ethtool_rings_set_req_set_header_dev_index(req, ifindex);
	/* 0 - off, 1 - auto, 2 - on */
	ethtool_rings_set_req_set_tcp_data_split(req, on ? 2 : 0);
	ret = ethtool_rings_set(ys, req);
	if (ret < 0)
		fprintf(stderr, "YNL failed: %s\n", ys->err.msg);
	ethtool_rings_set_req_free(req);

	if (ret == 0) {
		get_req = ethtool_rings_get_req_alloc();
		ethtool_rings_get_req_set_header_dev_index(get_req, ifindex);
		get_rsp = ethtool_rings_get(ys, get_req);
		ethtool_rings_get_req_free(get_req);
		if (get_rsp)
			fprintf(stderr, "TCP header split: %s\n",
				tcp_data_split_str(get_rsp->tcp_data_split));
		ethtool_rings_get_rsp_free(get_rsp);
	}

	ynl_sock_destroy(ys);

	return ret;
}

static int configure_rss(void)
{
	return run_command("sudo ethtool -X %s equal %d >&2", ifname, start_queue);
}

static int configure_channels(unsigned int rx, unsigned int tx)
{
	return run_command("sudo ethtool -L %s rx %u tx %u", ifname, rx, tx);
}

static int configure_flow_steering(struct sockaddr_in6 *server_sin)
{
	const char *type = "tcp6";
	const char *server_addr;
	char buf[40];

	inet_ntop(AF_INET6, &server_sin->sin6_addr, buf, sizeof(buf));
	server_addr = buf;

	if (IN6_IS_ADDR_V4MAPPED(&server_sin->sin6_addr)) {
		type = "tcp4";
		server_addr = strrchr(server_addr, ':') + 1;
	}

	/* Try configure 5-tuple */
	if (run_command("sudo ethtool -N %s flow-type %s %s %s dst-ip %s %s %s dst-port %s queue %d >&2",
			   ifname,
			   type,
			   client_ip ? "src-ip" : "",
			   client_ip ?: "",
			   server_addr,
			   client_ip ? "src-port" : "",
			   client_ip ? port : "",
			   port, start_queue))
		/* If that fails, try configure 3-tuple */
		if (run_command("sudo ethtool -N %s flow-type %s dst-ip %s dst-port %s queue %d >&2",
				ifname,
				type,
				server_addr,
				port, start_queue))
			/* If that fails, return error */
			return -1;

	return 0;
}

static int bind_rx_queue(unsigned int ifindex, unsigned int dmabuf_fd,
			 struct netdev_queue_id *queues,
			 unsigned int n_queue_index, struct ynl_sock **ys)
{
	struct netdev_bind_rx_req *req = NULL;
	struct netdev_bind_rx_rsp *rsp = NULL;
	struct ynl_error yerr;

	*ys = ynl_sock_create(&ynl_netdev_family, &yerr);
	if (!*ys) {
		fprintf(stderr, "YNL: %s\n", yerr.msg);
		return -1;
	}

	req = netdev_bind_rx_req_alloc();
	netdev_bind_rx_req_set_ifindex(req, ifindex);
	netdev_bind_rx_req_set_fd(req, dmabuf_fd);
	__netdev_bind_rx_req_set_queues(req, queues, n_queue_index);

	rsp = netdev_bind_rx(*ys, req);
	if (!rsp) {
		perror("netdev_bind_rx");
		goto err_close;
	}

	if (!rsp->_present.id) {
		perror("id not present");
		goto err_close;
	}

	fprintf(stderr, "got dmabuf id=%d\n", rsp->id);
	dmabuf_id = rsp->id;

	netdev_bind_rx_req_free(req);
	netdev_bind_rx_rsp_free(rsp);

	return 0;

err_close:
	fprintf(stderr, "YNL failed: %s\n", (*ys)->err.msg);
	netdev_bind_rx_req_free(req);
	ynl_sock_destroy(*ys);
	return -1;
}

static int bind_tx_queue(unsigned int ifindex, unsigned int dmabuf_fd,
			 struct ynl_sock **ys)
{
	struct netdev_bind_tx_req *req = NULL;
	struct netdev_bind_tx_rsp *rsp = NULL;
	struct ynl_error yerr;

	*ys = ynl_sock_create(&ynl_netdev_family, &yerr);
	if (!*ys) {
		fprintf(stderr, "YNL: %s\n", yerr.msg);
		return -1;
	}

	req = netdev_bind_tx_req_alloc();
	netdev_bind_tx_req_set_ifindex(req, ifindex);
	netdev_bind_tx_req_set_fd(req, dmabuf_fd);

	rsp = netdev_bind_tx(*ys, req);
	if (!rsp) {
		perror("netdev_bind_tx");
		goto err_close;
	}

	if (!rsp->_present.id) {
		perror("id not present");
		goto err_close;
	}

	fprintf(stderr, "got tx dmabuf id=%d\n", rsp->id);
	tx_dmabuf_id = rsp->id;

	netdev_bind_tx_req_free(req);
	netdev_bind_tx_rsp_free(rsp);

	return 0;

err_close:
	fprintf(stderr, "YNL failed: %s\n", (*ys)->err.msg);
	netdev_bind_tx_req_free(req);
	ynl_sock_destroy(*ys);
	return -1;
}

static void enable_reuseaddr(int fd)
{
	int opt = 1;
	int ret;

	ret = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
	if (ret)
		error(1, errno, "%s: [FAIL, SO_REUSEPORT]\n", TEST_PREFIX);

	ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	if (ret)
		error(1, errno, "%s: [FAIL, SO_REUSEADDR]\n", TEST_PREFIX);
}

static int parse_address(const char *str, int port, struct sockaddr_in6 *sin6)
{
	int ret;

	sin6->sin6_family = AF_INET6;
	sin6->sin6_port = htons(port);

	ret = inet_pton(sin6->sin6_family, str, &sin6->sin6_addr);
	if (ret != 1) {
		/* fallback to plain IPv4 */
		ret = inet_pton(AF_INET, str, &sin6->sin6_addr.s6_addr32[3]);
		if (ret != 1)
			return -1;

		/* add ::ffff prefix */
		sin6->sin6_addr.s6_addr32[0] = 0;
		sin6->sin6_addr.s6_addr32[1] = 0;
		sin6->sin6_addr.s6_addr16[4] = 0;
		sin6->sin6_addr.s6_addr16[5] = 0xffff;
	}

	return 0;
}

static struct netdev_queue_id *create_queues(void)
{
	struct netdev_queue_id *queues;
	size_t i = 0;

	queues = calloc(num_queues, sizeof(*queues));
	for (i = 0; i < num_queues; i++) {
		queues[i]._present.type = 1;
		queues[i]._present.id = 1;
		queues[i].type = NETDEV_QUEUE_TYPE_RX;
		queues[i].id = start_queue + i;
	}

	return queues;
}

static int do_server(struct memory_buffer *mem)
{
	char ctrl_data[sizeof(int) * 20000];
	size_t non_page_aligned_frags = 0;
	struct sockaddr_in6 client_addr;
	struct sockaddr_in6 server_sin;
	size_t page_aligned_frags = 0;
	size_t total_received = 0;
	socklen_t client_addr_len;
	size_t endptr = -1;
	bool is_devmem = false;
	char *tmp_mem = NULL;
	struct ynl_sock *ys;
	char iobuf[819200];
	char buffer[256];
	int socket_fd;
	int client_fd;
	int ret;

	ret = parse_address(server_ip, atoi(port), &server_sin);
	if (ret < 0)
		error(1, 0, "parse server address");

	if (reset_flow_steering())
		error(1, 0, "Failed to reset flow steering\n");

	if (configure_headersplit(1))
		error(1, 0, "Failed to enable TCP header split\n");

	/* Configure RSS to divert all traffic from our devmem queues */
	if (configure_rss())
		error(1, 0, "Failed to configure rss\n");

	/* Flow steer our devmem flows to start_queue */
	if (configure_flow_steering(&server_sin))
		error(1, 0, "Failed to configure flow steering\n");

	sleep(1);

	if (bind_rx_queue(ifindex, mem->fd, create_queues(), num_queues, &ys))
		error(1, 0, "Failed to bind\n");

	tmp_mem = malloc(mem->size);
	if (!tmp_mem)
		error(1, ENOMEM, "malloc failed");

	socket_fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (socket_fd < 0)
		error(1, errno, "%s: [FAIL, create socket]\n", TEST_PREFIX);

	enable_reuseaddr(socket_fd);

	fprintf(stderr, "binding to address %s:%d\n", server_ip,
		ntohs(server_sin.sin6_port));

	ret = bind(socket_fd, &server_sin, sizeof(server_sin));
	if (ret)
		error(1, errno, "%s: [FAIL, bind]\n", TEST_PREFIX);

	ret = listen(socket_fd, 1);
	if (ret)
		error(1, errno, "%s: [FAIL, listen]\n", TEST_PREFIX);

	client_addr_len = sizeof(client_addr);

	inet_ntop(AF_INET6, &server_sin.sin6_addr, buffer,
		  sizeof(buffer));
	fprintf(stderr, "Waiting or connection on %s:%d\n", buffer,
		ntohs(server_sin.sin6_port));
	client_fd = accept(socket_fd, &client_addr, &client_addr_len);

	inet_ntop(AF_INET6, &client_addr.sin6_addr, buffer,
		  sizeof(buffer));
	fprintf(stderr, "Got connection from %s:%d\n", buffer,
		ntohs(client_addr.sin6_port));

	while (1) {
		struct iovec iov = { .iov_base = iobuf,
				     .iov_len = sizeof(iobuf) };
		struct dmabuf_cmsg *dmabuf_cmsg = NULL;
		struct cmsghdr *cm = NULL;
		struct msghdr msg = { 0 };
		struct dmabuf_token token;
		ssize_t ret;

		is_devmem = false;

		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = ctrl_data;
		msg.msg_controllen = sizeof(ctrl_data);
		ret = recvmsg(client_fd, &msg, MSG_SOCK_DEVMEM);
		// fprintf(stderr, "recvmsg ret=%ld\n", ret);
		if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			continue;
		if (ret < 0) {
			perror("recvmsg");
			continue;
		}
		if (ret == 0) {
			fprintf(stderr, "client exited\n");
			break;
		}
		fprintf(stderr, "recvmsg_ret=%d\n", ret);

		for (cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
			if (cm->cmsg_level != SOL_SOCKET ||
			    (cm->cmsg_type != SCM_DEVMEM_DMABUF &&
			     cm->cmsg_type != SCM_DEVMEM_LINEAR)) {
				fprintf(stderr, "skipping non-devmem cmsg\n");
				continue;
			}

			dmabuf_cmsg = (struct dmabuf_cmsg *)CMSG_DATA(cm);
			is_devmem = true;

			if (cm->cmsg_type == SCM_DEVMEM_LINEAR) {
				/* TODO: process data copied from skb's linear
				 * buffer.
				 */
				fprintf(stderr,
					"SCM_DEVMEM_LINEAR. dmabuf_cmsg->frag_size=%u\n",
					dmabuf_cmsg->frag_size);

				continue;
			}

			if (dmabuf_cmsg->dmabuf_id != dmabuf_id)
				error(1, 0,
				      "received on wrong dmabuf_id: flow steering error\n");

			if (endptr == -1) {
				endptr = dmabuf_cmsg->frag_offset;
			} else {
				if (endptr == dmabuf_cmsg->frag_offset) {
					page_aligned_frags++;
				} else {
					endptr = dmabuf_cmsg->frag_offset;
					non_page_aligned_frags++;
				}
			}

			endptr += dmabuf_cmsg->frag_size;

			hipMemcpy(
				tmp_mem + total_received,
				mem->buf_mem + dmabuf_cmsg->frag_offset,
				dmabuf_cmsg->frag_size,
				hipMemcpyDeviceToDevice
			);

			/*
			if (do_validation) {	
				validate_buffer(tmp_mem + total_received,
						dmabuf_cmsg->frag_size, total_received);
			} else {
				print_nonzero_bytes(tmp_mem + total_received,
						    dmabuf_cmsg->frag_size);
			}
			*/

			token.token_start = dmabuf_cmsg->frag_token;
			token.token_count = 1;
			ret = setsockopt(client_fd, SOL_SOCKET,
					 SO_DEVMEM_DONTNEED, &token,
					 sizeof(token));
			if (ret != 1)
				error(1, 0,
				      "SO_DEVMEM_DONTNEED not enough tokens");

			total_received += dmabuf_cmsg->frag_size;

			fprintf(stderr,
				"received frag_page=%10llu, in_page_offset=%10llu, frag_offset=%10p, frag_size=%6u, token=%6u, total_received=%lu, dmabuf_id=%u\n",
				dmabuf_cmsg->frag_offset >> PAGE_SHIFT,
				dmabuf_cmsg->frag_offset % getpagesize(),
				dmabuf_cmsg->frag_offset,
				dmabuf_cmsg->frag_size, dmabuf_cmsg->frag_token,
				total_received, dmabuf_cmsg->dmabuf_id);
		}

		if (!is_devmem)
			error(1, 0, "flow steering error\n");

		// fprintf(stderr, "total_received=%lu\n", total_received);
	}

	fprintf(stderr, "%s: ok\n", TEST_PREFIX);

	fprintf(stderr, "page_aligned_frags=%lu, non_page_aligned_frags=%lu\n",
		page_aligned_frags, non_page_aligned_frags);

cleanup:

	free(tmp_mem);
	close(client_fd);
	close(socket_fd);
	ynl_sock_destroy(ys);

	return 0;
}

void run_devmem_tests(void)
{
	struct memory_buffer *mem;
	struct ynl_sock *ys;

	mem = NULL;

	/* Configure RSS to divert all traffic from our devmem queues */
	if (configure_rss())
		error(1, 0, "rss error\n");

	if (configure_headersplit(1))
		error(1, 0, "Failed to configure header split\n");

	if (!bind_rx_queue(ifindex, mem->fd,
			   calloc(num_queues, sizeof(struct netdev_queue_id)),
			   num_queues, &ys))
		error(1, 0, "Binding empty queues array should have failed\n");

	if (configure_headersplit(0))
		error(1, 0, "Failed to configure header split\n");

	if (!bind_rx_queue(ifindex, mem->fd, create_queues(), num_queues, &ys))
		error(1, 0, "Configure dmabuf with header split off should have failed\n");

	if (configure_headersplit(1))
		error(1, 0, "Failed to configure header split\n");

	if (bind_rx_queue(ifindex, mem->fd, create_queues(), num_queues, &ys))
		error(1, 0, "Failed to bind\n");

	/* Deactivating a bound queue should not be legal */
	if (!configure_channels(num_queues, num_queues - 1))
		error(1, 0, "Deactivating a bound queue should be illegal.\n");

	/* Closing the netlink socket does an implicit unbind */
	ynl_sock_destroy(ys);
}

static uint64_t gettimeofday_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (tv.tv_sec * 1000ULL) + (tv.tv_usec / 1000ULL);
}

static int do_poll(int fd)
{
	struct pollfd pfd;
	int ret;

	pfd.revents = 0;
	pfd.fd = fd;

	ret = poll(&pfd, 1, waittime_ms);
	if (ret == -1)
		error(1, errno, "poll");

	return ret && (pfd.revents & POLLERR);
}

static void wait_compl(int fd)
{
	int64_t tstop = gettimeofday_ms() + waittime_ms;
	char control[CMSG_SPACE(100)] = {};
	struct sock_extended_err *serr;
	struct msghdr msg = {};
	struct cmsghdr *cm;
	__u32 hi, lo;
	int ret;

	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);

	while (gettimeofday_ms() < tstop) {
		if (!do_poll(fd))
			continue;

		ret = recvmsg(fd, &msg, MSG_ERRQUEUE);
		if (ret < 0) {
			if (errno == EAGAIN)
				continue;
			error(1, errno, "recvmsg(MSG_ERRQUEUE)");
			return;
		}
		if (msg.msg_flags & MSG_CTRUNC)
			error(1, 0, "MSG_CTRUNC\n");

		for (cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
			if (cm->cmsg_level != SOL_IP &&
			    cm->cmsg_level != SOL_IPV6)
				continue;
			if (cm->cmsg_level == SOL_IP &&
			    cm->cmsg_type != IP_RECVERR)
				continue;
			if (cm->cmsg_level == SOL_IPV6 &&
			    cm->cmsg_type != IPV6_RECVERR)
				continue;

			serr = (void *)CMSG_DATA(cm);
			if (serr->ee_origin != SO_EE_ORIGIN_ZEROCOPY)
				error(1, 0, "wrong origin %u", serr->ee_origin);
			if (serr->ee_errno != 0)
				error(1, 0, "wrong errno %d", serr->ee_errno);

			hi = serr->ee_data;
			lo = serr->ee_info;

			fprintf(stderr, "tx complete [%d,%d]\n", lo, hi);
			return;
		}
	}

	error(1, 0, "did not receive tx completion");
}

static int do_client(struct memory_buffer *mem)
{
	char ctrl_data[CMSG_SPACE(sizeof(__u32))];
	struct sockaddr_in6 server_sin;
	struct sockaddr_in6 client_sin;
	struct ynl_sock *ys = NULL;
	struct iovec iov[MAX_IOV];
	struct msghdr msg = {};
	ssize_t line_size = 0;
	struct cmsghdr *cmsg;
	char *line = NULL;
	unsigned long mid;
	size_t len = 0;
	int socket_fd;
	__u32 ddmabuf;
	int opt = 1;
	int ret;
	size_t total_sended = 0;

	ret = parse_address(server_ip, atoi(port), &server_sin);
	if (ret < 0)
		error(1, 0, "parse server address");

	socket_fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (socket_fd < 0)
		error(1, socket_fd, "create socket");

	enable_reuseaddr(socket_fd);

	ret = setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE, ifname,
			 strlen(ifname) + 1);
	if (ret)
		error(1, errno, "bindtodevice");

	if (bind_tx_queue(ifindex, mem->fd, &ys))
		error(1, 0, "Failed to bind\n");

	if (client_ip) {
		ret = parse_address(client_ip, atoi(port), &client_sin);
		if (ret < 0)
			error(1, 0, "parse client address");

		ret = bind(socket_fd, &client_sin, sizeof(client_sin));
		if (ret)
			error(1, errno, "bind");
	}

	ret = setsockopt(socket_fd, SOL_SOCKET, SO_ZEROCOPY, &opt, sizeof(opt));
	if (ret)
		error(1, errno, "set sock opt");

	fprintf(stderr, "Connect to %s %d (via %s)\n", server_ip,
		ntohs(server_sin.sin6_port), ifname);

	ret = connect(socket_fd, &server_sin, sizeof(server_sin));
	if (ret)
		error(1, errno, "connect");

	if (do_validation) {
		line = malloc(mem->size);
		for (size_t i = 0; i < mem->size; i++)
			line[i] = i % do_validation;

		line_size = MAX_IOV * max_chunk;
	}

	while (total_sended < mem->size) {
		if (!do_validation) {
			free(line);
			line = NULL;
			line_size = getline(&line, &len, stdin);

			if (line_size < 0)
				break;
		}

		if (total_sended + line_size >= mem->size)
			line_size = mem->size - total_sended;

		if (max_chunk) {
			msg.msg_iovlen = (line_size + max_chunk - 1) / max_chunk;
			if (msg.msg_iovlen > MAX_IOV)
				error(1, 0,
				      "can't partition %zd bytes into maximum of %d chunks",
				      line_size, MAX_IOV);

			for (int i = 0; i < msg.msg_iovlen; i++) {
				iov[i].iov_base = (void *)(i * max_chunk);
				iov[i].iov_len = max_chunk;
			}

			iov[msg.msg_iovlen - 1].iov_len =
				line_size - (msg.msg_iovlen - 1) * max_chunk;
		} else {
			iov[0].iov_base = 0;
			iov[0].iov_len = line_size;
			msg.msg_iovlen = 1;
		}

		msg.msg_iov = iov;
		hipMemcpy(
			mem->buf_mem, line, line_size, hipMemcpyHostToDevice
		);

		msg.msg_control = ctrl_data;
		msg.msg_controllen = sizeof(ctrl_data);

		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_DEVMEM_DMABUF;
		cmsg->cmsg_len = CMSG_LEN(sizeof(__u32));

		ddmabuf = tx_dmabuf_id;

		*((__u32 *)CMSG_DATA(cmsg)) = ddmabuf;

		ret = sendmsg(socket_fd, &msg, MSG_ZEROCOPY);
		if (ret < 0)
			error(1, errno, "Failed sendmsg");

		fprintf(stderr, "sendmsg_ret=%d\n", ret);

		wait_compl(socket_fd);

		total_sended += ret;
	}

	fprintf(stderr, "%s: tx ok\n", TEST_PREFIX);

	free(line);
	close(socket_fd);

	if (ys)
		ynl_sock_destroy(ys);

	return 0;
}

int main(int argc, char *argv[])
{
	struct memory_buffer mem;
	int is_server = 0, opt;
	int ret;

	while ((opt = getopt(argc, argv, "ls:c:p:v:q:t:f:z:")) != -1) {
		switch (opt) {
		case 'l':
			is_server = 1;
			break;
		case 's':
			server_ip = optarg;
			break;
		case 'c':
			client_ip = optarg;
			break;
		case 'p':
			port = optarg;
			break;
		case 'v':
			do_validation = atoll(optarg);
			break;
		case 'q':
			num_queues = atoi(optarg);
			break;
		case 't':
			start_queue = atoi(optarg);
			break;
		case 'f':
			ifname = optarg;
			break;
		case 'z':
			max_chunk = atoi(optarg);
			break;
		case '?':
			fprintf(stderr, "unknown option: %c\n", optopt);
			break;
		}
	}

	if (!ifname)
		error(1, 0, "Missing -f argument\n");

	ifindex = if_nametoindex(ifname);

	fprintf(stderr, "using ifindex=%u\n", ifindex);

	if (!server_ip && !client_ip) {
		if (start_queue < 0 && num_queues < 0) {
			num_queues = rxq_num(ifindex);
			if (num_queues < 0)
				error(1, 0, "couldn't detect number of queues\n");
			if (num_queues < 2)
				error(1, 0,
				      "number of device queues is too low\n");
			/* make sure can bind to multiple queues */
			start_queue = num_queues / 2;
			num_queues /= 2;
		}

		if (start_queue < 0 || num_queues < 0)
			error(1, 0, "Both -t and -q are required\n");

		run_devmem_tests();
		return 0;
	}

	if (start_queue < 0 && num_queues < 0) {
		num_queues = rxq_num(ifindex);
		if (num_queues < 2)
			error(1, 0, "number of device queues is too low\n");

		num_queues = 1;
		start_queue = rxq_num(ifindex) - num_queues;

		if (start_queue < 0)
			error(1, 0, "couldn't detect number of queues\n");

		fprintf(stderr, "using queues %d..%d\n", start_queue, start_queue + num_queues);
	}

	for (; optind < argc; optind++)
		fprintf(stderr, "extra arguments: %s\n", argv[optind]);

	if (start_queue < 0)
		error(1, 0, "Missing -t argument\n");

	if (num_queues < 0)
		error(1, 0, "Missing -q argument\n");

	if (!server_ip)
		error(1, 0, "Missing -s argument\n");

	if (!port)
		error(1, 0, "Missing -p argument\n");

	hipError_t err1;
	err1 = hipMalloc((void **) &mem.buf_mem, getpagesize() * NUM_PAGES);
	// printf("err: %d\n", err1);
	hsa_status_t err2;
	err2 = hsa_amd_portable_export_dmabuf((void *) mem.buf_mem, getpagesize() * NUM_PAGES, &mem.fd, &mem.offset);
	// printf("err: %d\n", err2);
	// printf("offset: %p\n", mem.offset);
	// printf("pagesize: %zu\n", getpagesize());
	mem.size = getpagesize() * NUM_PAGES;
	ret = is_server ? do_server(&mem) : do_client(&mem);

	return ret;
}
