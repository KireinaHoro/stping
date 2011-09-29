/* $Id$ */

/*
 * SOCK_STREAM echo ping. This illustrates the following effects:
 *
 * - Dropped packets
 * - Corruption
 * - Out of order responses
 * - Duplicate packets
 *
 * Pending responses are stored in a simple linked list; these are removed
 * either when a response is received, or on timeout. A checksum is included
 * in the packet contents to detect corruption, and a sequence number is used
 * to identify the order of responses.
 *
 * SIGINFO causes current statistics to be written to stderr on the fly. The
 * total statistics are also printed to stderr when pinging is complete.
 *
 * This program (and the associated daemon) targets XPG4.2, not POSIX.
 */

/*
 * TODO: document with a diagram. Examples can be a new section for docs.bp.com
 * TODO: count out-of-order packets
 * TODO: gethostbyname for argv[1]
 * TODO: any other syscalls for EINTR?
 * TODO: select can't predict the future. consider making everything non-blocking
 * TODO: i am ever suspicious about timing; confirm lengths are ok for select() loop.
 * TODO: make timeout configurable
 * TODO: add "don't fragment" option
 * TODO: add packet size option, filled with random data, for stress testing. checksum this, too.
 * TODO: option to dump packet contents, tcpdump style, for visualisation.
 * TODO: don't use stdint.h!
 * TODO: keep going if IP vanishes (e.g. by DHCP); i.e. send() fails
 * TODO: i think the timing is wrong after handling a signal (e.g. SIGINFO)
 * TODO: print the number which are pending in the stats
 */

#ifndef _XOPEN_SOURCE
# define _XOPEN_SOURCE 500
#endif

/* for SIGINFO */
#if defined(__APPLE__)
# define _DARWIN_C_SOURCE
#endif

#ifdef USE_CTHRU
#include <cthru/socket.h>
#endif

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <assert.h>
#include <unistd.h>
#include <float.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <time.h>
#include <math.h>
#include <signal.h>

#include "common.h"

/*
 * Linux defines SIGINFO as "A synonym for SIGPWR" according to signal(7), but
 * does not actually #define it in <signal.h>.
 */
#if defined(__linux__) && !defined(SIGINFO)
# define SIGINFO SIGPWR
#endif

/*
 * Opensolaris has no convention for SIGINFO so we're arbitrarily using SIGUSR1.
 */
#if defined(__sun)
# define SIGINFO SIGUSR1
#endif

extern char *optarg;
extern int optind;

/*
 * The time to timeout pending responses, and the time between pings.
 * Both times are given in milliseconds. Culltime (given in seconds)
 * is the length of time to wait for unanswered pings.
 */
#define TIMEOUT  5.0 * 1000.0
#define INTERVAL 0.5 * 1000.0
#define CULLTIME 6

/* Variables for logging statistics */
unsigned int stat_sent;
unsigned int stat_recieved;
unsigned int stat_timedout;
unsigned int stat_ignored;

double stat_timemax;
double stat_timemin = DBL_MAX;
double stat_timesum;
double stat_timesqr;

/* flags for signal handlers */
volatile sig_atomic_t shouldexit;
volatile sig_atomic_t shouldinfo;


/*
 * A linked-list of unanswered ping requests.
 */
struct pending {
	struct timeval t;
	uint16_t seq;

	struct pending *next;
};

static void
sighandler(int s)
{
	switch (s) {
	case SIGINFO:
		shouldinfo = 1;
		break;

	case SIGINT:
		shouldexit = 1;
		break;

	case SIGALRM: /* handled just for EINTR */
	default:
		return;
	}
}

/*
 * Calculate the difference a given time and the current time; a - b.
 */
static struct timeval
xtimersub(struct timeval *a, struct timeval *b)
{
	struct timeval t;

	t.tv_sec  = a->tv_sec  - b->tv_sec;
	t.tv_usec = a->tv_usec - b->tv_usec;

	if (t.tv_usec < 0) {
		t.tv_sec--;
		t.tv_usec += 1000 * 1000;
	}

	return t;
}

