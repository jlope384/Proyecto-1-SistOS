/*
 * server.c  –  Chat Server
 * Uso: ./server <puerto>
 *
 * Características:
 *  - Multithreading: un thread por cliente + thread de inactividad
 *  - Broadcasting y mensajes directos
 *  - Registro / liberación de usuarios
 *  - Cambio de status (ACTIVO, OCUPADO, INACTIVO)
 *  - Auto-INACTIVO tras INACTIVITY_SECONDS sin mensajes
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include <signal.h>

#include "../include/proto_msgs.h"
#include "../include/framing.h"
#include "../include/serializer.h"

/* ── Configuration ── */
#define MAX_CLIENTS       64
#define INACTIVITY_SECONDS 60   /* set low for demo, e.g. 30 */
#define BUF_SIZE          4096

/* ── Client record ── */
typedef struct {
    int          fd;
    char         username[64];
    char         ip[INET_ADDRSTRLEN];
    StatusEnum   status;
    time_t       last_active;
    int          active;        /* 1 = slot in use */
    pthread_t    thread;
} Client;

static Client       clients[MAX_CLIENTS];
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Helpers ── */
static const char *status_str(StatusEnum s) {
    switch(s) {
        case STATUS_ENUM__ACTIVE:         return "ACTIVE";
        case STATUS_ENUM__DO_NOT_DISTURB: return "DO_NOT_DISTURB";
        case STATUS_ENUM__INVISIBLE:      return "INVISIBLE";
        default:                          return "UNKNOWN";
    }
}

/* Send a ServerResponse to a single fd */
static void send_response(int fd, int code, const char *msg, int ok) {
    ServerResponse r = {0};
    r.status_code    = code;
    r.message        = (char*)msg;
    r.is_successful  = ok;
    size_t len; uint8_t *buf = serialize_server_response(&r, &len);
    send_frame(fd, MSG_SERVER_RESPONSE, buf, (uint32_t)len);
    free(buf);
}

/* Broadcast a message to all ACTIVE / BUSY clients (not invisible sender) */
static void broadcast_message(const char *text, const char *origin) {
    BroadcastMessages bm = {0};
    bm.message        = (char*)text;
    bm.username_origin = (char*)origin;
    size_t len; uint8_t *buf = serialize_broadcast(&bm, &len);

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active)
            send_frame(clients[i].fd, MSG_BROADCAST, buf, (uint32_t)len);
    }
    pthread_mutex_unlock(&clients_mutex);
    free(buf);
}

/* Send DM to a specific user */
static int send_dm(const char *dest_username, const char *origin_username, const char *text) {
    ForDm dm = {0};
    dm.username_des   = (char*)dest_username;
    dm.message        = (char*)text;
    dm.username_origin = (char*)origin_username;
    size_t len; uint8_t *buf = serialize_for_dm(&dm, &len);

    int found = 0;
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active &&
            strcmp(clients[i].username, dest_username) == 0) {
            send_frame(clients[i].fd, MSG_FOR_DM, buf, (uint32_t)len);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    free(buf);
    return found;
}

/* Find client slot by fd (must hold lock) */
static int find_slot_by_fd(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active && clients[i].fd == fd) return i;
    return -1;
}

/* Remove client and close fd (must hold lock) */
static void remove_client(int idx) {
    printf("[SERVER] User '%s' disconnected.\n", clients[idx].username);
    close(clients[idx].fd);
    memset(&clients[idx], 0, sizeof(Client));
}

/* ── Inactivity monitor thread ── */
static void *inactivity_monitor(void *arg) {
    (void)arg;
    while (1) {
        sleep(10); /* check every 10s */
        time_t now = time(NULL);
        pthread_mutex_lock(&clients_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].active) continue;
            if (clients[i].status == STATUS_ENUM__INVISIBLE) continue;
            double elapsed = difftime(now, clients[i].last_active);
            if (elapsed >= INACTIVITY_SECONDS &&
                clients[i].status != STATUS_ENUM__INVISIBLE) {
                printf("[SERVER] User '%s' set INACTIVE (%.0fs idle).\n",
                       clients[i].username, elapsed);
                clients[i].status = STATUS_ENUM__INVISIBLE;
                /* Notify client */
                send_response(clients[i].fd, 200,
                              "Your status has been set to INACTIVE due to inactivity.", 1);
            }
        }
        pthread_mutex_unlock(&clients_mutex);
    }
    return NULL;
}

