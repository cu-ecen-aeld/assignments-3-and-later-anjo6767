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
#include <sys/time.h>

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#if USE_AESD_CHAR_DEVICE
#define DATAFILE "/dev/aesdchar"
#else
#define DATAFILE "/var/tmp/aesdsocketdata"
#endif
#define RECV_CHUNK 1024
#define SERVICE_PORT_STR "9000"

volatile sig_atomic_t g_run = 1;
static int g_listen_fd = -1;  

pthread_mutex_t g_file_mtx = PTHREAD_MUTEX_INITIALIZER;


struct conn_node {
    pthread_t tid; //Thread ID 
    int connfd; //Client socket 
    volatile sig_atomic_t done; //Set when thread finished 
    SLIST_ENTRY(conn_node) links;
};

SLIST_HEAD(conn_head, conn_node);
static struct conn_head g_conns = SLIST_HEAD_INITIALIZER(g_conns);

static pthread_t g_ts_tid;


static char *read_file_locked(size_t *out_len)
{
    char *buf = NULL;
    *out_len = 0;

    int fd = open(DATAFILE, O_RDONLY);
    if (fd < 0) 
    {
        return NULL;
    }

    /*struct stat st;
    if (fstat(fd, &st) == -1) 
    {
        close(fd);
        return NULL;
    }

    if (st.st_size == 0) 
    {
        close(fd);
        return NULL;
    }

    buf = malloc(st.st_size);
    if (!buf) 
    {
        close(fd);
        return NULL;
    }

    size_t offset = 0;
    while (offset < (size_t)st.st_size) 
    {
        ssize_t n = read(fd, buf + offset, (size_t)st.st_size - offset);
        if (n <= 0) 
        {
            free(buf);
            close(fd);
            return NULL;
        }
        offset += (size_t)n;
    }
    close(fd);
    *out_len = (size_t)st.st_size;
    return buf;*/
   
    size_t cap = 0, len = 0;

    for (;;) {
        char tmp[4096];
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n > 0) {
            if (len + (size_t)n > cap) {
                size_t newcap = cap ? cap * 2 : 4096;
                while (newcap < len + (size_t)n) newcap *= 2;
                char *nb = realloc(buf, newcap);
                if (!nb) { free(buf); close(fd); return NULL; }
                buf = nb; cap = newcap;
            }
            memcpy(buf + len, tmp, (size_t)n);
            len += (size_t)n;
        } else if (n == 0) {
            break;                  // EOF for the device on this read
        } else if (errno == EINTR) {
            continue;               // interrupted, retry
        } else {
            free(buf); close(fd); return NULL;   // real error
        }
    }

    close(fd);
    *out_len = len;
    return buf;          
}


