/*
 * serializer.c
 * Manual protobuf-c encoding/decoding for all message types.
 * Uses varint + length-delimited encoding compatible with the proto2 wire format.
 *
 * Wire types:
 *   0 = varint  (int32, bool, enum)
 *   2 = length-delimited (string, bytes, repeated)
 */
#include "serializer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ════════════════════════════════════════════════════
   Low-level varint / protobuf encoding primitives
   ════════════════════════════════════════════════════ */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} Buf;

static void buf_init(Buf *b) {
    b->data = malloc(64);
    b->len  = 0;
    b->cap  = 64;
}

static void buf_grow(Buf *b, size_t need) {
    while (b->cap < b->len + need) b->cap *= 2;
    b->data = realloc(b->data, b->cap);
}

static void buf_write_byte(Buf *b, uint8_t v) {
    buf_grow(b, 1);
    b->data[b->len++] = v;
}

/* Encode unsigned varint */
static void buf_write_varint(Buf *b, uint64_t v) {
    do {
        uint8_t byte = v & 0x7F;
        v >>= 7;
        if (v) byte |= 0x80;
        buf_write_byte(b, byte);
    } while (v);
}

/* Encode field tag: (field_number << 3) | wire_type */
static void buf_write_tag(Buf *b, uint32_t field, uint8_t wire) {
    buf_write_varint(b, ((uint64_t)field << 3) | wire);
}

/* Encode a string field (wire type 2) */
static void buf_write_string(Buf *b, uint32_t field, const char *s) {
    if (!s) return;
    size_t slen = strlen(s);
    buf_write_tag(b, field, 2);
    buf_write_varint(b, slen);
    buf_grow(b, slen);
    memcpy(b->data + b->len, s, slen);
    b->len += slen;
}

/* Encode a varint field (wire type 0) */
static void buf_write_varint_field(Buf *b, uint32_t field, uint64_t v) {
    buf_write_tag(b, field, 0);
    buf_write_varint(b, v);
}

/* Encode repeated string */
static void buf_write_repeated_string(Buf *b, uint32_t field, char **arr, size_t n) {
    for (size_t i = 0; i < n; i++) buf_write_string(b, field, arr[i]);
}

/* Encode repeated enum (packed: wire type 2, then varints) */
static void buf_write_repeated_enum_packed(Buf *b, uint32_t field, StatusEnum *arr, size_t n) {
    if (n == 0) return;
    /* First encode into temp buf to get byte length */
    Buf tmp; buf_init(&tmp);
    for (size_t i = 0; i < n; i++) buf_write_varint(&tmp, (uint64_t)(int32_t)arr[i]);
    buf_write_tag(b, field, 2);
    buf_write_varint(b, tmp.len);
    buf_grow(b, tmp.len);
    memcpy(b->data + b->len, tmp.data, tmp.len);
    b->len += tmp.len;
    free(tmp.data);
}

static uint8_t *buf_finish(Buf *b, size_t *out_len) {
    *out_len = b->len;
    return b->data; /* caller owns */
}

/* ════════════════════════════════════════════════════
   Low-level decode primitives
   ════════════════════════════════════════════════════ */

typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
} Reader;

static int reader_eof(Reader *r) { return r->pos >= r->len; }

static int read_varint(Reader *r, uint64_t *out) {
    uint64_t v = 0; int shift = 0;
    while (r->pos < r->len) {
        uint8_t b = r->data[r->pos++];
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) { *out = v; return 0; }
        shift += 7;
        if (shift >= 64) return -1;
    }
    return -1;
}

static int read_length_delimited(Reader *r, const uint8_t **out, size_t *out_len) {
    uint64_t len;
    if (read_varint(r, &len) < 0) return -1;
    if (r->pos + len > r->len) return -1;
    *out     = r->data + r->pos;
    *out_len = (size_t)len;
    r->pos  += len;
    return 0;
}

static char *read_string_field(Reader *r) {
    const uint8_t *s; size_t slen;
    if (read_length_delimited(r, &s, &slen) < 0) return NULL;
    char *str = malloc(slen + 1);
    memcpy(str, s, slen);
    str[slen] = '\0';
    return str;
}

/* Skip unknown field */
static int skip_field(Reader *r, int wire_type) {
    uint64_t v; const uint8_t *tmp; size_t tmp_len;
    switch (wire_type) {
        case 0: return read_varint(r, &v);
        case 1: if (r->pos + 8 > r->len) return -1; r->pos += 8; return 0;
        case 2: return read_length_delimited(r, &tmp, &tmp_len);
        case 5: if (r->pos + 4 > r->len) return -1; r->pos += 4; return 0;
        default: return -1;
    }
}

/* ════════════════════════════════════════════════════
   SERIALIZE implementations
   ════════════════════════════════════════════════════ */

