#pragma once
#include <stdint.h>
#include <stddef.h>

/* protobuf_c_boolean compatibility */
typedef int protobuf_c_boolean;

/* ─── StatusEnum ─── */
typedef enum {
    STATUS_ENUM__ACTIVE          = 0,
    STATUS_ENUM__DO_NOT_DISTURB  = 1,
    STATUS_ENUM__INVISIBLE       = 2
} StatusEnum;

/* ─── Message type IDs (wire protocol) ─── */
#define MSG_REGISTER           1
#define MSG_GENERAL            2
#define MSG_DM                 3
#define MSG_CHANGE_STATUS      4
#define MSG_LIST_USERS         5
#define MSG_GET_USER_INFO      6
#define MSG_QUIT               7
#define MSG_SERVER_RESPONSE   10
#define MSG_ALL_USERS         11
#define MSG_FOR_DM            12
#define MSG_BROADCAST         13
#define MSG_USER_INFO_RESP    14

/* ═══════════════════════════════════════════
   CLIENT → SERVER structs
   ═══════════════════════════════════════════ */

/* type 1 */
typedef struct {
    char             *username;
    char             *ip;
} Register;

/* type 2 */
typedef struct {
    char             *message;
    StatusEnum        status;
    char             *username_origin;
    char             *ip;
} MessageGeneral;

/* type 3 */
typedef struct {
    char             *message;
    StatusEnum        status;
    char             *username_des;
    char             *ip;
} MessageDm;

/* type 4 */
typedef struct {
    StatusEnum        status;
    char             *username;
    char             *ip;
} ChangeStatus;

/* type 5 */
typedef struct {
    char             *username;
    char             *ip;
} ListUsers;

/* type 6 */
typedef struct {
    char             *username_des;
    char             *username;
    char             *ip;
} GetUserInfo;

/* type 7 */
typedef struct {
    protobuf_c_boolean quit;
    char              *ip;
} Quit;

/* ═══════════════════════════════════════════
   SERVER → CLIENT structs
   ═══════════════════════════════════════════ */

/* type 10 */
typedef struct {
    int32_t            status_code;
    char              *message;
    protobuf_c_boolean is_successful;
} ServerResponse;

/* type 11 */
typedef struct {
    size_t            n_usernames;
    char            **usernames;
    size_t            n_status;
    StatusEnum       *status;
} AllUsers;

/* type 12 */
typedef struct {
    char             *username_des;
    char             *message;
    char             *username_origin;  /* optional */
} ForDm;

/* type 13 */
typedef struct {
    char             *message;
    char             *username_origin;
} BroadcastMessages;

/* type 14 */
typedef struct {
    char             *ip_address;
    char             *username;
    StatusEnum        status;
} GetUserInfoResponse;