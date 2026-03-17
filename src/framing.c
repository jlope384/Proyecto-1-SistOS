#include "framing.h"
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>

/* Read exactly n bytes from fd, handling partial reads */
static int read_exactly(int fd, uint8_t *buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t r = read(fd, buf + total, n - total);
        if (r <= 0) return -1;
        total += r;
    }
    return 0;
}

int send_frame(int fd, uint8_t type, const uint8_t *payload, uint32_t len) {
    uint8_t header[HEADER_SIZE];
    header[0] = type;
    uint32_t net_len = htonl(len);
    memcpy(header + 1, &net_len, 4);

    /* Send header */
    if (write(fd, header, HEADER_SIZE) != HEADER_SIZE) return -1;
    /* Send payload */
    if (len > 0) {
        size_t sent = 0;
        while (sent < len) {
            ssize_t w = write(fd, payload + sent, len - sent);
            if (w <= 0) return -1;
            sent += w;
        }
    }
    return (int)(HEADER_SIZE + len);
}

int recv_frame(int fd, uint8_t *type_out, uint8_t **payload_out) {
    uint8_t header[HEADER_SIZE];
    if (read_exactly(fd, header, HEADER_SIZE) < 0) return -1;

    *type_out = header[0];
    uint32_t net_len;
    memcpy(&net_len, header + 1, 4);
    uint32_t len = ntohl(net_len);

    if (len == 0) {
        *payload_out = NULL;
        return 0;
    }
    if (len > 4 * 1024 * 1024) return -1; /* sanity: max 4MB */

    *payload_out = malloc(len);
    if (!*payload_out) return -1;

    if (read_exactly(fd, *payload_out, len) < 0) {
        free(*payload_out);
        *payload_out = NULL;
        return -1;
    }
    return (int)len;
}