uint8_t *serialize_register(const Register *m, size_t *len) {
    Buf b; buf_init(&b);
    buf_write_string(&b, 1, m->username);
    buf_write_string(&b, 2, m->ip);
    return buf_finish(&b, len);
}

uint8_t *serialize_message_general(const MessageGeneral *m, size_t *len) {
    Buf b; buf_init(&b);
    buf_write_string(&b, 1, m->message);
    buf_write_varint_field(&b, 2, (uint64_t)(int32_t)m->status);
    buf_write_string(&b, 3, m->username_origin);
    buf_write_string(&b, 4, m->ip);
    return buf_finish(&b, len);
}

uint8_t *serialize_message_dm(const MessageDm *m, size_t *len) {
    Buf b; buf_init(&b);
    buf_write_string(&b, 1, m->message);
    buf_write_varint_field(&b, 2, (uint64_t)(int32_t)m->status);
    buf_write_string(&b, 3, m->username_des);
    buf_write_string(&b, 4, m->ip);
    return buf_finish(&b, len);
}

uint8_t *serialize_change_status(const ChangeStatus *m, size_t *len) {
    Buf b; buf_init(&b);
    buf_write_varint_field(&b, 1, (uint64_t)(int32_t)m->status);
    buf_write_string(&b, 2, m->username);
    buf_write_string(&b, 3, m->ip);
    return buf_finish(&b, len);
}

uint8_t *serialize_list_users(const ListUsers *m, size_t *len) {
    Buf b; buf_init(&b);
    buf_write_string(&b, 1, m->username);
    buf_write_string(&b, 2, m->ip);
    return buf_finish(&b, len);
}

uint8_t *serialize_get_user_info(const GetUserInfo *m, size_t *len) {
    Buf b; buf_init(&b);
    buf_write_string(&b, 1, m->username_des);
    buf_write_string(&b, 2, m->username);
    buf_write_string(&b, 3, m->ip);
    return buf_finish(&b, len);
}

uint8_t *serialize_quit(const Quit *m, size_t *len) {
    Buf b; buf_init(&b);
    buf_write_varint_field(&b, 1, m->quit ? 1 : 0);
    buf_write_string(&b, 2, m->ip);
    return buf_finish(&b, len);
}

uint8_t *serialize_server_response(const ServerResponse *m, size_t *len) {
    Buf b; buf_init(&b);
    buf_write_varint_field(&b, 1, (uint64_t)(int32_t)m->status_code);
    buf_write_string(&b, 2, m->message);
    buf_write_varint_field(&b, 3, m->is_successful ? 1 : 0);
    return buf_finish(&b, len);
}

uint8_t *serialize_all_users(const AllUsers *m, size_t *len) {
    Buf b; buf_init(&b);
    buf_write_repeated_string(&b, 1, m->usernames, m->n_usernames);
    buf_write_repeated_enum_packed(&b, 2, m->status, m->n_status);
    return buf_finish(&b, len);
}

uint8_t *serialize_for_dm(const ForDm *m, size_t *len) {
    Buf b; buf_init(&b);
    buf_write_string(&b, 1, m->username_des);
    buf_write_string(&b, 2, m->message);
    if (m->username_origin) buf_write_string(&b, 3, m->username_origin);
    return buf_finish(&b, len);
}

uint8_t *serialize_broadcast(const BroadcastMessages *m, size_t *len) {
    Buf b; buf_init(&b);
    buf_write_string(&b, 1, m->message);
    buf_write_string(&b, 2, m->username_origin);
    return buf_finish(&b, len);
}

uint8_t *serialize_user_info_resp(const GetUserInfoResponse *m, size_t *len) {
    Buf b; buf_init(&b);
    buf_write_string(&b, 1, m->ip_address);
    buf_write_string(&b, 2, m->username);
    buf_write_varint_field(&b, 3, (uint64_t)(int32_t)m->status);
    return buf_finish(&b, len);
}

/* ════════════════════════════════════════════════════
   DESERIALIZE implementations
   ════════════════════════════════════════════════════ */

Register *deserialize_register(const uint8_t *buf, size_t len) {
    Register *m = calloc(1, sizeof(*m));
    Reader r = {buf, len, 0};
    while (!reader_eof(&r)) {
        uint64_t tag; if (read_varint(&r, &tag) < 0) break;
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        if (field == 1 && wire == 2) m->username = read_string_field(&r);
        else if (field == 2 && wire == 2) m->ip = read_string_field(&r);
        else skip_field(&r, wire);
    }
    return m;
}

