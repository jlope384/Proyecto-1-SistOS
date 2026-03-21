/*
 * client_cli.c  –  CLI Chat Client para WSL/Linux
 * Uso: ./client_cli <username> <IP_servidor> <puerto>
 *
 * Comandos:
 *  /dm <username> <mensaje>    - Enviar mensaje directo
 *  /state <status>             - Cambiar estado (active, dnd, invisible)
 *  /users                      - Listar usuarios en línea
 *  /info <username>            - Obtener información de usuario
 *  /help                       - Mostrar ayuda
 *  /quit                       - Salir
 *  <mensaje>                   - Enviar mensaje general (broadcast)
 *
 * Threads:
 *  - Main thread: entrada del usuario + procesamiento de comandos
 *  - recv_thread: recibe mensajes del servidor
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <ctype.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include "../include/proto_msgs.h"
#include "../include/framing.h"
#include "../include/serializer.h"

/* ── Globals ── */
static int          server_fd   = -1;
static char         g_username[64];
static char         g_ip[INET_ADDRSTRLEN];
static StatusEnum   g_status    = STATUS_ENUM__ACTIVE;
static int          g_running   = 1;
static pthread_mutex_t g_print_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Estado del cliente ── */
typedef enum {
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_DISCONNECTED
} ClientState;

static ClientState g_state = STATE_CONNECTING;

/* ── Print seguro (thread-safe) ── */
static void safe_printf(const char *fmt, ...) {
    pthread_mutex_lock(&g_print_mutex);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    pthread_mutex_unlock(&g_print_mutex);
}

/* ── String de estado ── */
static const char *status_str(StatusEnum s) {
    switch(s) {
        case STATUS_ENUM__ACTIVE:         return "ACTIVE";
        case STATUS_ENUM__DO_NOT_DISTURB: return "DO_NOT_DISTURB";
        case STATUS_ENUM__INVISIBLE:      return "INVISIBLE";
        default:                          return "UNKNOWN";
    }
}

/* ── Convertir string a StatusEnum ── */
static int string_to_status(const char *str, StatusEnum *out) {
    if (strcasecmp(str, "active") == 0) {
        *out = STATUS_ENUM__ACTIVE;
        return 1;
    } else if (strcasecmp(str, "dnd") == 0 || strcasecmp(str, "do_not_disturb") == 0) {
        *out = STATUS_ENUM__DO_NOT_DISTURB;
        return 1;
    } else if (strcasecmp(str, "invisible") == 0) {
        *out = STATUS_ENUM__INVISIBLE;
        return 1;
    }
    return 0;
}

/* ── Thread receptor ── */
static void *recv_thread(void *arg) {
    (void)arg;
    uint8_t  type;
    uint8_t *payload;
    int      len;

    while (g_running) {
        len = recv_frame(server_fd, &type, &payload);
        if (len < 0) {
            safe_printf("\n[CLIENT] Desconectado del servidor.\n");
            g_running = 0;
            break;
        }

        switch (type) {

        case MSG_SERVER_RESPONSE: {
            ServerResponse *r = deserialize_server_response(payload, len);
            free(payload);
            if (!r) break;
            safe_printf("\n[SERVER %d] %s\n", r->status_code, 
                       r->message ? r->message : "");
            
            free_server_response(r);
            if (g_state == STATE_CONNECTING) {
                g_state = STATE_CONNECTED;
                safe_printf("> ");
            } else {
                safe_printf("> ");
            }
            break;
        }

        case MSG_BROADCAST: {
            BroadcastMessages *bm = deserialize_broadcast(payload, len);
            free(payload);
            if (!bm) break;
            
            safe_printf("\n[%s] %s\n", 
                       bm->username_origin ? bm->username_origin : "?",
                       bm->message ? bm->message : "");
            safe_printf("> ");
            
            free_broadcast(bm);
            break;
        }

        case MSG_FOR_DM: {
            ForDm *dm = deserialize_for_dm(payload, len);
            free(payload);
            if (!dm) break;
            
            safe_printf("\n[DM from %s] %s\n", 
                       dm->username_origin ? dm->username_origin : "?",
                       dm->message ? dm->message : "");
            safe_printf("> ");
            
            free_for_dm(dm);
            break;
        }

        case MSG_ALL_USERS: {
            AllUsers *au = deserialize_all_users(payload, len);
            free(payload);
            if (!au) break;
            
            safe_printf("\n── Usuarios en línea (%zu) ──\n", au->n_usernames);
            for (size_t i = 0; i < au->n_usernames; i++) {
                safe_printf("  • %s [%s]\n", 
                           au->usernames[i],
                           i < au->n_status ? status_str(au->status[i]) : "?");
            }
            safe_printf("> ");
            
            free_all_users(au);
            break;
        }

        case MSG_USER_INFO_RESP: {
            GetUserInfoResponse *r = deserialize_user_info_resp(payload, len);
            free(payload);
            if (!r) break;
            
            safe_printf("\n── Información de usuario ──\n");
            safe_printf("  Usuario: %s\n", r->username ? r->username : "?");
            safe_printf("  Estado: %s\n", status_str(r->status));
            safe_printf("  IP: %s\n", r->ip_address ? r->ip_address : "?");
            safe_printf("> ");
            
            free_user_info_resp(r);
            break;
        }

        default:
            free(payload);
            break;
        }
    }
    return NULL;
}

