/*
 * protocol.c - Kiem tra JSON UTF-8 va tao envelope giao thuc phien ban 1.
 * Luong: validate byte/nesting -> parse cJSON -> validate envelope -> dispatch.
 * cJSON tree/string duoc cap phat rieng va phai giai phong theo API tuong ung.
 */
#include "protocol.h"
#include "frame.h"

#include <stdlib.h>
#include <string.h>

/*
 * protocol_valid_utf8
 * Y nghia : Kiem tra UTF-8, loai NUL, overlong, surrogate va ma vuot Unicode.
 * Dau vao : text - vung byte hop le; length - so byte can kiem tra.
 * Dau ra  : 1 neu hop le, 0 neu sai; khong sua/copy du lieu.
 */
int protocol_valid_utf8(const uint8_t *text, size_t length)
{
    for (size_t i = 0; i < length;) {
        uint32_t value;
        size_t extra;
        uint8_t first = text[i++];
        if (first < 0x80) {
            if (first == 0) {
                return 0;
            }
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            value = first & 0x1fu;
            extra = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            value = first & 0x0fu;
            extra = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            value = first & 0x07u;
            extra = 3;
        } else {
            return 0;
        }
        if (extra > length - i) {
            return 0;
        }
        for (size_t j = 0; j < extra; ++j) {
            uint8_t next = text[i++];
            if ((next & 0xc0u) != 0x80u) {
                return 0;
            }
            value = (value << 6) | (next & 0x3fu);
        }
        if ((extra == 1 && value < 0x80) ||
            (extra == 2 && value < 0x800) ||
            (extra == 3 && value < 0x10000) ||
            (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) {
            return 0;
        }
    }
    return 1;
}

/*
 * skip_json_number
 * Y nghia : Kiem tra ngu phap so JSON chat hon strtod/cJSON (cam 01, 1., 1e).
 * Dau vao : payload/length - JSON; position - vi tri dau token so.
 * Dau ra  : 1 va dua position ve byte cuoi token; 0 khi cu phap/delimiter sai.
 */
static int skip_json_number(const uint8_t *payload, size_t length, size_t *position)
{
    size_t cursor = *position;
    if (payload[cursor] == '-') ++cursor;
    if (cursor == length) return 0;
    if (payload[cursor] == '0') {
        ++cursor;
    } else if (payload[cursor] >= '1' && payload[cursor] <= '9') {
        do { ++cursor; } while (cursor < length && payload[cursor] >= '0' && payload[cursor] <= '9');
    } else {
        return 0;
    }
    if (cursor < length && payload[cursor] == '.') {
        ++cursor;
        if (cursor == length || payload[cursor] < '0' || payload[cursor] > '9') return 0;
        do { ++cursor; } while (cursor < length && payload[cursor] >= '0' && payload[cursor] <= '9');
    }
    if (cursor < length && (payload[cursor] == 'e' || payload[cursor] == 'E')) {
        ++cursor;
        if (cursor < length && (payload[cursor] == '+' || payload[cursor] == '-')) ++cursor;
        if (cursor == length || payload[cursor] < '0' || payload[cursor] > '9') return 0;
        do { ++cursor; } while (cursor < length && payload[cursor] >= '0' && payload[cursor] <= '9');
    }
    if (cursor < length && payload[cursor] != ',' && payload[cursor] != '}' &&
        payload[cursor] != ']' && payload[cursor] != ' ' && payload[cursor] != '\t' &&
        payload[cursor] != '\n' && payload[cursor] != '\r') return 0;
    *position = cursor - 1u;
    return 1;
}

/*
 * valid_structure
 * Y nghia : Gioi han do sau truoc recursive parser; loai NUL escape va control
 *           character, so JSON sai. Bo qua escaped quote/backslash dung cach.
 * Dau vao : payload/length - byte UTF-8 cua JSON, chua can NUL ket thuc.
 * Dau ra  : 1 neu qua kiem tra so bo, 0 neu sai; cJSON se kiem tra cu phap sau.
 */
static int valid_structure(const uint8_t *payload, size_t length)
{
    size_t depth = 0;
    int in_string = 0;
    for (size_t i = 0; i < length; ++i) {
        uint8_t ch = payload[i];
        if (in_string && ch == '\\') {
            if (i + 5u < length && payload[i + 1u] == 'u' &&
                memcmp(payload + i + 2u, "0000", 4) == 0) {
                return 0;
            }
            if (++i == length) {
                return 0;
            }
        } else if (ch == '"') {
            in_string = !in_string;
        } else if (in_string && ch < 0x20) {
            return 0;
        } else if (!in_string && ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') {
            return 0;
        } else if (!in_string && (ch == '-' || (ch >= '0' && ch <= '9'))) {
            if (!skip_json_number(payload, length, &i)) return 0;
        } else if (!in_string && (ch == '{' || ch == '[')) {
            if (++depth > PROTOCOL_MAX_DEPTH) {
                return 0;
            }
        } else if (!in_string && (ch == '}' || ch == ']')) {
            if (depth == 0) {
                return 0;
            }
            --depth;
        }
    }
    return depth == 0 && !in_string;
}

/*
 * valid_token
 * Y nghia : Kiem tra do dai va tap ky tu ASCII cua type/request id.
 * Dau vao : text - chuoi NUL; maximum - gioi han byte; is_type - 1 cho type.
 * Dau ra  : 1 neu hop le, 0 neu rong/qua dai/co ky tu khong cho phep.
 */
static int valid_token(const char *text, size_t maximum, int is_type)
{
    size_t length = strlen(text);
    if (length == 0 || length > maximum) {
        return 0;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (is_type) {
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) {
                return 0;
            }
        } else if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                     (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
                     ch == '.' || ch == ':')) {
            return 0;
        }
    }
    return 1;
}

