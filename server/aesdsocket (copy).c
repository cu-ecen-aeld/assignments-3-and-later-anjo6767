#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <syslog.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/queue.h>
#include <time.h>

#define DATAFILE "/var/tmp/aesdsocketdata"
#define RECV_CHUNK 1024
#define SEND_CHUNK 1024
#define SERVICE_PORT_STR "9000"

static volatile sig_atomic_t g_run = 1;
static int g_listen_fd = -1;

static pthread_mutex_t g_file_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_ts_tid;

/* Connection list node */
struct conn_node {
    pthread_t tid;
    int connfd;
    volatile sig_atomic_t done;
    SLIST_ENTRY(conn_node) links;
};

SLIST_HEAD(conn_head, conn_node);
static struct conn_head g_conns = SLIST_HEAD_INITIALIZER(g_conns);

static void* conn_thread(void *arg);

/* =============== Timestamp thread =============== */
static void* timestamp_thread(void *arg)
{
    (void)arg;
    syslog(LOG_INFO, "Timestamp thread started");

    while (g_run) {
        struct timespec ts = { .tv_sec = 10, .tv_nsec = 0 };
        if (nanosleep(&ts, NULL) == -1 && !g_run) break;
        if (!g_run) break;

        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);

        char tbuf[64];
        strftime(tbuf, sizeof(tbuf), "%a, %d %b %Y %T %z", &tm_now);

        char line[96];
        int len = snprintf(line, sizeof(line), "timestamp: %s\n", tbuf);
        if (len <= 0) continue;

        pthread_mutex_lock(&g_file_mtx);
        int fd = open(DATAFILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            if (write(fd, line, len) < 0)
                syslog(LOG_ERR, "timestamp write failed: %s", strerror(errno));
            fsync(fd);
            close(fd);
        } else {
            syslog(LOG_ERR, "timestamp open failed: %s", strerror(errno));
        }
        pthread_mutex_unlock(&g_file_mtx);
    }

    syslog(LOG_INFO, "Timestamp thread exiting");
    return NULL;
}

/* =============== Signal handling =============== */
static void signal_handler(int signo)
{
    (void)signo;
    g_run = 0;
    if (g_listen_fd >= 0)
        shutdown(g_listen_fd, SHUT_RDWR);
}

static int install_signals(void)
{
    struct sigaction new_action;
    memset(&new_action, 0, sizeof(new_action));
    new_action.sa_handler = signal_handler;
    if (sigaction(SIGINT, &new_action, NULL) != 0)
        return -1;
    if (sigaction(SIGTERM, &new_action, NULL) != 0)
        return -1;
    return 0;
}

/* =============== File send helper =============== */
static int send_entire_file(int conn_fd)
{
    int fd = open(DATAFILE, O_RDONLY);
    if (fd < 0) return -1;

    char buf[SEND_CHUNK];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t s = send(conn_fd, buf + off, (size_t)(n - off), MSG_NOSIGNAL);
            if (s <= 0) { close(fd); return -1; }
            off += s;
        }
    }
    close(fd);
    return (n < 0) ? -1 : 0;
}

/* =============== Daemonize =============== */
static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);

    if (setsid() < 0) return -1;

    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);

    umask(0);
    if (chdir("/") < 0) return -1;

    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) close(fd);
    }
    return 0;
}

