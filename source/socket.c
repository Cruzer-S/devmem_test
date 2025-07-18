#include "socket.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <unistd.h>

#include <arpa/inet.h>

#include "logger.h"

#define ERR(...) do {		\
	log(__VA_ARGS__);	\
	exit(EXIT_FAILURE);	\
} while (true)

#define BACKLOG		15

static struct sockaddr_in sockaddr;
int sockfd;

static void socket_reuseaddr(int fd)
{
	int ret, opt;

	opt = 1;
	ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	if (ret == -1)
		ERR(PERRN, "failed to setsockopt(): ");

	opt = 1;
	ret = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
	if (ret == -1)
		ERR(PERRN, "failed to setsockup(): ");
}

void socket_create(char *address, int port, bool is_server)
{
	int ret;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd == -1)
		ERR(PERRN, "failed to socket(): ");

	memset(&sockaddr, 0x00, sizeof(struct sockaddr_in));
	sockaddr.sin_family = AF_INET;
	sockaddr.sin_port = htons(port);
	sockaddr.sin_addr.s_addr = inet_addr(address);

	if (is_server) {
		socket_reuseaddr(sockfd);

		ret = bind(sockfd, (struct sockaddr *) &sockaddr,
			   sizeof(struct sockaddr_in));
		if (ret == -1)
			ERR(PERRN, "failed to bind(): ");

		if (listen(sockfd, BACKLOG) == -1)
			ERR(PERRN, "failed to listen(): ");
	}
}

void socket_connect(void)
{
	int ret;

	ret = connect(sockfd, (struct sockaddr *) &sockaddr,
		      sizeof(struct sockaddr_in));
	if (ret == -1)
		ERR(PERRN, "failed to connect(): ");
}

int socket_accept(void)
{
	return accept(sockfd, NULL, 0);
}

void socket_destroy(void)
{
	close(sockfd);
}
