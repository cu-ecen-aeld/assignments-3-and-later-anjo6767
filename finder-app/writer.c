#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <writefile> <writestr>\n", argv[0]);
        return 1;
    }

    const char *writefile = argv[1];
    const char *writestr  = argv[2];

    /* Set up syslog with LOG_USER */
    openlog("writer", 0, LOG_USER);

    /* Required debug message */
    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

    /* Open/create/truncate the file (does NOT create missing directories) */
    int fd = open(writefile, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd == -1) {
        syslog(LOG_ERR, "Error opening file %s: %s", writefile, strerror(errno));
        closelog();
        return 1;
    }

    /* Write once (simple); check for error or short write */
    size_t len = strlen(writestr);
    ssize_t n  = write(fd, writestr, len);
    if (n == -1 || (size_t)n != len) {
        syslog(LOG_ERR, "Error writing to %s: %s", writefile,
               (n == -1) ? strerror(errno) : "short write");
        close(fd);
        closelog();
        return 1;
    }

    if (close(fd) == -1) {
        syslog(LOG_ERR, "Error closing file %s: %s", writefile, strerror(errno));
        closelog();
        return 1;
    }

    closelog();
    return 0;
}

