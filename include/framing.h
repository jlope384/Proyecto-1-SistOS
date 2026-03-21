#pragma once
#include <stdint.h>
#include <stdlib.h>

/* 5-byte header: [type:1][length:4 big-endian] */
#define HEADER_SIZE 5

/* Returns bytes written, -1 on error */
int send_frame(int fd, uint8_t type, const uint8_t *payload, uint32_t len);

/* Fills type and allocates *payload (caller must free). Returns payload len or -1 */
int recv_frame(int fd, uint8_t *type_out, uint8_t **payload_out);