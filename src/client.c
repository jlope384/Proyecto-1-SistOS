/*
 * client.c  –  Chat Client con GUI GTK3
 * Uso: ./client <username> <IP_servidor> <puerto>
 *
 * Threads:
 *  - Main thread: GTK event loop
 *  - recv_thread: recibe mensajes del servidor y actualiza la GUI via g_idle_add
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
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

/* GTK widgets (accessed only from main thread via g_idle_add) */
static GtkWidget   *chat_view;       /* GtkTextView: general chat */
static GtkWidget   *dm_view;         /* GtkTextView: direct messages */
static GtkWidget   *users_view;      /* GtkTextView: user list */
static GtkWidget   *entry_msg;       /* entry: message input */
static GtkWidget   *entry_dest;      /* entry: DM destination */
static GtkWidget   *status_label;    /* label: current status */
static GtkWidget   *window;

/* ── Append text to a GtkTextView (must be called on main thread) ── */
static void append_text(GtkWidget *tv, const char *text) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_insert(buf, &end, text, -1);
    /* Auto-scroll */
    GtkTextMark *mark = gtk_text_buffer_get_insert(buf);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(tv), mark);
}

/* ── g_idle_add payload structs ── */
typedef struct { GtkWidget *tv; char *text; } IdleAppend;
typedef struct { char *text; }               IdleStatus;

static gboolean idle_append_chat(gpointer data) {
    IdleAppend *p = data;
    append_text(p->tv, p->text);
    free(p->text); free(p);
    return G_SOURCE_REMOVE;
}
static gboolean idle_set_status(gpointer data) {
    IdleStatus *p = data;
    gtk_label_set_text(GTK_LABEL(status_label), p->text);
    free(p->text); free(p);
    return G_SOURCE_REMOVE;
}

/* Schedule status label update from any thread */
static void schedule_status_label(const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    IdleStatus *p = malloc(sizeof(*p));
    p->text = strdup(buf);
    g_idle_add(idle_set_status, p);
}

/* Schedule text append from any thread */
static void schedule_append(GtkWidget *tv, const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    IdleAppend *p = malloc(sizeof(*p));
    p->tv   = tv;
    p->text = strdup(buf);
    g_idle_add(idle_append_chat, p);
}
/* ── Status string ── */
static const char *status_str(StatusEnum s) {
    switch(s) {
        case STATUS_ENUM__ACTIVE:         return "ACTIVE";
        case STATUS_ENUM__DO_NOT_DISTURB: return "DO_NOT_DISTURB";
        case STATUS_ENUM__INVISIBLE:      return "INVISIBLE";
        default:                          return "UNKNOWN";
    }
}