/*
 * Check validity of a timeval structure, adjusting it if necessary. This is
 * provided for convenience of inaccurate arithmetic around the limits of
 * floating point calculations, to permit ping internals at fractions of a
 * second without unnecessarily complex.error-checking around select().
 *
 * Since negative values in this program are only ever produced by floating
 * point inaccuracies, they will always be small (to the order of epsilon or
 * so), and hence are just set to 0, if present.
 *
 * See SUS3 <sys/types.h>'s specification for a discussion of valid values here.
 */
static void
xitimerfix(struct timeval *tv)
{
	assert(tv->tv_usec <= 1000000);

	if (tv->tv_sec < 0) {
		tv->tv_sec = 0;
	}

	if (tv->tv_usec < 0) {
		tv->tv_usec = 0;
	}
}

static struct pending **
findpending(uint16_t seq, struct pending **p)
{
	struct pending **curr;

	for (curr = p; *curr; curr = &(*curr)->next) {
		if ((*curr)->seq == seq) {
			return curr;
		}
	}

	return NULL;
}

/*
 * Convert a timeval struct to milliseconds.
 */
static double
tvtoms(struct timeval *tv)
{
	return tv->tv_usec / 1000.0 + tv->tv_sec * 1000.0;
}

#if __STDC_VERSION__ - 0 < 199901L
int
round(double x)
{
	assert(x >= LONG_MIN - 0.5);
	assert(x <= LONG_MAX + 0.5);

	if (x >= 0) {
		return (long) (x + 0.5);
	} else { 
		return (long) (x - 0.5);
	}
}
#endif

static struct timeval
mstotv(double ms) {
	struct timeval tv;

	tv.tv_sec  = round(ms / 1000.0);
	tv.tv_usec = round(fmod(ms, 1000.0) * 1000.0);
	return tv;
}

static void
removepending(struct pending **p)
{
	struct pending *tmp;

	tmp = *p;
	*p = (*p)->next;
	free(tmp);
}

static int
sendecho(int s, struct pending **p, uint16_t seq)
{
	const char *buf;
	size_t len;

	buf = mkping(seq);
	len = strlen(buf);

	while (len > 0) {
		ssize_t r;

		r = send(s, buf, len, 0);
		if (r == -1) {
			switch (errno) {
			case ENOBUFS:
			case EINTR:
				continue;

			default:
				perror("send");
				return -1;
			}
		}

		assert(r >= 0);
		assert(r <= len);

		len -= r;
		buf += r;
	}

	stat_sent++;

	/* Add this request to the list of pings pending responses */
	{
		struct pending *new;

		new = malloc(sizeof *new);
		if (NULL == new) {
			perror("malloc");
			exit(EXIT_FAILURE);
		}

		if (-1 == gettimeofday(&new->t, NULL)) {
			perror("gettimeofday");
			exit(EXIT_FAILURE);
		}

		new->next = *p;
		new->seq  = seq;

		*p = new;
	}

	return 0;
}

static int
recvecho(int s, struct pending **p, struct sockaddr_in *sin)
{
	char buf[3 + 5 + 24 + 2];
	struct pending **curr;
	uint16_t seq;
	size_t len;

	assert(s != -1);
	assert(p != NULL);
	assert(sin != NULL);

	len = sizeof buf - 1;

	while (len > 0) {
		ssize_t r;

		r = recv(s, buf + (sizeof buf - 1 - len), len, 0);

		if (r == -1) {
			switch (errno) {
			case EINTR:
				continue;

			default:
				perror("recv");
				return -1;
			}
		}

		if (r == 0) {
			shouldexit = 1;
			return -1;
		}

		assert(r >= 1);
		assert(r <= len);

		len -= r;
	}

	buf[sizeof buf - 1] = '\0';

	stat_recieved++;

	if (1 != validate(buf, &seq)) {
		stat_ignored++;
		return 0;
	}

	curr = findpending(seq, p);
	if (curr == NULL) {
		fprintf(stderr, "disregarding: sequence %d not pending response\n", seq);
		stat_ignored++;
		return 0;
	}

	/* Calculate round-trip delta for this particular seq ID */
	{
		struct timeval now, dtv;
		double d;

		if (-1 == gettimeofday(&now, NULL)) {
			perror("gettimeofday");
			exit(EXIT_FAILURE);
		}

		dtv = xtimersub(&now, &(*curr)->t);
		d = tvtoms(&dtv);
		assert(d >= 0);

		printf("%d bytes from %s seq=%d time=%.3f ms\n",
			(int) strlen(buf), inet_ntoa(sin->sin_addr), (int) seq, d);

		stat_timesum += d;
		stat_timesqr += pow(d, 2);
		if (d < stat_timemin) {
			stat_timemin = d;
		}
		if (d > stat_timemax) {
			stat_timemax = d;
		}
	}

	removepending(curr);

	return 0;
}

