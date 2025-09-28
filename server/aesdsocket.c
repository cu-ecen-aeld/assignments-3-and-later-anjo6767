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

#define DATAFILE "/var/tmp/aesdsocketdata"
#define RECV_CHUNK 1024
#define SEND_CHUNK 1024
#define SERVICE_PORT_STR "9000"

static volatile sig_atomic_t g_run = 1;
static int g_listen_fd = -1;  //sockfd

static void signal_handler(void)
{
    g_run = 0;
    if (g_listen_fd >= 0) 
    	shutdown(g_listen_fd, SHUT_RDWR); // unblock accept()
}

static int install_signals(void) 
{
    struct sigaction new_action;
    memset(&new_action, 0, sizeof(new_action));
    new_action.sa_handler = signal_handler;
    if (sigaction(SIGINT,  &new_action, NULL) != 0) 
    {
    	return -1;
    }
    if (sigaction(SIGTERM, &new_action, NULL) != 0)
    {
    	return -1;
    }
    return 0;
}

static int send_entire_file(int conn_fd)
{
    int fd = open(DATAFILE, O_RDONLY);
    if (fd < 0) return -1;

    char buf[SEND_CHUNK];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) 
    {
        ssize_t off = 0;
        while (off < n) 
        {
            ssize_t s = send(conn_fd, buf + off, (size_t)(n - off), MSG_NOSIGNAL);
            if (s <= 0) { close(fd); return -1; }
            off += s;
        }
    }
    close(fd);
    return (n < 0) ? -1 : 0;
}

static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);                 // parent exits

    if (setsid() < 0) return -1;           // become session leader

    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);                 // first child exits

    umask(0);
    if (chdir("/") < 0) return -1;

    // redirect stdio to /dev/null
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) close(fd);
    }
    return 0;
}



int main(int argc, char *argv[])
{
    int run_daemon = 0;

    int opt;
    while((opt = getopt(argc, argv, "d")) != -1) 
    {
        switch (opt) {
        case 'd': 
        	run_daemon = 1; 
        	break;
        default: 
        	break;
        }
    }

    openlog("aesdsocket", 0, LOG_USER);
    //signal(SIGPIPE, SIG_IGN); // avoid SIGPIPE on client timeout

    if (install_signals() == -1) 
    {
        syslog(LOG_ERR, "sigaction: %s", strerror(errno));
        closelog();
        return -1;
    }

    unlink(DATAFILE);    //start fresh each run

    struct addrinfo hints, *res = NULL, *p = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     // don't care IPv4 or IPv6
    //hints.ai_family = AF_INET;          // IPv4 only (your current choice)
    hints.ai_socktype = SOCK_STREAM;    // TCP stream sockets
    hints.ai_flags = AI_PASSIVE;        // use local IP

    int status = getaddrinfo(NULL, SERVICE_PORT_STR, &hints, &res);
    if (status != 0) {
        syslog(LOG_ERR, "getaddrinfo: %s", gai_strerror(status));
        closelog();
        exit(1);
    }

    int sockfd;
    for (p = res; p != NULL; p = p->ai_next) //pointing to struct addrinfo
    {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) 
        {
        	continue;
        }

        /*int yes = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
            syslog(LOG_ERR, "setsockopt: %s", strerror(errno));
            close(sockfd);
            sockfd = -1;
            continue;
        }*/

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == 0) 
        {
            syslog(LOG_INFO, "aesdsocket bound to port %s", SERVICE_PORT_STR);
            // success
            break;
        } else 
        {
            syslog(LOG_ERR, "bind: %s", strerror(errno));
            close(sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo(res);

    if (sockfd < 0) 
    {
        closelog();
        return -1; // fail on socket-step errors
    }

    //daemonize after binding, before listen
    if (run_daemon) 
    {
        if (daemonize() < 0) 
        {
            syslog(LOG_ERR, "daemonize failed: %s", strerror(errno));
            close(sockfd);
            closelog();
            return -1;
        }
    }

    //Listen
    if (listen(sockfd, SOMAXCONN) < 0) {
        syslog(LOG_ERR, "listen: %s", strerror(errno));
        close(sockfd);
        closelog();
        return -1;
    }
    g_listen_fd = sockfd;
    syslog(LOG_INFO, "Listening on port %s", SERVICE_PORT_STR);

    while (g_run) {
        struct sockaddr_storage their_addr;
        socklen_t addrlen = sizeof(their_addr);
        int connfd = accept(sockfd, (struct sockaddr *)&their_addr, &addrlen);
        if (connfd < 0) 
        {
            if (!g_run) 
            	break;              
            /*if (errno == EINTR) 
            	continue;*/
            syslog(LOG_ERR, "accept: %s", strerror(errno));
            continue;
        }

        char ipstr[INET6_ADDRSTRLEN] = "unknown";
        void *addrptr = NULL;
        if (their_addr.ss_family == AF_INET) 
        {
            addrptr = &((struct sockaddr_in *)&their_addr)->sin_addr;
        } 
        else if (their_addr.ss_family == AF_INET6) 
        {
            addrptr = &((struct sockaddr_in6 *)&their_addr)->sin6_addr;
        }
        if (addrptr)
        {
        	inet_ntop(their_addr.ss_family, addrptr, ipstr, sizeof ipstr);
        }
        syslog(LOG_INFO, "Accepted connection from %s", ipstr);

        //recv 
        int data_fd = open(DATAFILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
        
        if (data_fd < 0) 
        {
            syslog(LOG_ERR, "open(%s): %s", DATAFILE, strerror(errno));
            close(connfd);
            syslog(LOG_INFO, "Closed connection from %s", ipstr);
            continue;
        }

        char chunk[RECV_CHUNK];
        char *pkt = NULL;
        size_t pkt_len = 0;

        while (g_run) 
        {
            ssize_t r = recv(connfd, chunk, sizeof chunk, 0);
            if (r == 0)
            	 break;                          // client closed
            if (r < 0) 
            {
                /*if (errno == EINTR && !g_run) break;
                if (errno == EINTR) continue;*/
                syslog(LOG_ERR, "recv: %s", strerror(errno));
                break;
            }

            char *np = realloc(pkt, pkt_len + (size_t)r);
            if (!np) 
            {
                syslog(LOG_ERR, "realloc failed—discarding current packet");
                free(pkt); 
                pkt = NULL; 
                pkt_len = 0;
                continue;
            }
            pkt = np;
            memcpy(pkt + pkt_len, chunk, (size_t)r);
            pkt_len += (size_t)r;

            size_t start = 0;
            for (size_t i = 0; i < pkt_len; ++i) 
            {
                if (pkt[i] == '\n') {
                    size_t one_len = i + 1 - start;     // include '\n'

                    if (write(data_fd, pkt + start, one_len) < 0)     //append complete packet 
                    {
                        syslog(LOG_ERR, "write(%s): %s", DATAFILE, strerror(errno));
                    } 
                    
                    else 
                    {
                        fsync(data_fd);      // ensure visible before read back
                        if (send_entire_file(connfd) < 0) 
                        {
                            // client likely went away
                            start = i + 1;
                            break;
                        }
                    }
                    start = i + 1;
                }
            }


        }

        free(pkt);
        close(data_fd);
        close(connfd);
        syslog(LOG_INFO, "Closed connection from %s", ipstr);
    }

    syslog(LOG_INFO, "Caught signal, exiting");
    if (g_listen_fd >= 0) 
    {
    	close(g_listen_fd);
    }
    unlink(DATAFILE);
    closelog();
    return 0;
}

