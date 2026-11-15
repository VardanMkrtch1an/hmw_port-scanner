#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define DEFAULT_JOBS     100
#define MAX_JOBS         1000
#define DEFAULT_TIMEOUT  1000
#define MAX_TIMEOUT      60000

/* One connect attempt in progress. */
struct slot {
    int fd;
    int port;
    long long deadline;   /* ms, monotonic clock */
};

static void usage(const char *prog)
{
    printf("Usage: %s [OPTIONS] -t <targets> -p <ports>\n"
           "\n"
           "Options:\n"
           "  -t, --targets <spec>  targets: single IP (192.168.1.42),\n"
           "                        IP range (192.168.1.1-192.168.1.255)\n"
           "                        or hostname (example.com)\n"
           "  -p, --ports <spec>    ports: single port (22) or range (1-1024)\n"
           "  -j, --jobs <n>        max simultaneous connect attempts\n"
           "                        (1-%d, default %d)\n"
           "  -w, --timeout <ms>    connection timeout in milliseconds\n"
           "                        (1-%d, default %d)\n"
           "  -h, --help            show this help and exit\n"
           "\n"
           "Output: one line per open port, e.g. 192.168.1.42:22 open\n",
           prog, MAX_JOBS, DEFAULT_JOBS, MAX_TIMEOUT, DEFAULT_TIMEOUT);
}

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Parse a decimal integer in [min, max]. */
static int parse_int(const char *s, int min, int max, int *out)
{
    char *endp;
    long v;

    errno = 0;
    v = strtol(s, &endp, 10);
    if (errno || endp == s || *endp != '\0' || v < min || v > max)
        return -1;
    *out = (int)v;
    return 0;
}

/* Parse an IPv4 address into host order. */
static int parse_ip(const char *s, uint32_t *ip)
{
    struct in_addr a;

    if (inet_pton(AF_INET, s, &a) != 1)
        return -1;
    *ip = ntohl(a.s_addr);
    return 0;
}

/* Resolve a hostname to its first IPv4 address (host order). */
static int resolve_host(const char *name, uint32_t *ip)
{
    struct addrinfo hints, *res;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(name, NULL, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "error: cannot resolve '%s': %s\n",
                name, gai_strerror(rc));
        return -1;
    }
    *ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    return 0;
}

/*
 * Parse "a.b.c.d", "a.b.c.d-e.f.g.h" or a hostname into a host-order
 * start/end range. A hostname may contain '-', so it is tried last.
 */
static int parse_targets(const char *spec, uint32_t *start, uint32_t *end)
{
    char buf[256];
    char *dash;

    if (parse_ip(spec, start) == 0) {
        *end = *start;
        return 0;
    }

    if (strlen(spec) >= sizeof(buf))
        return -1;
    strcpy(buf, spec);
    dash = strchr(buf, '-');
    if (dash) {
        *dash = '\0';
        if (parse_ip(buf, start) == 0 && parse_ip(dash + 1, end) == 0)
            return *start <= *end ? 0 : -1;
    }

    if (resolve_host(spec, start) < 0)
        return -1;
    *end = *start;
    return 0;
}

/* Parse "22" or "1-1024". */
static int parse_ports(const char *spec, int *start, int *end)
{
    char buf[32];
    char *dash;

    if (strlen(spec) >= sizeof(buf))
        return -1;
    strcpy(buf, spec);

    dash = strchr(buf, '-');
    if (dash)
        *dash = '\0';

    if (parse_int(buf, 1, 65535, start) < 0)
        return -1;
    *end = *start;
    if (dash && parse_int(dash + 1, 1, 65535, end) < 0)
        return -1;
    return *start <= *end ? 0 : -1;
}

/*
 * Start a non-blocking connect to ip:port.
 * Returns 1 if it connected at once, 0 if it failed at once,
 * or -1 if it is in progress (*fd_out is then the socket).
 */
static int start_connect(uint32_t ip, int port, int *fd_out)
{
    struct sockaddr_in addr;
    int fd, flags;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 0;
    }
    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(ip);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        close(fd);
        return 1;
    }
    if (errno == EINPROGRESS) {
        *fd_out = fd;
        return -1;
    }
    close(fd);
    return 0;
}

/*
 * Scan ports [pstart, pend] on ip with up to jobs connects at once.
 * Sets open[port] = 1 for each open port.
 */
