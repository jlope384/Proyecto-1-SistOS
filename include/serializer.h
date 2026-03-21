#pragma once
#include "proto_msgs.h"
#include <stdint.h>
#include <stdlib.h>

/* ── Serialize helpers: return heap-allocated bytes, set *len. Caller frees. ── */
uint8_t *serialize_register(const Register *m, size_t *len);
uint8_t *serialize_message_general(const MessageGeneral *m, size_t *len);
uint8_t *serialize_message_dm(const MessageDm *m, size_t *len);
uint8_t *serialize_change_status(const ChangeStatus *m, size_t *len);
uint8_t *serialize_list_users(const ListUsers *m, size_t *len);
uint8_t *serialize_get_user_info(const GetUserInfo *m, size_t *len);
uint8_t *serialize_quit(const Quit *m, size_t *len);

uint8_t *serialize_server_response(const ServerResponse *m, size_t *len);
uint8_t *serialize_all_users(const AllUsers *m, size_t *len);
uint8_t *serialize_for_dm(const ForDm *m, size_t *len);
uint8_t *serialize_broadcast(const BroadcastMessages *m, size_t *len);
uint8_t *serialize_user_info_resp(const GetUserInfoResponse *m, size_t *len);

/* ── Deserialize helpers: return heap-allocated struct. Caller frees with free(). ── */
Register            *deserialize_register(const uint8_t *buf, size_t len);
MessageGeneral      *deserialize_message_general(const uint8_t *buf, size_t len);
MessageDm           *deserialize_message_dm(const uint8_t *buf, size_t len);
ChangeStatus        *deserialize_change_status(const uint8_t *buf, size_t len);
ListUsers           *deserialize_list_users(const uint8_t *buf, size_t len);
GetUserInfo         *deserialize_get_user_info(const uint8_t *buf, size_t len);
Quit                *deserialize_quit(const uint8_t *buf, size_t len);

ServerResponse      *deserialize_server_response(const uint8_t *buf, size_t len);
AllUsers            *deserialize_all_users(const uint8_t *buf, size_t len);
ForDm               *deserialize_for_dm(const uint8_t *buf, size_t len);
BroadcastMessages   *deserialize_broadcast(const uint8_t *buf, size_t len);
GetUserInfoResponse *deserialize_user_info_resp(const uint8_t *buf, size_t len);

/* Free functions */
void free_register(Register *m);
void free_message_general(MessageGeneral *m);
void free_message_dm(MessageDm *m);
void free_change_status(ChangeStatus *m);
void free_list_users(ListUsers *m);
void free_get_user_info(GetUserInfo *m);
void free_quit(Quit *m);
void free_server_response(ServerResponse *m);
void free_all_users(AllUsers *m);
void free_for_dm(ForDm *m);
void free_broadcast(BroadcastMessages *m);
void free_user_info_resp(GetUserInfoResponse *m);