/* ── Receive thread ── */
static void *recv_thread(void *arg) {
    (void)arg;
    uint8_t  type;
    uint8_t *payload;
    int      len;

    while (1) {
        len = recv_frame(server_fd, &type, &payload);
        if (len < 0) {
            schedule_append(chat_view, "\n[CLIENT] Disconnected from server.\n");
            break;
        }

        switch (type) {

        case MSG_SERVER_RESPONSE: {
            ServerResponse *r = deserialize_server_response(payload, len);
            free(payload);
            if (!r) break;
            schedule_append(chat_view,
                "\n[SERVER %d] %s\n", r->status_code, r->message ? r->message : "");
            /* If server forced INACTIVE status, update label and local state */
            if (r->message && strstr(r->message, "INACTIVE")) {
                g_status = STATUS_ENUM__INVISIBLE;
                schedule_status_label("Status: INVISIBLE (set by server)");
            }
            free_server_response(r);
            break;
        }

        case MSG_BROADCAST: {
            BroadcastMessages *bm = deserialize_broadcast(payload, len);
            free(payload);
            if (!bm) break;
            schedule_append(chat_view,
                "\n[%s]: %s\n",
                bm->username_origin ? bm->username_origin : "?",
                bm->message ? bm->message : "");
            free_broadcast(bm);
            break;
        }

        case MSG_FOR_DM: {
            ForDm *dm = deserialize_for_dm(payload, len);
            free(payload);
            if (!dm) break;
            schedule_append(dm_view,
                "\n[DM from %s]: %s\n",
                dm->username_origin ? dm->username_origin : "?",
                dm->message ? dm->message : "");
            free_for_dm(dm);
            break;
        }

        case MSG_ALL_USERS: {
            AllUsers *au = deserialize_all_users(payload, len);
            free(payload);
            if (!au) break;
            char line[256];
            snprintf(line, sizeof(line), "\n── Users online (%zu) ──\n", au->n_usernames);
            schedule_append(users_view, line);
            for (size_t i = 0; i < au->n_usernames; i++) {
                const char *st = (i < au->n_status) ? status_str(au->status[i]) : "?";
                snprintf(line, sizeof(line), "  • %s [%s]\n",
                         au->usernames[i] ? au->usernames[i] : "?", st);
                schedule_append(users_view, line);
            }
            free_all_users(au);
            break;
        }

        case MSG_USER_INFO_RESP: {
            GetUserInfoResponse *r = deserialize_user_info_resp(payload, len);
            free(payload);
            if (!r) break;
            schedule_append(chat_view,
                "\n[User Info] %s | IP: %s | Status: %s\n",
                r->username ? r->username : "?",
                r->ip_address ? r->ip_address : "?",
                status_str(r->status));
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

/* ══════════════════════════════════════════════════
   GTK Signal Handlers
   ══════════════════════════════════════════════════ */

/* Send broadcast message */
static void on_send_broadcast(GtkWidget *btn, gpointer data) {
    (void)btn; (void)data;
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry_msg));
    if (!text || strlen(text) == 0) return;

    MessageGeneral mg = {0};
    mg.message        = (char*)text;
    mg.status         = g_status;
    mg.username_origin = g_username;
    mg.ip              = g_ip;
    size_t len; uint8_t *buf = serialize_message_general(&mg, &len);
    send_frame(server_fd, MSG_GENERAL, buf, (uint32_t)len);
    free(buf);
    gtk_entry_set_text(GTK_ENTRY(entry_msg), "");
}

/* Send DM */
static void on_send_dm(GtkWidget *btn, gpointer data) {
    (void)btn; (void)data;
    const char *dest = gtk_entry_get_text(GTK_ENTRY(entry_dest));
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry_msg));
    if (!dest || strlen(dest) == 0) {
        append_text(chat_view, "\n[CLIENT] Enter a destination username for DM.\n");
        return;
    }
    if (!text || strlen(text) == 0) return;

    MessageDm dm = {0};
    dm.message     = (char*)text;
    dm.status      = g_status;
    dm.username_des = (char*)dest;
    dm.ip          = g_ip;
    size_t len; uint8_t *buf = serialize_message_dm(&dm, &len);
    send_frame(server_fd, MSG_DM, buf, (uint32_t)len);
    free(buf);

    append_text(dm_view, "\n[DM to ");
    append_text(dm_view, dest);
    append_text(dm_view, "]: ");
    append_text(dm_view, text);
    append_text(dm_view, "\n");
    gtk_entry_set_text(GTK_ENTRY(entry_msg), "");
}

/* List users */
static void on_list_users(GtkWidget *btn, gpointer data) {
    (void)btn; (void)data;
    ListUsers lu = {0};
    lu.username = g_username;
    lu.ip       = g_ip;
    size_t len; uint8_t *buf = serialize_list_users(&lu, &len);
    send_frame(server_fd, MSG_LIST_USERS, buf, (uint32_t)len);
    free(buf);
}

/* Get user info dialog */
static void on_get_user_info(GtkWidget *btn, gpointer data) {
    (void)btn; (void)data;
    /* Use a simple dialog to ask username */
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Get User Info", GTK_WINDOW(window),
        GTK_DIALOG_MODAL,
        "_OK",     GTK_RESPONSE_OK,
        "_Cancel", GTK_RESPONSE_CANCEL,
        NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *entry   = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Username to query");
    gtk_container_add(GTK_CONTAINER(content), entry);
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const char *quser = gtk_entry_get_text(GTK_ENTRY(entry));
        if (quser && strlen(quser) > 0) {
            GetUserInfo gui = {0};
            gui.username_des = (char*)quser;
            gui.username     = g_username;
            gui.ip           = g_ip;
            size_t len2; uint8_t *buf = serialize_get_user_info(&gui, &len2);
            send_frame(server_fd, MSG_GET_USER_INFO, buf, (uint32_t)len2);
            free(buf);
        }
    }
    gtk_widget_destroy(dialog);
}

