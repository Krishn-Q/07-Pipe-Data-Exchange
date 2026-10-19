#include "pipe_lab.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Note on fflush(stdout) before every fork():
 * if stdout is redirected to a file or pipe (as a grading script does), it is
 * fully buffered. Anything still in the buffer at fork() time is copied into
 * the child and would be printed twice. Flushing first avoids that.
 *
 * Children finish with fflush(stdout) + _exit(0) rather than `return`:
 * returning would send the child back into main(), where it would run the
 * remaining tasks a second time.
 */

/*
 * Task A – one-way pipe
 *
 * The parent writes the string "ping" into a pipe.
 * The child reads it and prints exactly:
 *   child received: ping
 * The parent then waits for the child and prints exactly:
 *   parent: sent ping
 *
 * Required steps in the child:
 *   1. close fd[1] (unused write end)
 *   2. read the message into a buffer
 *   3. null-terminate the buffer and print "child received: <message>"
 *   4. close fd[0]
 *
 * Required steps in the parent:
 *   1. close fd[0] (unused read end)
 *   2. write the string "ping" into fd[1]
 *   3. close fd[1]
 *   4. waitpid() for the child
 *   5. print "parent: sent ping"
 */
int task_one_way(void) {
    int fd[2];
    if (pipe(fd) == -1) { perror("pipe"); return 1; }

    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        /* 1. close unused write end */
        close(fd[1]);

        /* 2. read the message; loop because one read() may return part of it */
        char buf[64];
        size_t total = 0;
        ssize_t n;
        while (total < sizeof(buf) - 1 &&
               (n = read(fd[0], buf + total, sizeof(buf) - 1 - total)) != 0) {
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("read");
                close(fd[0]);
                _exit(1);
            }
            total += (size_t)n;
        }

        /* 3. null-terminate and print */
        buf[total] = '\0';
        printf("child received: %s\n", buf);

        /* 4. close read end */
        close(fd[0]);
        fflush(stdout);
        _exit(0);
    }

    /* 1. close unused read end */
    close(fd[0]);

    /* 2. write "ping" */
    const char *msg = "ping";
    if (write(fd[1], msg, strlen(msg)) != (ssize_t)strlen(msg)) {
        perror("write");
        close(fd[1]);
        waitpid(pid, NULL, 0);
        return 1;
    }

    /* 3. close write end -> child's read() will return 0 after "ping" */
    close(fd[1]);

    /* 4. wait for the child */
    int status;
    if (waitpid(pid, &status, 0) == -1) { perror("waitpid"); return 1; }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 1;

    /* 5. print */
    printf("parent: sent ping\n");
    fflush(stdout);
    return 0;
}

/*
 * Task B – two-way pipe
 *
 * Pipes are unidirectional, so bidirectional communication needs TWO pipes:
 *   p2c[2]: parent writes → child reads
 *   c2p[2]: child writes  → parent reads
 *
 * The parent sends the integer 21 through p2c.
 * The child reads it, doubles it to 42, and prints exactly:
 *   child computed: 42
 * The child sends the result back through c2p.
 * The parent reads the result and prints exactly:
 *   parent received: 42
 *
 * Required: close every pipe end that is not used in each process.
 * Hint: write/read a raw int with write(fd, &val, sizeof(val)).
 */
int task_two_way(void) {
    int p2c[2], c2p[2];
    if (pipe(p2c) == -1 || pipe(c2p) == -1) { perror("pipe"); return 1; }

    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        /* child reads p2c and writes c2p; close the other two ends */
        close(p2c[1]);
        close(c2p[0]);

        int val;
        if (read(p2c[0], &val, sizeof(val)) != (ssize_t)sizeof(val)) {
            perror("child read");
            close(p2c[0]);
            close(c2p[1]);
            _exit(1);
        }
        close(p2c[0]);

        int result = val * 2;
        printf("child computed: %d\n", result);
        fflush(stdout);

        if (write(c2p[1], &result, sizeof(result)) != (ssize_t)sizeof(result)) {
            perror("child write");
            close(c2p[1]);
            _exit(1);
        }
        close(c2p[1]);
        _exit(0);
    }

    /* parent writes p2c and reads c2p; close the other two ends */
    close(p2c[0]);
    close(c2p[1]);

    int val = 21;
    if (write(p2c[1], &val, sizeof(val)) != (ssize_t)sizeof(val)) {
        perror("parent write");
        close(p2c[1]);
        close(c2p[0]);
        waitpid(pid, NULL, 0);
        return 1;
    }
    close(p2c[1]);

    int result;
    if (read(c2p[0], &result, sizeof(result)) != (ssize_t)sizeof(result)) {
        perror("parent read");
        close(c2p[0]);
        waitpid(pid, NULL, 0);
        return 1;
    }
    close(c2p[0]);

    int status;
    if (waitpid(pid, &status, 0) == -1) { perror("waitpid"); return 1; }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 1;

    printf("parent received: %d\n", result);
    fflush(stdout);
    return 0;
}