MessageGeneral *deserialize_message_general(const uint8_t *buf, size_t len) {
    MessageGeneral *m = calloc(1, sizeof(*m));
    Reader r = {buf, len, 0};
    while (!reader_eof(&r)) {
        uint64_t tag; if (read_varint(&r, &tag) < 0) break;
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        uint64_t v;
        if (field == 1 && wire == 2) m->message = read_string_field(&r);
        else if (field == 2 && wire == 0) { read_varint(&r, &v); m->status = (StatusEnum)(int32_t)v; }
        else if (field == 3 && wire == 2) m->username_origin = read_string_field(&r);
        else if (field == 4 && wire == 2) m->ip = read_string_field(&r);
        else skip_field(&r, wire);
    }
    return m;
}

MessageDm *deserialize_message_dm(const uint8_t *buf, size_t len) {
    MessageDm *m = calloc(1, sizeof(*m));
    Reader r = {buf, len, 0};
    while (!reader_eof(&r)) {
        uint64_t tag; if (read_varint(&r, &tag) < 0) break;
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        uint64_t v;
        if (field == 1 && wire == 2) m->message = read_string_field(&r);
        else if (field == 2 && wire == 0) { read_varint(&r, &v); m->status = (StatusEnum)(int32_t)v; }
        else if (field == 3 && wire == 2) m->username_des = read_string_field(&r);
        else if (field == 4 && wire == 2) m->ip = read_string_field(&r);
        else skip_field(&r, wire);
    }
    return m;
}

ChangeStatus *deserialize_change_status(const uint8_t *buf, size_t len) {
    ChangeStatus *m = calloc(1, sizeof(*m));
    Reader r = {buf, len, 0};
    while (!reader_eof(&r)) {
        uint64_t tag; if (read_varint(&r, &tag) < 0) break;
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        uint64_t v;
        if (field == 1 && wire == 0) { read_varint(&r, &v); m->status = (StatusEnum)(int32_t)v; }
        else if (field == 2 && wire == 2) m->username = read_string_field(&r);
        else if (field == 3 && wire == 2) m->ip = read_string_field(&r);
        else skip_field(&r, wire);
    }
    return m;
}

ListUsers *deserialize_list_users(const uint8_t *buf, size_t len) {
    ListUsers *m = calloc(1, sizeof(*m));
    Reader r = {buf, len, 0};
    while (!reader_eof(&r)) {
        uint64_t tag; if (read_varint(&r, &tag) < 0) break;
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        if (field == 1 && wire == 2) m->username = read_string_field(&r);
        else if (field == 2 && wire == 2) m->ip = read_string_field(&r);
        else skip_field(&r, wire);
    }
    return m;
}

GetUserInfo *deserialize_get_user_info(const uint8_t *buf, size_t len) {
    GetUserInfo *m = calloc(1, sizeof(*m));
    Reader r = {buf, len, 0};
    while (!reader_eof(&r)) {
        uint64_t tag; if (read_varint(&r, &tag) < 0) break;
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        if (field == 1 && wire == 2) m->username_des = read_string_field(&r);
        else if (field == 2 && wire == 2) m->username = read_string_field(&r);
        else if (field == 3 && wire == 2) m->ip = read_string_field(&r);
        else skip_field(&r, wire);
    }
    return m;
}

Quit *deserialize_quit(const uint8_t *buf, size_t len) {
    Quit *m = calloc(1, sizeof(*m));
    Reader r = {buf, len, 0};
    while (!reader_eof(&r)) {
        uint64_t tag; if (read_varint(&r, &tag) < 0) break;
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        uint64_t v;
        if (field == 1 && wire == 0) { read_varint(&r, &v); m->quit = (v != 0); }
        else if (field == 2 && wire == 2) m->ip = read_string_field(&r);
        else skip_field(&r, wire);
    }
    return m;
}

ServerResponse *deserialize_server_response(const uint8_t *buf, size_t len) {
    ServerResponse *m = calloc(1, sizeof(*m));
    Reader r = {buf, len, 0};
    while (!reader_eof(&r)) {
        uint64_t tag; if (read_varint(&r, &tag) < 0) break;
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        uint64_t v;
        if (field == 1 && wire == 0) { read_varint(&r, &v); m->status_code = (int32_t)v; }
        else if (field == 2 && wire == 2) m->message = read_string_field(&r);
        else if (field == 3 && wire == 0) { read_varint(&r, &v); m->is_successful = (v != 0); }
        else skip_field(&r, wire);
    }
    return m;
}

