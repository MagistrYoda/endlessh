#define _POSIX_C_SOURCE  200112L
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <poll.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define DEFAULT_PORT              2222
#define DEFAULT_DELAY            10000  /* milliseconds */
#define DEFAULT_MAX_LINE_LENGTH     32
#define DEFAULT_MAX_CLIENTS       4096
#define DEFAULT_CONFIG_FILE       "/etc/endlessh/config"

#define XSTR(s) STR(s)
#define STR(s) #s

static long long
uepoch(void)
{
    struct timespec tv;
    clock_gettime(CLOCK_REALTIME, &tv);
    return tv.tv_sec * 1000ULL + tv.tv_nsec / 1000000ULL;
}

static enum loglevel {
    LOG_NONE,
    LOG_INFO,
    LOG_DEBUG
} loglevel = LOG_NONE;

static void
logmsg(enum loglevel level, const char *format, ...)
{
    if (loglevel >= level) {
        long long now = uepoch();
        printf("%lld.%03lld: ", now / 1000, now % 1000);
        va_list ap;
        va_start(ap, format);
        vprintf(format, ap);
        va_end(ap);
        fputc('\n', stdout);
    }
}

struct client {
    char ipaddr[INET6_ADDRSTRLEN];
    long long connect_time;
    long long send_next;
    long long bytes_sent;
    struct client *next;
    int port;
    int fd;
};

static struct client *
client_new(int fd, long long send_next)
{
    struct client *c = malloc(sizeof(*c));
    if (c) {
        c->connect_time = uepoch();
        c->send_next = send_next;
        c->bytes_sent = 0;
        c->next = 0;
        c->fd = fd;

        /* Get IP address */
        struct sockaddr_storage addr;
        socklen_t len = sizeof(addr);
        if (getpeername(fd, (struct sockaddr*)&addr, &len) == -1) {
            c->ipaddr[0] = 0;
        } else {
            if (addr.ss_family == AF_INET) {
                struct sockaddr_in *s = (struct sockaddr_in *)&addr;
                c->port = ntohs(s->sin_port);
                inet_ntop(AF_INET, &s->sin_addr,
                          c->ipaddr, sizeof(c->ipaddr));
            } else {
                struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
                c->port = ntohs(s->sin6_port);
                inet_ntop(AF_INET6, &s->sin6_addr,
                          c->ipaddr, sizeof(c->ipaddr));
            }
        }
    }
    return c;
}

static void
client_destroy(struct client *client)
{
    logmsg(LOG_DEBUG, "close(%d)", client->fd);
    long long dt = uepoch() - client->connect_time;
    logmsg(LOG_INFO,
            "CLOSE host=%s:%d fd=%d "
            "time=%lld.%03lld bytes=%lld",
            client->ipaddr, client->port, client->fd,
            dt / 1000, dt % 1000,
            client->bytes_sent);
    close(client->fd);
    free(client);
}

struct queue {
    struct client *head;
    struct client *tail;
};

static void
queue_init(struct queue *q)
{
    q->head = q->tail = 0;
}

static struct client *
queue_remove(struct queue *q, int fd)
{
    /* Yes, this is a linear search, but the element we're looking for
     * is virtually always one of the first few elements.
     */
    struct client *c;
    struct client **prev = &q->head;
    for (c = q->head; c; prev = &c->next, c = c->next) {
        if (c->fd == fd) {
            if (q->tail == c)
                q->tail = 0;
            *prev = c->next;
            c->next = 0;
            break;
        }
    }
    return c;
}

static void
queue_append(struct queue *q, struct client *c)
{
    if (!q->tail) {
        q->head = q->tail = c;
    } else {
        q->tail->next = c;
        q->tail = c;
    }
}

static void
queue_destroy(struct queue *q)
{
    struct client *c = q->head;
    while (c) {
        struct client *dead = c;
        c = c->next;
        client_destroy(dead);
    }
    q->head = q->tail = 0;
}

struct pollvec {
    size_t cap;
    size_t fill;
    struct pollfd *fds;
};