/*
 * protocol_message_destroy
 * Y nghia : Free cJSON tree va xoa cac pointer muon ben trong message.
 * Dau vao : message - state parse da khoi tao/da parse, khong NULL.
 * Dau ra  : Khong tra ve; message ve rong, pointer type/data/id het hieu luc.
 */
void protocol_message_destroy(protocol_message_t *message)
{
    cJSON_Delete(message->root);
    memset(message, 0, sizeof(*message));
}

/*
 * protocol_parse
 * Y nghia : Parse dung mot JSON object va validate envelope, version, field.
 * Dau vao : payload/length - byte frame; message - state dau ra chua giu tree;
 *           error - noi nhan mo ta loi tinh, caller khong free chuoi nay.
 * Dau ra  : 0 thanh cong (caller destroy message); -1 loi, message rong.
 */
int protocol_parse(const uint8_t *payload, size_t length,
                   protocol_message_t *message, const char **error)
{
    char *copy;
    const char *end = NULL;
    const cJSON *version;
    const cJSON *type;
    const cJSON *request;
    const char *reserved[] = {"version", "type", "request_id", "data"};
    memset(message, 0, sizeof(*message));
    *error = "Invalid UTF-8, NUL, nesting or JSON structure";
    if (payload == NULL || length == 0 || length > FRAME_MAX_PAYLOAD ||
        !protocol_valid_utf8(payload, length) || !valid_structure(payload, length)) {
        return -1;
    }
    copy = malloc(length + 1u);
    if (copy == NULL) {
        *error = "Out of memory";
        return -1;
    }
    memcpy(copy, payload, length);
    copy[length] = '\0';
    message->root = cJSON_ParseWithLengthOpts(copy, length + 1u, &end, 1);
    int parsed = message->root != NULL && end == copy + length;
    free(copy);
    if (!parsed || !cJSON_IsObject(message->root)) {
        *error = "Expected one complete JSON object";
        goto invalid;
    }
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
        size_t count = 0;
        const cJSON *item;
        cJSON_ArrayForEach(item, message->root) {
            if (strcmp(item->string, reserved[i]) == 0) {
                ++count;
            }
        }
        if (count > 1) {
            *error = "Duplicate envelope field";
            goto invalid;
        }
    }
    version = cJSON_GetObjectItemCaseSensitive(message->root, "version");
    type = cJSON_GetObjectItemCaseSensitive(message->root, "type");
    request = cJSON_GetObjectItemCaseSensitive(message->root, "request_id");
    message->data = cJSON_GetObjectItemCaseSensitive(message->root, "data");
    if (!cJSON_IsNumber(version) || version->valuedouble != PROTOCOL_VERSION) {
        *error = "Unsupported or missing protocol version";
        goto invalid;
    }
    if (!cJSON_IsString(type) || !valid_token(type->valuestring, PROTOCOL_MAX_TYPE, 1)) {
        *error = "Invalid message type";
        goto invalid;
    }
    message->type = type->valuestring;
    if (request == NULL && strcmp(message->type, MSG_ERROR) == 0) {
        message->request_id = NULL;
    } else if (!cJSON_IsString(request) ||
               !valid_token(request->valuestring, PROTOCOL_MAX_REQUEST_ID, 0)) {
        *error = "Invalid or missing request_id";
        goto invalid;
    } else {
        message->request_id = request->valuestring;
    }
    if (!cJSON_IsObject(message->data)) {
        *error = "data must be an object";
        goto invalid;
    }
    *error = NULL;
    return 0;

invalid:
    protocol_message_destroy(message);
    return -1;
}

/*
 * protocol_build
 * Y nghia : Tao JSON envelope compact; copy data de khong lay ownership caller.
 * Dau vao : type - ten lenh; request_id - id hoac NULL cho loi chua xac dinh;
 *           data - object hop le hoac NULL de tao object rong.
 * Dau ra  : Chuoi cap phat moi (caller cJSON_free), NULL neu het bo nho.
 */
char *protocol_build(const char *type, const char *request_id, const cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *body = data == NULL ? cJSON_CreateObject() : cJSON_Duplicate(data, 1);
    if (root == NULL || body == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(body);
        return NULL;
    }
    if (cJSON_AddNumberToObject(root, "version", PROTOCOL_VERSION) == NULL ||
        cJSON_AddStringToObject(root, "type", type) == NULL ||
        (request_id != NULL && cJSON_AddStringToObject(root, "request_id", request_id) == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(body);
        return NULL;
    }
    if (!cJSON_AddItemToObject(root, "data", body)) {
        cJSON_Delete(root);
        cJSON_Delete(body);
        return NULL;
    }
    char *serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return serialized;
}

/*
 * protocol_error
 * Y nghia : Tao SYS_ERROR voi code/message trong data.
 * Dau vao : request_id - id lien quan hoac NULL; code/detail - chuoi NUL.
 * Dau ra  : JSON moi (caller cJSON_free), NULL neu cap phat that bai.
 */
char *protocol_error(const char *request_id, const char *code, const char *detail)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL || cJSON_AddStringToObject(data, "code", code) == NULL ||
        cJSON_AddStringToObject(data, "message", detail) == NULL) {
        cJSON_Delete(data);
        return NULL;
    }
    char *result = protocol_build(MSG_ERROR, request_id, data);
    cJSON_Delete(data);
    return result;
}