/*
 * Task C – EOF limitation
 *
 * When every write end of a pipe is closed, the next read() returns 0.
 * This is how a reader detects end-of-stream (EOF).
 *
 * Sequence:
 *   parent: write "hello" (5 bytes) into the pipe, then CLOSE the write end
 *   child:  read in a loop until read() returns 0; track total bytes
 *           print exactly: child: got EOF after 5 bytes
 *
 * The loop must check the return value of read() on every iteration.
 * The child should NOT assume a single read delivers all the bytes.
 */
int task_eof(void) {
    int fd[2];
    if (pipe(fd) == -1) { perror("pipe"); return 1; }

    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        /* must close our own copy of the write end, or read() never returns 0 */
        close(fd[1]);

        char buf[16];
        size_t total = 0;
        for (;;) {
            ssize_t n = read(fd[0], buf, sizeof(buf));
            if (n > 0) {                 /* got data */
                total += (size_t)n;
            } else if (n == 0) {         /* EOF: all write ends closed */
                break;
            } else {                     /* error */
                if (errno == EINTR) continue;
                perror("read");
                close(fd[0]);
                _exit(1);
            }
        }

        printf("child: got EOF after %zu bytes\n", total);
        close(fd[0]);
        fflush(stdout);
        _exit(0);
    }

    close(fd[0]);

    const char *msg = "hello";
    if (write(fd[1], msg, strlen(msg)) != (ssize_t)strlen(msg)) {
        perror("write");
        close(fd[1]);
        waitpid(pid, NULL, 0);
        return 1;
    }
    close(fd[1]);                        /* this close is what produces EOF */

    int status;
    if (waitpid(pid, &status, 0) == -1) { perror("waitpid"); return 1; }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 1;
    return 0;
}

/*
 * Task D – SIGPIPE / EPIPE limitation
 *
 * Writing to a pipe with no readers delivers SIGPIPE to the writer,
 * killing it unless the signal is ignored.  With SIGPIPE ignored,
 * write() returns -1 and sets errno to EPIPE.
 *
 * A synchronisation pipe (sync_fd) is used so the parent only writes
 * after the child has already closed the read end of data_fd.
 *
 * Required sequence:
 *   child:  close data_fd[0] and data_fd[1]
 *           write one byte to sync_fd[1] (ack), close sync_fd[1], exit
 *   parent: close sync_fd[1]
 *           read one byte from sync_fd[0] (blocks until child sends ack)
 *           close sync_fd[0] and data_fd[0]
 *           call signal(SIGPIPE, SIG_IGN)
 *           write one byte to data_fd[1]; check if errno == EPIPE
 *           print exactly: parent: broken pipe detected
 *           close data_fd[1], waitpid
 */
int task_broken_pipe(void) {
    int data_fd[2];
    int sync_fd[2];
    if (pipe(data_fd) == -1 || pipe(sync_fd) == -1) { perror("pipe"); return 1; }

    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        /* child never reads sync_fd */
        close(sync_fd[0]);

        /* drop the child's reader (and writer) on the data pipe */
        close(data_fd[0]);
        close(data_fd[1]);

        /* ack: "my read end is closed" */
        char ack = 'x';
        if (write(sync_fd[1], &ack, 1) != 1) {
            perror("child write ack");
            close(sync_fd[1]);
            _exit(1);
        }
        close(sync_fd[1]);
        _exit(0);
    }

    /* parent never writes sync_fd */
    close(sync_fd[1]);

    /* block until the child has closed its data_fd read end */
    char ack;
    if (read(sync_fd[0], &ack, 1) != 1) {
        perror("parent read ack");
        close(sync_fd[0]);
        close(data_fd[0]);
        close(data_fd[1]);
        waitpid(pid, NULL, 0);
        return 1;
    }
    close(sync_fd[0]);

    /* close our own reader: now data_fd has no readers at all */
    close(data_fd[0]);

    /* ignore SIGPIPE so write() fails with EPIPE instead of killing us */
    signal(SIGPIPE, SIG_IGN);

    int rc = 1;
    char byte = 'B';
    errno = 0;
    ssize_t n = write(data_fd[1], &byte, 1);
    if (n == -1 && errno == EPIPE) {
        printf("parent: broken pipe detected\n");
        fflush(stdout);
        rc = 0;
    } else if (n == -1) {
        perror("write");
    } else {
        fprintf(stderr, "write unexpectedly succeeded\n");
    }

    close(data_fd[1]);
    if (waitpid(pid, NULL, 0) == -1) { perror("waitpid"); return 1; }
    return rc;
}

/* ------------------------------------------------------------------ */
int main(void) {
    if (task_one_way() != 0)     { fprintf(stderr, "task_one_way failed\n");     return 1; }
    if (task_two_way() != 0)     { fprintf(stderr, "task_two_way failed\n");     return 1; }
    if (task_eof() != 0)         { fprintf(stderr, "task_eof failed\n");         return 1; }
    if (task_broken_pipe() != 0) { fprintf(stderr, "task_broken_pipe failed\n"); return 1; }
    printf("all tasks done\n");
    return 0;
}