/*
 * SOCK_DGRAM echo ping daemon.
 *
 * Ping repsonses are sent back to the source port of the ping client.
 */

#define _GNU_SOURCE

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "common.h"

static int
bindon(int s, struct sockaddr_in *sin)
{
	const int ov = 1;

	if (-1 == setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &ov, sizeof ov)) {
		perror("setsockopt");
		close(s);
		return -1;
	}

	if (-1 == bind(s, (void *) sin, sizeof *sin)) {
		perror("bind");
		close(s);
		return -1;
	}

	return s;
}

static int
recvecho(int s, uint16_t *seq, struct sockaddr_in *sin, socklen_t sinsz, int quiet)
{
	char buf[DEFAULT_PAYLOAD + MAX_EXTRA_PAYLOAD];
	ssize_t r;
	int payload_len;

	r = recvfrom(s, buf, sizeof buf, 0, (void *) sin, &sinsz);
	if (-1 == r) {
		perror("recvfrom");
		return -1;
	}

	if (1 != validate(buf, seq)) {
		return -1;
	}

	payload_len = strlen(buf) + 1;
	if (!quiet)
		printf("%d bytes from %s seq=%d\n", payload_len, inet_ntoa(sin->sin_addr), *seq);
	return payload_len - DEFAULT_PAYLOAD;
}

static void
sendecho(int s, uint16_t seq, struct sockaddr_in *sin, int extra_payload)
{
	const char *buf;

	buf = mkping(seq, extra_payload);

	if (-1 == sendto(s, buf, strlen(buf) + 1, 0, (void *) sin, sizeof *sin)) {
		perror("sendto");
	}
}

int
main(int argc, char *argv[])
{
	int s;
	struct sockaddr_in sin;
	int quiet;
	int payload_len;

	if (3 != argc && 4 != argc) {
		fprintf(stderr, "usage: dgpingd <address> <port> [quiet]\n");
		return EXIT_FAILURE;
	}

	quiet = argc == 4;

	s = getaddr(argv[1], argv[2], &sin, SOCK_DGRAM, IPPROTO_UDP);
	if (-1 == s) {
		return EXIT_FAILURE;
	}

	/* TODO bind on INADDR_ANY instead? We could broadcast pings by default. */
	s = bindon(s, &sin);
	if (-1 == s) {
		fprintf(stderr, "unable to listen\n");
		return EXIT_FAILURE;
	}

	if (0 != setvbuf(stdout, NULL, _IOLBF, 0)) {
		perror("setvbuf");
		return EXIT_FAILURE;
	}

	/* TODO find "UDP" automatically */
	printf("listening on %s:%s %s\n", argv[1], argv[2], "UDP/IP");

	for (;;) {
		uint16_t seq;

		if ((payload_len = recvecho(s, &seq, &sin, sizeof sin, quiet)) >= 0) {
			sendecho(s, seq, &sin, payload_len);
		}
	}

	/* NOTREACHED */

	return EXIT_SUCCESS;
}

