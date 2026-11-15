#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define TIMEOUT_MS 1000

static void usage(const char *prog)
{
    printf("Usage: %s [OPTIONS] -t <targets> -p <ports>\n"
           "\n"
           "Options:\n"
           "  -t, --targets <spec>  targets: single IP (192.168.1.42)\n"
           "                        or IP range (192.168.1.1-192.168.1.255)\n"
           "  -p, --ports <spec>    ports: single port (22) or range (1-1024)\n"
           "  -h, --help            show this help and exit\n"
           "\n"
           "Output: one line per open port, e.g. 192.168.1.42:22 open\n",
           prog);
}

/* Parse "a.b.c.d" or "a.b.c.d-e.f.g.h" into host-order start/end. */
static int parse_targets(const char *spec, uint32_t *start, uint32_t *end)
{
    char buf[64];
    struct in_addr a;
    char *dash;

    if (strlen(spec) >= sizeof(buf))
        return -1;
    strcpy(buf, spec);

    dash = strchr(buf, '-');
    if (dash)
        *dash = '\0';

    if (inet_pton(AF_INET, buf, &a) != 1)
        return -1;
    *start = ntohl(a.s_addr);
    *end = *start;

    if (dash) {
        if (inet_pton(AF_INET, dash + 1, &a) != 1)
            return -1;
        *end = ntohl(a.s_addr);
    }
    return *start <= *end ? 0 : -1;
}

static int parse_port(const char *s, int *port)
{
    char *endp;
    long v;

    errno = 0;
    v = strtol(s, &endp, 10);
    if (errno || endp == s || *endp != '\0' || v < 1 || v > 65535)
        return -1;
    *port = (int)v;
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

    if (parse_port(buf, start) < 0)
        return -1;
    *end = *start;
    if (dash && parse_port(dash + 1, end) < 0)
        return -1;
    return *start <= *end ? 0 : -1;
}

/* Return 1 if a TCP connection to ip:port succeeds, 0 otherwise. */
static int port_open(uint32_t ip, int port)
{
    struct sockaddr_in addr;
    struct timeval tv;
    int fd, ok;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 0;
    }

    /* On Linux SO_SNDTIMEO also limits how long connect() blocks. */
    tv.tv_sec = TIMEOUT_MS / 1000;
    tv.tv_usec = (TIMEOUT_MS % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(ip);

    ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    close(fd);
    return ok;
}

int main(int argc, char *argv[])
{
    static struct option longopts[] = {
        {"targets", required_argument, NULL, 't'},
        {"ports",   required_argument, NULL, 'p'},
        {"help",    no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    const char *targets = NULL, *ports = NULL;
    uint32_t ip_start, ip_end, ip;
    int port_start, port_end, port, c;

    while ((c = getopt_long(argc, argv, "t:p:h", longopts, NULL)) != -1) {
        switch (c) {
        case 't':
            targets = optarg;
            break;
        case 'p':
            ports = optarg;
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

        for (port = port_start; port <= port_end; port++) {
            if (port_open(ip, port)) {
                printf("%s:%d open\n", ipstr, port);
                fflush(stdout);
            }
        }
        if (ip == ip_end)
            break;
    }
    return 0;
}