AllUsers *deserialize_all_users(const uint8_t *buf, size_t len) {
    AllUsers *m = calloc(1, sizeof(*m));
    /* First pass: count fields */
    Reader r = {buf, len, 0};
    size_t n_u = 0, n_s = 0;
    while (!reader_eof(&r)) {
        uint64_t tag; if (read_varint(&r, &tag) < 0) break;
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        const uint8_t *tmp; size_t tlen; uint64_t v;
        if (field == 1 && wire == 2) { read_length_delimited(&r, &tmp, &tlen); n_u++; }
        else if (field == 2 && wire == 2) {
            /* packed enums: count varints inside */
            read_length_delimited(&r, &tmp, &tlen);
            Reader pr = {tmp, tlen, 0};
            while (!reader_eof(&pr)) { if (read_varint(&pr, &v) < 0) break; n_s++; }
        } else skip_field(&r, wire);
    }
    m->usernames = calloc(n_u + 1, sizeof(char*));
    m->status    = calloc(n_s + 1, sizeof(StatusEnum));
    m->n_usernames = 0; m->n_status = 0;
    /* Second pass: fill */
    r.pos = 0;
    while (!reader_eof(&r)) {
        uint64_t tag; if (read_varint(&r, &tag) < 0) break;
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        const uint8_t *tmp; size_t tlen; uint64_t v;
        if (field == 1 && wire == 2) m->usernames[m->n_usernames++] = read_string_field(&r);
        else if (field == 2 && wire == 2) {
            read_length_delimited(&r, &tmp, &tlen);
            Reader pr = {tmp, tlen, 0};
            while (!reader_eof(&pr)) {
                if (read_varint(&pr, &v) < 0) break;
                m->status[m->n_status++] = (StatusEnum)(int32_t)v;
            }
        } else skip_field(&r, wire);
    }
    return m;
}

ForDm *deserialize_for_dm(const uint8_t *buf, size_t len) {
    ForDm *m = calloc(1, sizeof(*m));
    Reader r = {buf, len, 0};
    while (!reader_eof(&r)) {
        uint64_t tag; if (read_varint(&r, &tag) < 0) break;
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        if (field == 1 && wire == 2) m->username_des = read_string_field(&r);
        else if (field == 2 && wire == 2) m->message = read_string_field(&r);
        else if (field == 3 && wire == 2) m->username_origin = read_string_field(&r);
        else skip_field(&r, wire);
    }
    return m;
}

BroadcastMessages *deserialize_broadcast(const uint8_t *buf, size_t len) {
    BroadcastMessages *m = calloc(1, sizeof(*m));
    Reader r = {buf, len, 0};
    while (!reader_eof(&r)) {
        uint64_t tag; if (read_varint(&r, &tag) < 0) break;
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        if (field == 1 && wire == 2) m->message = read_string_field(&r);
        else if (field == 2 && wire == 2) m->username_origin = read_string_field(&r);
        else skip_field(&r, wire);
    }
    return m;
}

GetUserInfoResponse *deserialize_user_info_resp(const uint8_t *buf, size_t len) {
    GetUserInfoResponse *m = calloc(1, sizeof(*m));
    Reader r = {buf, len, 0};
    while (!reader_eof(&r)) {
        uint64_t tag; if (read_varint(&r, &tag) < 0) break;
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        uint64_t v;
        if (field == 1 && wire == 2) m->ip_address = read_string_field(&r);
        else if (field == 2 && wire == 2) m->username = read_string_field(&r);
        else if (field == 3 && wire == 0) { read_varint(&r, &v); m->status = (StatusEnum)(int32_t)v; }
        else skip_field(&r, wire);
    }
    return m;
}

/* ════════════════════════════════════════════════════
   FREE implementations
   ════════════════════════════════════════════════════ */

void free_register(Register *m)             { if(!m) return; free(m->username); free(m->ip); free(m); }
void free_message_general(MessageGeneral *m){ if(!m) return; free(m->message); free(m->username_origin); free(m->ip); free(m); }
void free_message_dm(MessageDm *m)          { if(!m) return; free(m->message); free(m->username_des); free(m->ip); free(m); }
void free_change_status(ChangeStatus *m)    { if(!m) return; free(m->username); free(m->ip); free(m); }
void free_list_users(ListUsers *m)          { if(!m) return; free(m->username); free(m->ip); free(m); }
void free_get_user_info(GetUserInfo *m)     { if(!m) return; free(m->username_des); free(m->username); free(m->ip); free(m); }
void free_quit(Quit *m)                     { if(!m) return; free(m->ip); free(m); }
void free_server_response(ServerResponse *m){ if(!m) return; free(m->message); free(m); }
void free_all_users(AllUsers *m) {
    if(!m) return;
    for(size_t i=0;i<m->n_usernames;i++) free(m->usernames[i]);
    free(m->usernames); free(m->status); free(m);
}
void free_for_dm(ForDm *m)                  { if(!m) return; free(m->username_des); free(m->message); free(m->username_origin); free(m); }
void free_broadcast(BroadcastMessages *m)   { if(!m) return; free(m->message); free(m->username_origin); free(m); }
void free_user_info_resp(GetUserInfoResponse *m){ if(!m) return; free(m->ip_address); free(m->username); free(m); }