/* Change status */
static void on_change_status(GtkWidget *btn, gpointer data) {
    (void)btn; (void)data;
    /* Cycle through statuses */
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Change Status", GTK_WINDOW(window),
        GTK_DIALOG_MODAL,
        "_OK",     GTK_RESPONSE_OK,
        "_Cancel", GTK_RESPONSE_CANCEL,
        NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *combo   = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "ACTIVE");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "DO_NOT_DISTURB");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "INVISIBLE");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), (int)g_status);
    gtk_container_add(GTK_CONTAINER(content), combo);
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
        if (idx >= 0) {
            g_status = (StatusEnum)idx;
            ChangeStatus cs = {0};
            cs.status   = g_status;
            cs.username = g_username;
            cs.ip       = g_ip;
            size_t len2; uint8_t *buf = serialize_change_status(&cs, &len2);
            send_frame(server_fd, MSG_CHANGE_STATUS, buf, (uint32_t)len2);
            free(buf);
            /* Update local status label */
            char slabel[128];
            snprintf(slabel, sizeof(slabel), "Status: %s", status_str(g_status));
            gtk_label_set_text(GTK_LABEL(status_label), slabel);
        }
    }
    gtk_widget_destroy(dialog);
}

/* Help dialog */
static void on_help(GtkWidget *btn, gpointer data) {
    (void)btn; (void)data;
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "Chat Help\n\n"
        "• Broadcast: type message, click 'Send'\n"
        "• DM: type destination username, type message, click 'Send DM'\n"
        "• List Users: click 'List Users'\n"
        "• User Info: click 'User Info'\n"
        "• Change Status: click 'Status'\n"
        "• Quit: close the window\n"
    );
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* Quit */
static void on_quit(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;
    if (server_fd >= 0) {
        Quit q = {0}; q.quit = 1; q.ip = g_ip;
        size_t len; uint8_t *buf = serialize_quit(&q, &len);
        send_frame(server_fd, MSG_QUIT, buf, (uint32_t)len);
        free(buf);
        close(server_fd);
    }
    gtk_main_quit();
}

/* Enter key on message entry sends broadcast */
static void on_entry_activate(GtkEntry *entry, gpointer data) {
    (void)entry; (void)data;
    on_send_broadcast(NULL, NULL);
}

/* ══════════════════════════════════════════════════
   Build GTK UI
   ══════════════════════════════════════════════════ */
static void build_ui(void) {
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char title[128];
    snprintf(title, sizeof(title), "Chat – %s [%s]", g_username, status_str(g_status));
    gtk_window_set_title(GTK_WINDOW(window), title);
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);
    g_signal_connect(window, "destroy", G_CALLBACK(on_quit), NULL);

    /* Main vertical box */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 6);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    /* ── Toolbar ── */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    status_label = gtk_label_new("Status: ACTIVE");
    gtk_box_pack_start(GTK_BOX(toolbar), status_label, FALSE, FALSE, 0);

    GtkWidget *btn_list   = gtk_button_new_with_label("List Users");
    GtkWidget *btn_info   = gtk_button_new_with_label("User Info");
    GtkWidget *btn_status = gtk_button_new_with_label("Status");
    GtkWidget *btn_help   = gtk_button_new_with_label("Help");
    GtkWidget *btn_quit   = gtk_button_new_with_label("Quit");
    g_signal_connect(btn_list,   "clicked", G_CALLBACK(on_list_users),    NULL);
    g_signal_connect(btn_info,   "clicked", G_CALLBACK(on_get_user_info), NULL);
    g_signal_connect(btn_status, "clicked", G_CALLBACK(on_change_status), NULL);
    g_signal_connect(btn_help,   "clicked", G_CALLBACK(on_help),          NULL);
    g_signal_connect(btn_quit,   "clicked", G_CALLBACK(on_quit),          NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), btn_quit,   FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(toolbar), btn_help,   FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(toolbar), btn_status, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(toolbar), btn_info,   FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(toolbar), btn_list,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    /* ── Paned: left = chat+dm, right = users ── */
    GtkWidget *hpane = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), hpane, TRUE, TRUE, 0);

    /* Left pane: stacked chat + DM */
    GtkWidget *left_vpane = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(hpane), left_vpane, TRUE, TRUE);

    /* General chat view */
    chat_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(chat_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(chat_view), GTK_WRAP_WORD_CHAR);
    GtkWidget *chat_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(chat_scroll), chat_view);
    GtkWidget *chat_frame  = gtk_frame_new("General Chat");
    gtk_container_add(GTK_CONTAINER(chat_frame), chat_scroll);
    gtk_paned_pack1(GTK_PANED(left_vpane), chat_frame, TRUE, TRUE);

    /* DM view */
    dm_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(dm_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(dm_view), GTK_WRAP_WORD_CHAR);
    GtkWidget *dm_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(dm_scroll), dm_view);
    GtkWidget *dm_frame  = gtk_frame_new("Direct Messages");
    gtk_container_add(GTK_CONTAINER(dm_frame), dm_scroll);
    gtk_paned_pack2(GTK_PANED(left_vpane), dm_frame, TRUE, TRUE);

    /* Right pane: users list */
    users_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(users_view), FALSE);
    GtkWidget *users_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(users_scroll), users_view);
    GtkWidget *users_frame  = gtk_frame_new("Online Users");
    gtk_container_add(GTK_CONTAINER(users_frame), users_scroll);
    gtk_paned_pack2(GTK_PANED(hpane), users_frame, FALSE, TRUE);
    gtk_paned_set_position(GTK_PANED(hpane), 620);

    /* ── Input area ── */
    GtkWidget *input_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    entry_dest = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_dest), "DM to (username)");
    gtk_widget_set_size_request(entry_dest, 150, -1);
    entry_msg = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_msg), "Type a message...");
    g_signal_connect(entry_msg, "activate", G_CALLBACK(on_entry_activate), NULL);

    GtkWidget *btn_send    = gtk_button_new_with_label("Send");
    GtkWidget *btn_send_dm = gtk_button_new_with_label("Send DM");
    g_signal_connect(btn_send,    "clicked", G_CALLBACK(on_send_broadcast), NULL);
    g_signal_connect(btn_send_dm, "clicked", G_CALLBACK(on_send_dm),        NULL);

    gtk_box_pack_start(GTK_BOX(input_box), entry_dest, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(input_box), entry_msg,  TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(input_box), btn_send,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(input_box), btn_send_dm,FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), input_box, FALSE, FALSE, 0);

    gtk_widget_show_all(window);
}