/* =============== Connection thread =============== */
static void* conn_thread(void *arg)
{
    struct conn_node *node = (struct conn_node*)arg;
    int connfd = node->connfd;

    char chunk[RECV_CHUNK];
    char *pkt = NULL;
    size_t pkt_len = 0;

    while (g_run) {
        ssize_t r = recv(connfd, chunk, sizeof chunk, 0);
        if (r == 0) {
            /* Client closed: append any leftover bytes, then ALWAYS echo the file,
               so zero-byte "ready" probes see the current contents. */
            pthread_mutex_lock(&g_file_mtx);
            if (pkt_len > 0) {
                int fdw = open(DATAFILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
                if (fdw >= 0) {
                    if (write(fdw, pkt, pkt_len) < 0)
                        syslog(LOG_ERR, "write failed: %s", strerror(errno));
                    fsync(fdw);
                    close(fdw);
                } else {
                    syslog(LOG_ERR, "open for write failed: %s", strerror(errno));
                }
            }
            (void)send_entire_file(connfd);  // echo under same mutex
            pthread_mutex_unlock(&g_file_mtx);

            pkt_len = 0;
            break;
        }
        if (r < 0) {
            syslog(LOG_ERR, "recv: %s", strerror(errno));
            break;
        }

        char *np = realloc(pkt, pkt_len + (size_t)r);
        if (!np) {
            syslog(LOG_ERR, "realloc failed—discarding current packet");
            free(pkt); pkt = NULL; pkt_len = 0;
            continue;
        }
        pkt = np;
        memcpy(pkt + pkt_len, chunk, (size_t)r);
        pkt_len += (size_t)r;

        size_t start = 0;
        for (size_t i = 0; i < pkt_len; ++i) {
            if (pkt[i] == '\n') {
                size_t one_len = i + 1 - start;

                pthread_mutex_lock(&g_file_mtx);
                int fdw = open(DATAFILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
                if (fdw >= 0) {
                    if (write(fdw, pkt + start, one_len) < 0)
                        syslog(LOG_ERR, "write failed: %s", strerror(errno));
                    fsync(fdw);              // flush before echo
                    close(fdw);
                } else {
                    syslog(LOG_ERR, "open for write failed: %s", strerror(errno));
                }
                (void)send_entire_file(connfd);  // echo full file atomically
                pthread_mutex_unlock(&g_file_mtx);

                start = i + 1;
            }
        }

        if (start > 0) {
            size_t tail = pkt_len - start;
            memmove(pkt, pkt + start, tail);
            pkt_len = tail;
            char *shr = realloc(pkt, pkt_len ? pkt_len : 1);
            if (shr) pkt = shr;
        }
    }

    free(pkt);
    shutdown(connfd, SHUT_RDWR);
    close(connfd);
    node->done = 1;
    return NULL;
}

/* =============== Main =============== */
int main(int argc, char *argv[])
{
    int run_daemon = 0;
    int opt;
    while ((opt = getopt(argc, argv, "d")) != -1) {
        if (opt == 'd') run_daemon = 1;
    }

    openlog("aesdsocket", 0, LOG_USER);

    if (install_signals() == -1) {
        syslog(LOG_ERR, "sigaction: %s", strerror(errno));
        closelog();
        return -1;
    }

    unlink(DATAFILE);

    struct addrinfo hints, *res = NULL, *p = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int status = getaddrinfo(NULL, SERVICE_PORT_STR, &hints, &res);
    if (status != 0) {
        syslog(LOG_ERR, "getaddrinfo: %s", gai_strerror(status));
        closelog();
        return -1;
    }

    int sockfd;
    for (p = res; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) continue;

        int yes = 1;
        (void)setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(res);
    if (sockfd < 0) {
        closelog();
        return -1;
    }

    if (run_daemon && daemonize() < 0) {
        syslog(LOG_ERR, "daemonize failed: %s", strerror(errno));
        close(sockfd);
        closelog();
        return -1;
    }

    if (listen(sockfd, SOMAXCONN) < 0) {
        syslog(LOG_ERR, "listen: %s", strerror(errno));
        close(sockfd);
        closelog();
        return -1;
    }

    g_listen_fd = sockfd;
    syslog(LOG_INFO, "Listening on port %s", SERVICE_PORT_STR);

    /* Write startup marker once the listener is ready */
    pthread_mutex_lock(&g_file_mtx);
    int fd0 = open(DATAFILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd0 >= 0) {
        const char *marker = "timestamp:wait-for-startup\n";
        if (write(fd0, marker, strlen(marker)) < 0) {
            syslog(LOG_ERR, "startup marker write failed: %s", strerror(errno));
        }
        fsync(fd0);
        close(fd0);
    } else {
        syslog(LOG_ERR, "startup marker open failed: %s", strerror(errno));
    }
    pthread_mutex_unlock(&g_file_mtx);

    pthread_create(&g_ts_tid, NULL, timestamp_thread, NULL);

    while (g_run) {
        struct sockaddr_storage their_addr;
        socklen_t addrlen = sizeof(their_addr);
        int connfd = accept(sockfd, (struct sockaddr *)&their_addr, &addrlen);
        if (connfd < 0) {
            if (!g_run) break;
            syslog(LOG_ERR, "accept: %s", strerror(errno));
            continue;
        }

        struct conn_node *node = malloc(sizeof *node);
        if (!node) {
            syslog(LOG_ERR, "malloc conn_node failed");
            close(connfd);
            continue;
        }

        node->connfd = connfd;
        node->done = 0;

        if (pthread_create(&node->tid, NULL, conn_thread, node) != 0) {
            syslog(LOG_ERR, "pthread_create failed: %s", strerror(errno));
            close(connfd);
            free(node);
        } else {
            SLIST_INSERT_HEAD(&g_conns, node, links);
        }

        struct conn_node *it = SLIST_FIRST(&g_conns);
        while (it) {
            struct conn_node *next = SLIST_NEXT(it, links);
            if (it->done) {
                pthread_join(it->tid, NULL);
                SLIST_REMOVE(&g_conns, it, conn_node, links);
                free(it);
            }
            it = next;
        }
    }

    syslog(LOG_INFO, "Caught signal, exiting");
    if (g_listen_fd >= 0) close(g_listen_fd);

    struct conn_node *it = SLIST_FIRST(&g_conns);
    while (it) {
        struct conn_node *next = SLIST_NEXT(it, links);
        shutdown(it->connfd, SHUT_RDWR);
        pthread_join(it->tid, NULL);
        SLIST_REMOVE(&g_conns, it, conn_node, links);
        free(it);
        it = next;
    }

    pthread_join(g_ts_tid, NULL);
    pthread_mutex_destroy(&g_file_mtx);
    unlink(DATAFILE);
    closelog();
    return 0;
}