/*
 * Cull pending packets older than TIMEOUT seconds.
 */
static void
culltimeouts(struct pending **p)
{
	struct pending **curr;
	struct pending **next;
	struct timeval now;

	if (-1 == gettimeofday(&now, NULL)) {
		perror("gettimeofday");
		exit(EXIT_FAILURE);
	}

	for (curr = p; *curr; curr = next) {
		struct timeval dtv;
		double d;

		dtv = xtimersub(&now, &(*curr)->t);
		d = tvtoms(&dtv);
		if (d > TIMEOUT) {
			stat_timedout++;
			printf("timeout: seq=%d time=%.3f ms\n", (*curr)->seq, d);
			removepending(curr);
			next = curr;
		} else {
			next = &(*curr)->next;
		}
	}
}

static void
printstats(FILE *f, int multiline)
{
	double avg;
	double variance;

	assert(f != NULL);

	fprintf(f, multiline ? "%u transmitted, "
	                       "%u received, "
	                       "%u timed out, "
	                       "%u disregarded, "
	                       "%.1f%% packet loss"
	                     : "%u/%u packets, "
	                       "%u timed out, "
	                       "%u disregarded, "
	                       "%.1f%% loss",
		stat_sent, stat_recieved, stat_timedout, stat_ignored,
		(stat_sent - stat_recieved) * 100.0 / stat_sent);

	if (stat_recieved == 0) {
		fprintf(f, "\n");
		return;
	}

	fprintf(f, multiline ? "\n"
	                       "round-trip "
	                     : ", ");

	/* Calculate statistics */
	avg = stat_timesum / stat_recieved;

	if (stat_recieved == 1) {
		fprintf(f, "min/avg/max = "
			   "%.3f/%.3f/%.3f\n",
			stat_timemin, avg, stat_timemax);
	} else {
		variance = (stat_timesqr - stat_recieved * pow(avg, 2))
			/ (stat_recieved - 1);

		fprintf(f, "min/avg/max/stddev = "
			   "%.3f/%.3f/%.3f/%.3f ms\n",
			stat_timemin, avg, stat_timemax, sqrt(variance));
	}
}

static void
usage(void) {
	fprintf(stderr, "usage: stping [ -c <count> ] [ -i interval ] "
		"<address> <port>\n");
}