/* ── Get local IP ── */
static void get_local_ip(const char *server_ip, int port, char *out, size_t olen) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, server_ip, &addr.sin_addr);
    connect(s, (struct sockaddr*)&addr, sizeof(addr));
    struct sockaddr_in local; socklen_t ll = sizeof(local);
    getsockname(s, (struct sockaddr*)&local, &ll);
    inet_ntop(AF_INET, &local.sin_addr, out, olen);
    close(s);
}

/* ── Main ── */
int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    if (argc != 4) {
        fprintf(stderr, "Uso: %s <username> <IP_servidor> <puerto>\n", argv[0]);
        return 1;
    }
    strncpy(g_username, argv[1], 63);
    const char *server_ip   = argv[2];
    int         server_port = atoi(argv[3]);

    /* Determine our local IP */
    get_local_ip(server_ip, server_port, g_ip, sizeof(g_ip));
    printf("[CLIENT] Local IP: %s\n", g_ip);

    /* Connect to server */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in saddr = {0};
    saddr.sin_family = AF_INET;
    saddr.sin_port   = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &saddr.sin_addr) <= 0) {
        /* Try hostname resolution */
        struct hostent *h = gethostbyname(server_ip);
        if (!h) { fprintf(stderr, "Cannot resolve %s\n", server_ip); return 1; }
        memcpy(&saddr.sin_addr, h->h_addr_list[0], h->h_length);
    }
    if (connect(server_fd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
        perror("connect"); return 1;
    }
    printf("[CLIENT] Connected to %s:%d\n", server_ip, server_port);

    /* Register */
    Register reg = {0};
    reg.username = g_username;
    reg.ip       = g_ip;
    size_t rlen; uint8_t *rbuf = serialize_register(&reg, &rlen);
    send_frame(server_fd, MSG_REGISTER, rbuf, (uint32_t)rlen);
    free(rbuf);

    /* Wait for server registration response before showing GUI */
    uint8_t type; uint8_t *payload; int plen;
    plen = recv_frame(server_fd, &type, &payload);
    if (plen < 0 || type != MSG_SERVER_RESPONSE) {
        fprintf(stderr, "[CLIENT] No response from server.\n");
        return 1;
    }
    ServerResponse *resp = deserialize_server_response(payload, plen);
    free(payload);
    if (!resp || !resp->is_successful) {
        fprintf(stderr, "[CLIENT] Registration failed: %s\n",
                resp ? resp->message : "unknown error");
        free_server_response(resp);
        return 1;
    }
    printf("[CLIENT] Registered as '%s'.\n", g_username);
    free_server_response(resp);

    /* Build UI */
    build_ui();

    /* Start recv thread */
    pthread_t rt;
    pthread_create(&rt, NULL, recv_thread, NULL);
    pthread_detach(rt);

    /* Welcome message */
    append_text(chat_view, "Welcome to the chat! Type a message and press Enter or 'Send'.\n");
    append_text(chat_view, "For DMs, fill the 'DM to' field and click 'Send DM'.\n\n");

    gtk_main();
    return 0;
}