/* ── Per-client handler thread ── */
static void *handle_client(void *arg) {
    int fd = *(int*)arg;
    free(arg);

    uint8_t  type;
    uint8_t *payload;
    int      len;

    while (1) {
        len = recv_frame(fd, &type, &payload);
        if (len < 0) {
            /* Client disconnected ungracefully */
            pthread_mutex_lock(&clients_mutex);
            int idx = find_slot_by_fd(fd);
            if (idx >= 0) remove_client(idx);
            pthread_mutex_unlock(&clients_mutex);
            break;
        }

        /* Update last_active */
        pthread_mutex_lock(&clients_mutex);
        int idx = find_slot_by_fd(fd);
        if (idx >= 0) clients[idx].last_active = time(NULL);
        pthread_mutex_unlock(&clients_mutex);

        switch (type) {

        /* ── TYPE 1: Register ── */
        case MSG_REGISTER: {
            Register *msg = deserialize_register(payload, len);
            free(payload);
            if (!msg || !msg->username || !msg->ip) {
                send_response(fd, 400, "Bad register message.", 0);
                free_register(msg);
                break;
            }
            pthread_mutex_lock(&clients_mutex);
            /* Check duplicate username or IP */
            int dup = 0;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (!clients[i].active) continue;
                if (strcmp(clients[i].username, msg->username) == 0) { dup = 1; break; }
            }
            if (dup) {
                pthread_mutex_unlock(&clients_mutex);
                send_response(fd, 409, "Username already taken.", 0);
            } else {
                /* Find free slot */
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++)
                    if (!clients[i].active) { slot = i; break; }
                if (slot < 0) {
                    pthread_mutex_unlock(&clients_mutex);
                    send_response(fd, 503, "Server full.", 0);
                } else {
                    clients[slot].fd          = fd;
                    clients[slot].status      = STATUS_ENUM__ACTIVE;
                    clients[slot].last_active = time(NULL);
                    clients[slot].active      = 1;
                    strncpy(clients[slot].username, msg->username, 63);
                    strncpy(clients[slot].ip,       msg->ip,       INET_ADDRSTRLEN-1);
                    pthread_mutex_unlock(&clients_mutex);
                    printf("[SERVER] User '%s' registered from %s.\n",
                           msg->username, msg->ip);
                    send_response(fd, 200, "Registration successful.", 1);
                }
            }
            free_register(msg);
            break;
        }

        /* ── TYPE 2: Message General (broadcast) ── */
        case MSG_GENERAL: {
            MessageGeneral *msg = deserialize_message_general(payload, len);
            free(payload);
            if (!msg) break;
            printf("[BROADCAST] %s: %s\n", msg->username_origin, msg->message);
            broadcast_message(msg->message, msg->username_origin);
            free_message_general(msg);
            break;
        }

        /* ── TYPE 3: Message DM ── */
        case MSG_DM: {
            MessageDm *msg = deserialize_message_dm(payload, len);
            free(payload);
            if (!msg) break;
            /* Find sender username */
            pthread_mutex_lock(&clients_mutex);
            char sender[64] = "unknown";
            int si = find_slot_by_fd(fd);
            if (si >= 0) strncpy(sender, clients[si].username, 63);
            pthread_mutex_unlock(&clients_mutex);

            printf("[DM] %s → %s: %s\n", sender, msg->username_des, msg->message);
            if (!send_dm(msg->username_des, sender, msg->message))
                send_response(fd, 404, "User not found or offline.", 0);
            else
                send_response(fd, 200, "Message delivered.", 1);
            free_message_dm(msg);
            break;
        }

        /* ── TYPE 4: Change Status ── */
        case MSG_CHANGE_STATUS: {
            ChangeStatus *msg = deserialize_change_status(payload, len);
            free(payload);
            if (!msg) break;
            pthread_mutex_lock(&clients_mutex);
            int si = find_slot_by_fd(fd);
            if (si >= 0) {
                clients[si].status = msg->status;
                clients[si].last_active = time(NULL);
                printf("[SERVER] '%s' status → %s\n",
                       clients[si].username, status_str(msg->status));
            }
            pthread_mutex_unlock(&clients_mutex);
            send_response(fd, 200, "Status updated.", 1);
            free_change_status(msg);
            break;
        }

        /* ── TYPE 5: List Users ── */
        case MSG_LIST_USERS: {
            free(payload);
            pthread_mutex_lock(&clients_mutex);
            /* Build AllUsers response */
            AllUsers au = {0};
            char *names[MAX_CLIENTS];
            StatusEnum statuses[MAX_CLIENTS];
            int count = 0;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].active) {
                    names[count]    = clients[i].username;
                    statuses[count] = clients[i].status;
                    count++;
                }
            }
            au.usernames   = names;
            au.status      = statuses;
            au.n_usernames = count;
            au.n_status    = count;
            size_t alen; uint8_t *abuf = serialize_all_users(&au, &alen);
            pthread_mutex_unlock(&clients_mutex);
            send_frame(fd, MSG_ALL_USERS, abuf, (uint32_t)alen);
            free(abuf);
            break;
        }

        /* ── TYPE 6: Get User Info ── */
        case MSG_GET_USER_INFO: {
            GetUserInfo *msg = deserialize_get_user_info(payload, len);
            free(payload);
            if (!msg) break;
            pthread_mutex_lock(&clients_mutex);
            int found = 0;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].active &&
                    strcmp(clients[i].username, msg->username_des) == 0) {
                    GetUserInfoResponse resp = {0};
                    resp.ip_address = clients[i].ip;
                    resp.username   = clients[i].username;
                    resp.status     = clients[i].status;
                    size_t rlen; uint8_t *rbuf = serialize_user_info_resp(&resp, &rlen);
                    pthread_mutex_unlock(&clients_mutex);
                    send_frame(fd, MSG_USER_INFO_RESP, rbuf, (uint32_t)rlen);
                    free(rbuf);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                pthread_mutex_unlock(&clients_mutex);
                send_response(fd, 404, "User not found.", 0);
            }
            free_get_user_info(msg);
            break;
        }

        /* ── TYPE 7: Quit ── */
        case MSG_QUIT: {
            free(payload);
            pthread_mutex_lock(&clients_mutex);
            int si = find_slot_by_fd(fd);
            if (si >= 0) remove_client(si);
            pthread_mutex_unlock(&clients_mutex);
            goto thread_exit;
        }

        default:
            free(payload);
            fprintf(stderr, "[SERVER] Unknown message type %d\n", type);
            break;
        }
    }

thread_exit:
    return NULL;
}

/* ── Main ── */
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <puerto>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    signal(SIGPIPE, SIG_IGN); /* ignore broken pipe */

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server_fd, 16) < 0) { perror("listen"); return 1; }

    printf("[SERVER] Listening on port %d  (inactivity timeout: %ds)\n",
           port, INACTIVITY_SECONDS);

    /* Start inactivity monitor */
    pthread_t mon;
    pthread_create(&mon, NULL, inactivity_monitor, NULL);
    pthread_detach(mon);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t clen = sizeof(client_addr);
        int cfd = accept(server_fd, (struct sockaddr*)&client_addr, &clen);
        if (cfd < 0) { perror("accept"); continue; }

        printf("[SERVER] New connection from %s\n",
               inet_ntoa(client_addr.sin_addr));

        int *fd_ptr = malloc(sizeof(int));
        *fd_ptr = cfd;
        pthread_t t;
        pthread_create(&t, NULL, handle_client, fd_ptr);
        pthread_detach(t);
    }
    return 0;
}