/* ── Enviar mensaje general ── */
static void send_broadcast(const char *message) {
    if (!message || strlen(message) == 0) return;

    MessageGeneral mg = {0};
    mg.message        = (char*)message;
    mg.status         = g_status;
    mg.username_origin = g_username;
    mg.ip              = g_ip;
    
    size_t len;
    uint8_t *buf = serialize_message_general(&mg, &len);
    send_frame(server_fd, MSG_GENERAL, buf, (uint32_t)len);
    free(buf);
}

/* ── Enviar mensaje directo ── */
static void send_dm(const char *dest, const char *message) {
    if (!dest || strlen(dest) == 0) {
        safe_printf("[ERROR] Destino de usuario requerido.\n");
        return;
    }
    if (!message || strlen(message) == 0) {
        safe_printf("[ERROR] Mensaje vacío.\n");
        return;
    }

    MessageDm dm = {0};
    dm.message     = (char*)message;
    dm.status      = g_status;
    dm.username_des = (char*)dest;
    dm.ip          = g_ip;
    
    size_t len;
    uint8_t *buf = serialize_message_dm(&dm, &len);
    send_frame(server_fd, MSG_DM, buf, (uint32_t)len);
    free(buf);
    
    safe_printf("[DM enviado a %s]: %s\n", dest, message);
}

/* ── Cambiar estado ── */
static void change_status(const char *status_str_arg) {
    StatusEnum new_status;
    if (!string_to_status(status_str_arg, &new_status)) {
        safe_printf("[ERROR] Estado inválido. Use: active, dnd, invisible\n");
        return;
    }

    ChangeStatus cs = {0};
    cs.status   = new_status;
    cs.username = g_username;
    cs.ip       = g_ip;
    
    size_t len;
    uint8_t *buf = serialize_change_status(&cs, &len);
    send_frame(server_fd, MSG_CHANGE_STATUS, buf, (uint32_t)len);
    free(buf);
    
    g_status = new_status;
    safe_printf("[STATUS] Estado cambiado a: %s\n", status_str(new_status));
}

/* ── Listar usuarios ── */
static void list_users(void) {
    ListUsers lu = {0};
    lu.username = g_username;
    lu.ip       = g_ip;
    
    size_t len;
    uint8_t *buf = serialize_list_users(&lu, &len);
    send_frame(server_fd, MSG_LIST_USERS, buf, (uint32_t)len);
    free(buf);
}

/* ── Obtener información de usuario ── */
static void get_user_info(const char *username) {
    if (!username || strlen(username) == 0) {
        safe_printf("[ERROR] Nombre de usuario requerido.\n");
        return;
    }

    GetUserInfo gui = {0};
    gui.username_des = (char*)username;
    gui.username     = g_username;
    gui.ip           = g_ip;
    
    size_t len;
    uint8_t *buf = serialize_get_user_info(&gui, &len);
    send_frame(server_fd, MSG_GET_USER_INFO, buf, (uint32_t)len);
    free(buf);
}

/* ── Mostrar ayuda ── */
static void show_help(void) {
    safe_printf(
        "\n════════════════════════════════════════════════\n"
        "Comandos disponibles:\n"
        "════════════════════════════════════════════════\n"
        "  /dm <usuario> <mensaje>  - Enviar mensaje directo\n"
        "  /state <estado>          - Cambiar estado\n"
        "                             (active, dnd, invisible)\n"
        "  /users                   - Listar usuarios en línea\n"
        "  /info <usuario>          - Obtener info de usuario\n"
        "  /help                    - Mostrar esta ayuda\n"
        "  /quit                    - Salir\n"
        "  <mensaje>                - Enviar mensaje general\n"
        "════════════════════════════════════════════════\n\n"
    );
}