static void
pollvec_init(struct pollvec *v)
{
    v->cap = 4;
    v->fill = 0;
    v->fds = malloc(v->cap * sizeof(v->fds[0]));
    if (!v->fds) {
        fprintf(stderr, "endlessh: pollvec initialization failed\n");
        exit(EXIT_FAILURE);
    }
}

static void
pollvec_clear(struct pollvec *v)
{
    v->fill = 0;
}

static int
pollvec_push(struct pollvec *v, int fd, short events)
{
    if (v->cap == v->fill) {
        size_t size = v->cap * 2 * sizeof(v->fds[0]);
        if (size < v->cap * sizeof(v->fds[0]))
            return 0; // overflow
        struct pollfd *grow = realloc(v->fds, size);
        if (!grow)
            return 0;
        v->fds = grow;
        v->cap *= 2;
    }
    v->fds[v->fill].fd = fd;
    v->fds[v->fill].events = events;
    v->fill++;
    return 1;
}

static void
pollvec_free(struct pollvec *v)
{
    free(v->fds);
    v->fds = 0;
}

static void
check(int r)
{
    if (r == -1) {
        fprintf(stderr, "endlessh: fatal: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

static int
randline(char *line, int maxlen)
{
    int len = 3 + rand() % (maxlen - 2);
    for (int i = 0; i < len - 2; i++)
        line[i] = 32 + rand() % 95;
    line[len - 2] = 13;
    line[len - 1] = 10;
    if (memcmp(line, "SSH-", 4) == 0)
        line[0] = 'X';
    return len;
}

static volatile sig_atomic_t running = 1;

static void
sigterm_handler(int signal)
{
    (void)signal;
    running = 0;
}

static volatile sig_atomic_t reload = 0;

static void
sighup_handler(int signal)
{
    (void)signal;
    reload = 1;
}

struct config {
    int port;
    int delay;
    int max_line_length;
    int max_clients;
};

#define CONFIG_DEFAULT { \
    .port            = DEFAULT_PORT, \
    .delay           = DEFAULT_DELAY, \
    .max_line_length = DEFAULT_MAX_LINE_LENGTH, \
    .max_clients     = DEFAULT_MAX_CLIENTS, \
}

static void
config_set_port(struct config *c, const char *s, int hardfail)
{
    errno = 0;
    char *end;
    long tmp = strtol(s, &end, 10);
    if (errno || *end || tmp < 1 || tmp > 65535) {
        fprintf(stderr, "endlessh: Invalid port: %s\n", s);
        if (hardfail)
            exit(EXIT_FAILURE);
    } else {
        c->port = tmp;
    }
}

static void
config_set_delay(struct config *c, const char *s, int hardfail)
{
    errno = 0;
    char *end;
    long tmp = strtol(s, &end, 10);
    if (errno || *end || tmp < 1 || tmp > INT_MAX) {
        fprintf(stderr, "endlessh: Invalid delay: %s\n", s);
        if (hardfail)
            exit(EXIT_FAILURE);
    } else {
        c->delay = tmp;
    }
}

static void
config_set_max_clients(struct config *c, const char *s, int hardfail)
{
    errno = 0;
    char *end;
    long tmp = strtol(s, &end, 10);
    if (errno || *end || tmp < 1 || tmp > INT_MAX) {
        fprintf(stderr, "endlessh: Invalid max clients: %s\n", s);
        if (hardfail)
            exit(EXIT_FAILURE);
    } else {
        c->max_clients = tmp;
    }
}

static void
config_set_max_line_length(struct config *c, const char *s, int hardfail)
{
    errno = 0;
    char *end;
    long tmp = strtol(s, &end, 10);
    if (errno || *end || tmp < 3 || tmp > 255) {
        fprintf(stderr, "endlessh: Invalid line length: %s\n", s);
        if (hardfail)
            exit(EXIT_FAILURE);
    } else {
        c->max_line_length = tmp;
    }
}

static void
config_fail(const char *file, long lineno, int hardfail)
{
    fprintf(stderr, "%s:%ld: Expected integer\n", file, lineno);
    if (hardfail) exit(EXIT_FAILURE);
}

static void
config_load(struct config *c, const char *file, int hardfail)
{
    long lineno = 0;
    FILE *f = fopen(file, "r");
    if (f) {
        const char *delim = " \n";
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            lineno++;
            char *save = 0;
            char *tok = strtok_r(line, delim, &save);
            if (!tok) {
                continue;
            } else if (!strcmp(tok, "Port")) {
                tok = strtok_r(0, delim, &save);
                if (tok) {
                    config_set_port(c, tok, hardfail);
                } else {
                    config_fail(file, lineno, hardfail);
                }
            } else if (!strcmp(tok, "Delay")) {
                tok = strtok_r(0, delim, &save);
                if (tok) {
                    config_set_delay(c, tok, hardfail);
                } else {
                    config_fail(file, lineno, hardfail);
                }
            } else if (!strcmp(tok, "MaxLineLength")) {
                tok = strtok_r(0, delim, &save);
                if (tok) {
                    config_set_max_line_length(c, tok, hardfail);
                } else {
                    config_fail(file, lineno, hardfail);
                }
            } else if (!strcmp(tok, "MaxClients")) {
                tok = strtok_r(0, delim, &save);
                if (tok) {
                    config_set_max_line_length(c, tok, hardfail);
                } else {
                    config_fail(file, lineno, hardfail);
                }
            } else {
                fprintf(stderr, "%s:%ld: Unknown option '%s'\n",
                        file, lineno, tok);
            }
        }
        fclose(f);
    }
}

static void
config_log(const struct config *c)
{
    logmsg(LOG_INFO, "Port %d", c->port);
    logmsg(LOG_INFO, "Delay %ld", c->delay);
    logmsg(LOG_INFO, "MaxLineLength %d", c->max_line_length);
    logmsg(LOG_INFO, "MaxClients %d", c->max_clients);
}

static void
usage(FILE *f)
{
    fprintf(f, "Usage: endlessh [-vh] [-d MS] [-f CONFIG] [-l LEN] "
                               "[-m LIMIT] [-p PORT]\n");
    fprintf(f, "  -d INT    Message millisecond delay ["
            XSTR(DEFAULT_DELAY) "]\n");
    fprintf(f, "  -f        Set config file ["
            DEFAULT_CONFIG_FILE "]\n");
    fprintf(f, "  -h        Print this help message and exit\n");
    fprintf(f, "  -l INT    Maximum banner line length (3-255) ["
            XSTR(DEFAULT_MAX_LINE_LENGTH) "]\n");
    fprintf(f, "  -m INT    Maximum number of clients ["
            XSTR(DEFAULT_MAX_CLIENTS) "]\n");
    fprintf(f, "  -p INT    Listening port [" XSTR(DEFAULT_PORT) "]\n");
    fprintf(f, "  -v        Print diagnostics to standard output "
            "(repeatable)\n");
}

int
main(int argc, char **argv)
{
    struct config config = CONFIG_DEFAULT;
    const char *config_file = DEFAULT_CONFIG_FILE;
    config_load(&config, config_file, 1);

    int option;
    while ((option = getopt(argc, argv, "d:f:hl:m:p:v")) != -1) {
        switch (option) {
            case 'd':
                config_set_delay(&config, optarg, 1);
                break;
            case 'f':
                config_file = optarg;
                config_load(&config, optarg, 1);
                break;
            case 'h':
                usage(stdout);
                exit(EXIT_SUCCESS);
                break;
            case 'l':
                config_set_max_line_length(&config, optarg, 1);
                break;
            case 'm':
                config_set_max_clients(&config, optarg, 1);
                break;
            case 'p':
                config_set_port(&config, optarg, 1);
                break;
            case 'v':
                if (!loglevel++)
                    setvbuf(stdout, 0, _IOLBF, 0);
                break;
            default:
                usage(stderr);
                exit(EXIT_FAILURE);
        }
    }

    /* Log configuration */
    config_log(&config);

    {
        struct sigaction sa = {.sa_handler = sigterm_handler};
        check(sigaction(SIGTERM, &sa, 0));
    }

    {
        struct sigaction sa = {.sa_handler = sighup_handler};
        check(sigaction(SIGHUP, &sa, 0));
    }

    int nclients = 0;

    struct queue queue[1];
    queue_init(queue);

    struct pollvec pollvec[1];
    pollvec_init(pollvec);

    int server = socket(AF_INET, SOCK_STREAM, 0);
    check(server);

    int dummy = 1;
    check(setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &dummy, sizeof(dummy)));

    struct sockaddr_in addr = {
        AF_INET,
        htons(config.port),
        {htonl(INADDR_ANY)}
    };
    check(bind(server, (void *)&addr, sizeof(addr)));
    check(listen(server, INT_MAX));

    logmsg(LOG_DEBUG, "listen(port=%d)", config.port);

    srand(time(0));
    while (running) {
        if (reload) {
            /* Configuration reload requested (SIGHUP) */
            config_load(&config, config_file, 0);
            config_log(&config);
            reload = 0;
        }

        /* Enqueue the listening socket first */
        pollvec_clear(pollvec);
        if (nclients < config.max_clients)
            pollvec_push(pollvec, server, POLLIN);
        else
            pollvec_push(pollvec, -1, POLLIN);

        /* Enqueue clients that are due for another message */
        int timeout = -1;
        long long now = uepoch();
        for (struct client *c = queue->head; c; c = c->next) {
            if (c->send_next <= now) {
                pollvec_push(pollvec, c->fd, POLLOUT);
            } else {
                timeout = c->send_next - now;
                break;
            }
        }

        /* Wait for next event */
        logmsg(LOG_DEBUG, "poll(%zu, %d)%s", pollvec->fill, timeout,
                nclients >= config.max_clients ? " (no accept)" : "");
        int r = poll(pollvec->fds, pollvec->fill, timeout);
        logmsg(LOG_DEBUG, "= %d", r);
        if (r == -1) {
            switch (errno) {
                case EINTR:
                    logmsg(LOG_DEBUG, "EINTR");
                    continue;
                default:
                    fprintf(stderr, "endlessh: fatal: %s\n", strerror(errno));
                    exit(EXIT_FAILURE);
            }
        }

        /* Check for new incoming connections */
        if (pollvec->fds[0].revents & POLLIN) {
            int fd = accept(server, 0, 0);
            logmsg(LOG_DEBUG, "accept() = %d", fd);
            if (fd == -1) {
                const char *msg = strerror(errno);
                switch (errno) {
                    case EMFILE:
                    case ENFILE:
                        config.max_clients = nclients;
                        logmsg(LOG_INFO,
                                "MaxClients %d",
                                nclients);
                        break;
                    case ECONNABORTED:
                    case EINTR:
                    case ENOBUFS:
                    case ENOMEM:
                    case EPROTO:
                        fprintf(stderr, "endlessh: warning: %s\n", msg);
                        break;
                    default:
                        fprintf(stderr, "endlessh: fatal: %s\n", msg);
                        exit(EXIT_FAILURE);
                }
            } else {
                long long send_next = uepoch() + config.delay / 2;
                struct client *client = client_new(fd, send_next);
                if (!client) {
                    fprintf(stderr, "endlessh: warning: out of memory\n");
                    close(fd);
                }
                nclients++;
                logmsg(LOG_INFO, "ACCEPT host=%s:%d fd=%d n=%d/%d",
                        client->ipaddr, client->port, client->fd,
                        nclients, config.max_clients);
                queue_append(queue, client);
            }
        }

        /* Write lines to ready clients */
        for (size_t i = 1; i < pollvec->fill; i++) {
            short fd = pollvec->fds[i].fd;
            short revents = pollvec->fds[i].revents;
            struct client *client = queue_remove(queue, fd);

            if (revents & POLLHUP) {
                client_destroy(client);
                nclients--;

            } else if (revents & POLLOUT) {
                char line[256];
                int len = randline(line, config.max_line_length);
                /* Don't really care if send is short */
                ssize_t out = send(fd, line, len, MSG_DONTWAIT);
                if (out < 0)
                    client_destroy(client);
                else {
                    logmsg(LOG_DEBUG, "send(%d) = %d", fd, (int)out);
                    client->bytes_sent += out;
                    client->send_next = uepoch() + config.delay;
                    queue_append(queue, client);
                }
            }
        }
    }

    pollvec_free(pollvec);
    queue_destroy(queue);
}