static void scan_target(uint32_t ip, int pstart, int pend, int jobs,
                        int timeout_ms, unsigned char *open)
{
    struct slot *slots = calloc(jobs, sizeof(*slots));
    struct pollfd *pfds = calloc(jobs, sizeof(*pfds));
    int next = pstart, active = 0, i, rc, fd;

    if (!slots || !pfds) {
        perror("calloc");
        exit(1);
    }

    while (next <= pend || active > 0) {
        long long now, wait;

        /* Fill free slots with new connect attempts. */
        while (active < jobs && next <= pend) {
            rc = start_connect(ip, next, &fd);
            if (rc == 1) {
                open[next] = 1;
            } else if (rc == -1) {
                slots[active].fd = fd;
                slots[active].port = next;
                slots[active].deadline = now_ms() + timeout_ms;
                active++;
            }
            next++;
        }
        if (active == 0)
            continue;

        /* Wait until a socket is ready or the earliest deadline. */
        now = now_ms();
        wait = slots[0].deadline - now;
        for (i = 0; i < active; i++) {
            pfds[i].fd = slots[i].fd;
            pfds[i].events = POLLOUT;
            pfds[i].revents = 0;
            if (slots[i].deadline - now < wait)
                wait = slots[i].deadline - now;
        }
        if (wait < 0)
            wait = 0;

        rc = poll(pfds, active, (int)wait);
        if (rc < 0 && errno != EINTR) {
            perror("poll");
            exit(1);
        }

        /* Collect finished and timed-out attempts (backwards, so that
         * moving the last slot into a freed one is safe). */
        now = now_ms();
        for (i = active - 1; i >= 0; i--) {
            int done = 0;

            if (rc > 0 && pfds[i].revents) {
                int err = 0;
                socklen_t len = sizeof(err);

                getsockopt(slots[i].fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0)
                    open[slots[i].port] = 1;
                done = 1;
            } else if (now >= slots[i].deadline) {
                done = 1;   /* no answer: filtered */
            }

            if (done) {
                close(slots[i].fd);
                slots[i] = slots[--active];
            }
        }
    }

    free(slots);
    free(pfds);
}

int main(int argc, char *argv[])
{
    static struct option longopts[] = {
        {"targets", required_argument, NULL, 't'},
        {"ports",   required_argument, NULL, 'p'},
        {"jobs",    required_argument, NULL, 'j'},
        {"timeout", required_argument, NULL, 'w'},
        {"help",    no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    const char *targets = NULL, *ports = NULL;
    int jobs = DEFAULT_JOBS, timeout_ms = DEFAULT_TIMEOUT;
    uint32_t ip_start, ip_end, ip;
    int port_start, port_end, port, c;
    static unsigned char open[65536];

    while ((c = getopt_long(argc, argv, "t:p:j:w:h", longopts, NULL)) != -1) {
        switch (c) {
        case 't':
            targets = optarg;
            break;
        case 'p':
            ports = optarg;
            break;
        case 'j':
            if (parse_int(optarg, 1, MAX_JOBS, &jobs) < 0) {
                fprintf(stderr, "error: invalid jobs '%s' (1-%d)\n",
                        optarg, MAX_JOBS);
                return 1;
            }
            break;
        case 'w':
            if (parse_int(optarg, 1, MAX_TIMEOUT, &timeout_ms) < 0) {
                fprintf(stderr, "error: invalid timeout '%s' (1-%d ms)\n",
                        optarg, MAX_TIMEOUT);
                return 1;
            }
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (!targets || !ports) {
        fprintf(stderr, "error: -t and -p are required\n");
        usage(argv[0]);
        return 1;
    }
    if (parse_targets(targets, &ip_start, &ip_end) < 0) {
        fprintf(stderr, "error: invalid targets '%s'\n", targets);
        return 1;
    }
    if (parse_ports(ports, &port_start, &port_end) < 0) {
        fprintf(stderr, "error: invalid ports '%s'\n", ports);
        return 1;
    }

    for (ip = ip_start;; ip++) {
        struct in_addr a;
        char ipstr[INET_ADDRSTRLEN];

        a.s_addr = htonl(ip);
        inet_ntop(AF_INET, &a, ipstr, sizeof(ipstr));

        memset(open, 0, sizeof(open));
        scan_target(ip, port_start, port_end, jobs, timeout_ms, open);

        for (port = port_start; port <= port_end; port++)
            if (open[port])
                printf("%s:%d open\n", ipstr, port);
        fflush(stdout);

        if (ip == ip_end)
            break;
    }
    return 0;
}