/* ── Procesar comando ── */
static void process_command(char *line) {
    if (!line || strlen(line) == 0) return;

    /* Trim espacios al inicio */
    while (isspace(*line)) line++;
    if (strlen(line) == 0) return;

    /* Comando /dm */
    if (strncmp(line, "/dm ", 4) == 0) {
        char *rest = line + 4;
        char *space = strchr(rest, ' ');
        if (!space) {
            safe_printf("[ERROR] Uso: /dm <usuario> <mensaje>\n");
            return;
        }
        char username[64];
        strncpy(username, rest, space - rest);
        username[space - rest] = '\0';
        send_dm(username, space + 1);
        return;
    }

    /* Comando /state */
    if (strncmp(line, "/state ", 7) == 0) {
        change_status(line + 7);
        return;
    }

    /* Comando /users */
    if (strcmp(line, "/users") == 0) {
        list_users();
        return;
    }

    /* Comando /info */
    if (strncmp(line, "/info ", 6) == 0) {
        get_user_info(line + 6);
        return;
    }

    /* Comando /help */
    if (strcmp(line, "/help") == 0) {
        show_help();
        return;
    }

    /* Comando /quit */
    if (strcmp(line, "/quit") == 0) {
        if (server_fd >= 0) {
            Quit q = {0};
            q.quit = 1;
            q.ip   = g_ip;
            size_t len;
            uint8_t *buf = serialize_quit(&q, &len);
            send_frame(server_fd, MSG_QUIT, buf, (uint32_t)len);
            free(buf);
            close(server_fd);
        }
        g_running = 0;
        return;
    }

    /* Si no es comando, es mensaje general */
    if (line[0] != '/') {
        send_broadcast(line);
        return;
    }

    safe_printf("[ERROR] Comando desconocido: %s\n", line);
}

/* ── Obtener IP local ── */
static void get_local_ip(const char *server_ip, int port, char *out, size_t olen) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        perror("socket");
        strncpy(out, "127.0.0.1", olen - 1);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
        struct hostent *h = gethostbyname(server_ip);
        if (h) {
            memcpy(&addr.sin_addr, h->h_addr_list[0], h->h_length);
        }
    }

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        strncpy(out, "127.0.0.1", olen - 1);
        close(s);
        return;
    }

    struct sockaddr_in local;
    socklen_t ll = sizeof(local);
    if (getsockname(s, (struct sockaddr*)&local, &ll) < 0) {
        perror("getsockname");
        strncpy(out, "127.0.0.1", olen - 1);
        close(s);
        return;
    }

    inet_ntop(AF_INET, &local.sin_addr, out, olen);
    close(s);
}

/* ── Main ── */
int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <username> <IP_servidor> <puerto>\n", argv[0]);
        fprintf(stderr, "Ejemplo: %s alice 127.0.0.1 5000\n", argv[0]);
        return 1;
    }

    strncpy(g_username, argv[1], 63);
    const char *server_ip   = argv[2];
    int         server_port = atoi(argv[3]);

    /* Determinar IP local */
    get_local_ip(server_ip, server_port, g_ip, sizeof(g_ip));
    safe_printf("[CLIENT] IP local: %s\n", g_ip);

    /* Conectar al servidor */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in saddr = {0};
    saddr.sin_family = AF_INET;
    saddr.sin_port   = htons(server_port);

    if (inet_pton(AF_INET, server_ip, &saddr.sin_addr) <= 0) {
        struct hostent *h = gethostbyname(server_ip);
        if (!h) {
            fprintf(stderr, "No se puede resolver %s\n", server_ip);
            return 1;
        }
        memcpy(&saddr.sin_addr, h->h_addr_list[0], h->h_length);
    }

    if (connect(server_fd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
        perror("connect");
        return 1;
    }

    safe_printf("[CLIENT] Conectado a %s:%d\n", server_ip, server_port);

    /* Registarse */
    Register reg = {0};
    reg.username = g_username;
    reg.ip       = g_ip;
    
    size_t rlen;
    uint8_t *rbuf = serialize_register(&reg, &rlen);
    send_frame(server_fd, MSG_REGISTER, rbuf, (uint32_t)rlen);
    free(rbuf);

    /* Esperar respuesta de registro */
    uint8_t type;
    uint8_t *payload;
    int plen;

    plen = recv_frame(server_fd, &type, &payload);
    if (plen < 0 || type != MSG_SERVER_RESPONSE) {
        fprintf(stderr, "[CLIENT] Sin respuesta del servidor.\n");
        return 1;
    }

    ServerResponse *resp = deserialize_server_response(payload, plen);
    free(payload);

    if (!resp || !resp->is_successful) {
        fprintf(stderr, "[CLIENT] Registro fallido: %s\n",
                resp ? resp->message : "error desconocido");
        if (resp) free_server_response(resp);
        return 1;
    }

    safe_printf("[CLIENT] Registrado como '%s'.\n", g_username);
    safe_printf("[CLIENT] Escribe /help para ver los comandos.\n\n");
    free_server_response(resp);

    g_state = STATE_CONNECTED;

    /* Iniciar thread receptor */
    pthread_t rt;
    pthread_create(&rt, NULL, recv_thread, NULL);
    pthread_detach(rt);

    /* Loop principal: lectura de entrada del usuario */
    char line[1024];
    while (g_running) {
        safe_printf("> ");
        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        /* Quitar newline */
        line[strcspn(line, "\n")] = '\0';

        process_command(line);
    }

    /* Limpiar conexión */
    if (server_fd >= 0) {
        close(server_fd);
    }

    safe_printf("\n[CLIENT] Desconectado.\n");
    return 0;
}