int
main(int argc, char **argv)
{
	int s;
	int count;
	uint16_t seq;
	struct pending *p;
	struct sockaddr_in sin;
	struct sigaction sigact;
	sigset_t set;
	double interval;

	sigemptyset(&set);
	(void) sigaddset(&set, SIGINT);
	(void) sigaddset(&set, SIGINFO);
	(void) sigaddset(&set, SIGALRM);

	sigact.sa_handler = sighandler;
	sigact.sa_mask    = set;
	sigact.sa_flags   = 0;

	/* defaults */
	interval = INTERVAL;

	/* Handle CLI options */
	count = 0;
	{
		int c;

		while ((c = getopt(argc, argv, "hc:i:")) != -1) {
			switch (c) {
			case 'c':
				count = atoi(optarg);
				if (count <= 0) {
					fprintf(stderr, "Invalid ping count\n");
					return EXIT_FAILURE;
				}
				break;

			case 'i':
				interval = atof(optarg) * 1000.0;
				if (interval < DBL_EPSILON) {
					fprintf(stderr, "Invalid ping interval\n");
					return EXIT_FAILURE;
				}
				break;

			case '?':
			case 'h':
			default:
				usage();
				return EXIT_FAILURE;
			}
		}
		argc -= optind;
		argv += optind;
	}

	if (2 != argc) {
		usage();
		return EXIT_FAILURE;
	}

	s = getaddr(argv[0], argv[1], &sin);
	if (-1 == s) {
		return EXIT_FAILURE;
	}

	if (-1 == connect(s, (void *) &sin, sizeof sin)) {
		perror("connect");
		return EXIT_FAILURE;
	}

	if (0 != setvbuf(stdout, NULL, _IOLBF, 0)) {
		perror("setvbuf");
		return EXIT_FAILURE;
	}

	if (0 != setvbuf(stderr, NULL, _IOLBF, 0)) {
		perror("setvbuf");
		return EXIT_FAILURE;
	}

	if (-1 == sigaction(SIGINFO, &sigact, NULL)) {
		perror("sigaction");
		return EXIT_FAILURE;
	}

	if (-1 == sigaction(SIGINT, &sigact, NULL)) {
		perror("sigaction");
		return EXIT_FAILURE;
	}

	p = NULL;
	for (seq = 0; !shouldexit; seq++) {
		struct timeval t;
		int r;

		if (-1 == sendecho(s, &p, seq)) {
			break;
		}


		/*
		 * This loop is responsible for two things: delaying for 'interval',
		 * whilst dealing with any incoming responses as and when they appear.
		 * The latter must be as timely as possible, so it may interrupt the
		 * interval delay.
		 *
		 * Once the delay is complete, a new ping is sent.
		 */

		t = mstotv(interval);
		xitimerfix(&t);

		r = -1;

		while (!shouldexit && r != 0) {
			struct timeval before, after;
			fd_set rfds;

			if (shouldinfo) {
				printstats(stderr, 0);
				shouldinfo = 0;
			}

			if (-1 == gettimeofday(&before, NULL)) {
				perror("gettimeofday");
				exit(EXIT_FAILURE);
			}

			FD_ZERO(&rfds);
			FD_SET(s, &rfds);

			r = select(s + 1, &rfds, NULL, NULL, &t);
			switch (r) {
			case 0:
				/* interval reached */
				continue;

			case -1:
				break;

			default:
				/* handle activity */
				if (FD_ISSET(s, &rfds)) {
					if (-1 == recvecho(s, &p, &sin)) {
						continue;
					}
				}

				if (-1 == gettimeofday(&after, NULL)) {
					perror("gettimeofday");
					exit(EXIT_FAILURE);
				}

				{
					struct timeval elapsed;

					elapsed = xtimersub(&after, &before);
					t = mstotv(interval);
					t = xtimersub(&t, &elapsed);
					xitimerfix(&t);
				}

				continue;
			}

			switch (errno) {
			case EINTR:
				continue;

			default:
				perror("select");
				return EXIT_FAILURE;
			}
		}

		culltimeouts(&p);

		if (count != 0 && seq + 1 >= count) {
			break;
		}
	}

	/* XXX: race */
	shouldexit = 0;

	/*
	 * Continue waiting for any pending responses, until either they remain or
	 * timeout. In either case, the pending queue becomes empty. If no pending
	 * responses arrive, the alarm() call provides a timeout to exit.
	 */
	if (-1 == sigaction(SIGALRM, &sigact, NULL)) {
		perror("sigaction");
		return EXIT_FAILURE;
	}

	if (-1 == alarm(CULLTIME)) {
		perror("alarm");
		return EXIT_FAILURE;
	}

	while (!shouldexit && p != NULL) {
		(void) recvecho(s, &p, &sin);

		culltimeouts(&p);
	}

	close(s);

	fprintf(stdout, "\n- STREAM Ping Statistics -\n");
	printstats(stdout, 1);

	return EXIT_SUCCESS;
}

