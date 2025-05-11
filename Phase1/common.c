#include "common_impl.h"
#include <sys/socket.h>

/* Vous pouvez ecrire ici toutes les fonctions */
/* qui pourraient etre utilisees par le lanceur */
/* et le processus intermediaire. N'oubliez pas */
/* de declarer le prototype de ces nouvelles */
/* fonctions dans common_impl.h */

ssize_t dsm_send_all(int fd, void *buf, size_t len, int flags) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t ret = send(fd, (char*)buf + sent, len - sent, flags);
        if (ret == -1) {
            if (errno == EINTR) continue;
            if (errno == EPIPE) return sent;
            return -1;
        }
        sent += ret;
    }
    return sent;
}

ssize_t dsm_recv_all(int fd, void *buf, size_t len, int flags) {
    size_t received = 0;
    while (received < len) {
        ssize_t ret = recv(fd, (char*)buf + received, len - received, flags);
        if (ret == -1) {
            if (errno == EINTR) continue;
            if (errno == EPIPE) return received;
            
            return -1;
        }
        if (ret == 0) { // Connection closed
            return received;
        }
        received += ret;
    }
    return received;
}

ssize_t dsm_read_all(int fd, void *buf, size_t len) {
    size_t read_total = 0;
    while (read_total < len) {
        ssize_t ret = read(fd, (char*)buf + read_total, len - read_total);
        if (ret == -1) {
            if (errno == EINTR) continue;
            if (errno == EPIPE) return read_total;
            return -1;
        }
        if (ret == 0) { // EOF
            return read_total;
        }
        read_total += ret;
    }
    return read_total;
}

ssize_t dsm_write_all(int fd, const void *buf, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t ret = write(fd, (char*)buf + written, len - written);
        if (ret == -1) {
            if (errno == EINTR) continue;
            if (errno == EPIPE) return written;
            return -1;
        }
        written += ret;
    }
    return written;
}