static void *conn_thread(void *arg)
{
    struct conn_node *node = (struct conn_node *)arg;
    int connfd = node->connfd;

    char recv_buf[RECV_CHUNK];
    char *packet = NULL;     
    size_t packet_len = 0;

    while (g_run) 
    {
        ssize_t bytes = recv(connfd, recv_buf, sizeof(recv_buf), 0);
        if (bytes == 0) 
	{
            pthread_mutex_lock(&g_file_mtx);
            if (packet_len > 0) 
	    {
                //int fdw = open(DATAFILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
                #if USE_AESD_CHAR_DEVICE
		int fdw = open(DATAFILE, O_WRONLY);
		#else
		int fdw = open(DATAFILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
		#endif
                if (fdw >= 0) 
		{
                    (void)write(fdw, packet, packet_len);
                    (void)fsync(fdw);
                    close(fdw);
                }
            }
        

            size_t flen;
            char *filedata = read_file_locked(&flen);
            pthread_mutex_unlock(&g_file_mtx);

            if (filedata && flen > 0) {
                size_t offset = 0;
                while (offset < flen) {
                    ssize_t s = send(connfd, filedata + offset, flen - offset, MSG_NOSIGNAL);
                    if (s <= 0) {
                        break;
                    }
                    offset += (size_t)s;
                }
            }
            free(filedata);
            break;
        } else if (bytes < 0) 
	{
            syslog(LOG_ERR, "recv failed: %s", strerror(errno));
            break;
        }

        char *new_packet = realloc(packet, packet_len + (size_t)bytes);
        if (!new_packet) 
	{
            syslog(LOG_ERR, "realloc failed: %s", strerror(errno));
            free(packet);
            packet = NULL;
            packet_len = 0;
            continue;
        }
        packet = new_packet;
        memcpy(packet + packet_len, recv_buf, (size_t)bytes);
        packet_len += (size_t)bytes;


        size_t start = 0;
        for (size_t i = 0; i < packet_len; ++i) 
	{
            if (packet[i] == '\n') 
	    {
                size_t chunk_len = (i + 1) - start;

                pthread_mutex_lock(&g_file_mtx);
                //int fdw = open(DATAFILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
                #if USE_AESD_CHAR_DEVICE
		int fdw = open(DATAFILE, O_WRONLY);
		#else
		int fdw = open(DATAFILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
		#endif
                if (fdw >= 0) 
		{
                    (void)write(fdw, packet + start, chunk_len);
                    (void)fsync(fdw);
                    close(fdw);
                }

                size_t flen;
                char *filedata = read_file_locked(&flen);
                pthread_mutex_unlock(&g_file_mtx);


                if (filedata && flen > 0) 
		{
                    size_t off = 0;
                    while (off < flen) 
	            {
                        ssize_t sent = send(connfd, filedata + off, flen - off, MSG_NOSIGNAL);
                        if (sent <= 0) 
			{
                            break;
                        }
                        off += (size_t)sent;
                    }
                }
                free(filedata);

                start = i + 1;
            }
        }

        if (start > 0) 
	{
            size_t tail = packet_len - start;
            memmove(packet, packet + start, tail);
            packet_len = tail;
            char *shrunk = realloc(packet, packet_len ? packet_len : 1);
            if (shrunk) 
	    {
                packet = shrunk;
            }
        }
    }

    free(packet);
    shutdown(connfd, SHUT_RDWR);
    close(connfd);
    node->done = 1;
    return NULL;
}

#if !USE_AESD_CHAR_DEVICE
static void *timestamp_thread(void *arg)
{
    (void)arg;
    while (g_run) {
        struct timespec ts = { .tv_sec = 10, .tv_nsec = 0 };
        if (nanosleep(&ts, NULL) == -1 && !g_run) {
            break;
        }
        if (!g_run) {
            break;
        }

        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);

        char timebuf[64];
        strftime(timebuf, sizeof timebuf, "%a, %d %b %Y %T %z", &tm_now);

        char line[96];
        int len = snprintf(line, sizeof line, "timestamp: %s\n", timebuf);
        if (len <= 0) {
            continue;
        }

        pthread_mutex_lock(&g_file_mtx);
        int fd = open(DATAFILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            (void)write(fd, line, (size_t)len);
            (void)fsync(fd);
            close(fd);
        }
        pthread_mutex_unlock(&g_file_mtx);
    }
    return NULL;
}
#endif

static void signal_handler(int signo)
{
    (void)signo;
    g_run = 0;
    if (g_listen_fd >= 0) 
    {
        shutdown(g_listen_fd, SHUT_RDWR);
    }
}

static int install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = signal_handler;
    if (sigaction(SIGINT, &sa, NULL) != 0) 
    {
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) 
    {
        return -1;
    }

    struct sigaction sp;
    memset(&sp, 0, sizeof sp);
    sp.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sp, NULL) != 0) {
        return -1;
    }
    return 0;
}


static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) 
    {
        return -1;
    }
    if (pid > 0) 
    {
        _exit(0);
    }

    if (setsid() < 0) 
    {
        return -1;
    }
    pid = fork();
    if (pid < 0) 
    {
        return -1;
    }
    if (pid > 0) 
    {
        _exit(0);
    }

    umask(0);
    if (chdir("/") < 0) 
    {
        return -1;
    }

    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) 
    {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) 
        {
            close(fd);
        }
    }
    return 0;
}

