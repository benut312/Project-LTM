#ifndef LTM_PROTOCOL_H
#define LTM_PROTOCOL_H

#include <cJSON.h>
#include <stddef.h>
#include <stdint.h>

#define PROTOCOL_VERSION 1
#define PROTOCOL_MAX_TYPE 48u
#define PROTOCOL_MAX_REQUEST_ID 64u
#define PROTOCOL_MAX_DEPTH 32u

#define MSG_ECHO "ECHO"
#define MSG_PING "SYS_PING"
#define MSG_PONG "SYS_PONG"
#define MSG_ERROR "SYS_ERROR"

typedef struct {
    cJSON *root;
    const char *type;
    const char *request_id;
    const cJSON *data;
} protocol_message_t;

/* On failure, message is empty and error points to a static description. */
int protocol_parse(const uint8_t *payload, size_t length,
                   protocol_message_t *message, const char **error);
void protocol_message_destroy(protocol_message_t *message);
int protocol_valid_utf8(const uint8_t *text, size_t length);

/* Returned strings belong to the caller and must be released with cJSON_free. */
char *protocol_build(const char *type, const char *request_id, const cJSON *data);
char *protocol_error(const char *request_id, const char *code, const char *detail);

#endif