int main(int argc, char *argv[])
{
    int run_daemon = 0;
    int opt;
    while ((opt = getopt(argc, argv, "d")) != -1) 
    {
        if (opt == 'd') 
        {
            run_daemon = 1;
        }
    }
#if !USE_AESD_CHAR_DEVICE
    unlink(DATAFILE);
#endif
    openlog("aesdsocket", 0, LOG_USER);


    if (install_signals() != 0) 
    {
        syslog(LOG_ERR, "sigaction failed: %s", strerror(errno));
        closelog();
        return -1;
    }

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int status = getaddrinfo(NULL, SERVICE_PORT_STR, &hints, &res);
    if (status != 0) 
    {
        syslog(LOG_ERR, "getaddrinfo: %s", gai_strerror(status));
        closelog();
        return -1;
    }

    int sockfd = -1;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) 
    {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) 
        {
            continue;
        }
        int yes = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == 0) 
        {
            break;
        }
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(res);
    if (sockfd < 0) 
    {
        syslog(LOG_ERR, "socket/bind failed");
        closelog();
        return -1;
    }

    if (run_daemon && daemonize() < 0) 
    {
        syslog(LOG_ERR, "daemonize failed");
        close(sockfd);
        closelog();
        return -1;
    }

    if (listen(sockfd, SOMAXCONN) < 0) 
    {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        close(sockfd);
        closelog();
        return -1;
    }
    g_listen_fd = sockfd;
    syslog(LOG_INFO, "aesdsocket listening on port %s", SERVICE_PORT_STR);


#if !USE_AESD_CHAR_DEVICE
    if (pthread_create(&g_ts_tid, NULL, timestamp_thread, NULL) != 0) 
    {
        syslog(LOG_ERR, "timestamp thread creation failed");
        close(sockfd);
        closelog();
        return -1;
    }
#endif

    while (g_run) 
    {
        int connfd = accept(sockfd, NULL, NULL);
        if (connfd < 0) 
        {
            if (!g_run) 
            {
                break;
            }
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        struct conn_node *node = malloc(sizeof *node);
        if (!node) {
            syslog(LOG_ERR, "malloc failed");
            close(connfd);
            continue;
        }
        node->connfd = connfd;
        node->done = 0;

        if (pthread_create(&node->tid, NULL, conn_thread, node) != 0) {
            syslog(LOG_ERR, "pthread_create failed: %s", strerror(errno));
            close(connfd);
            free(node);
            continue;
        }

        SLIST_INSERT_HEAD(&g_conns, node, links);

        struct conn_node *it = SLIST_FIRST(&g_conns);
        while (it) 
	{
            struct conn_node *next = SLIST_NEXT(it, links);
            if (it->done) 
            {
                pthread_join(it->tid, NULL);
                SLIST_REMOVE(&g_conns, it, conn_node, links);
                free(it);
            }
            it = next;
        }
    }

    if (g_listen_fd >= 0) 
    {
        close(g_listen_fd);
    }

#if !USE_AESD_CHAR_DEVICE
    g_run = 0;
    pthread_cancel(g_ts_tid);
    pthread_join(g_ts_tid, NULL);
#endif

    struct conn_node *it = SLIST_FIRST(&g_conns);
    while (it) 
    {
        struct conn_node *next = SLIST_NEXT(it, links);
        shutdown(it->connfd, SHUT_RDWR);
        pthread_join(it->tid, NULL);
        SLIST_REMOVE(&g_conns, it, conn_node, links);
        free(it);
        it = next;
    }

#if !USE_AESD_CHAR_DEVICE
    unlink(DATAFILE);
#endif
    pthread_mutex_destroy(&g_file_mtx);
    syslog(LOG_INFO, "aesdsocket exiting");
    closelog();
    return 0;
}

