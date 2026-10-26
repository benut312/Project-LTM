/*
 * test_protocol.c - Kiem thu envelope, UTF-8, NUL, nesting va memory ownership.
 * Chay: make unit-test; assert that bai lam process tra ve loi.
 */
#include "protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * expect_invalid
 * Y nghia : Assert parser tu choi payload va khong de lai tree can free.
 * Dau vao : text/length - byte JSON can thu, khong can NUL ket thuc.
 * Dau ra  : Khong tra ve; assert neu parse thanh cong hoac de lai state ban.
 */
static void expect_invalid(const char *text, size_t length)
{
    protocol_message_t message;
    const char *error = NULL;
    assert(protocol_parse((const uint8_t *)text, length, &message, &error) == -1);
    assert(error != NULL && message.root == NULL);
}

/*
 * valid_and_invalid_messages
 * Y nghia : Thu envelope chuan, trailing JSON, sai field/version, duplicate/NUL.
 * Dau vao : Khong co.
 * Dau ra  : Khong tra ve; assert neu hop dong parse/ownership khong dung.
 */
static void valid_and_invalid_messages(void)
{
    const char *valid = "{\"version\":1,\"type\":\"ECHO\",\"request_id\":\"r-1\",\"data\":{\"text\":\"Xin chào 😀\"}}  ";
    protocol_message_t message;
    const char *error = NULL;
    assert(protocol_parse((const uint8_t *)valid, strlen(valid), &message, &error) == 0);
    assert(error == NULL && strcmp(message.type, MSG_ECHO) == 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(message.data, "text")->valuestring,
                  "Xin chào 😀") == 0);
    protocol_message_destroy(&message);
    const char *bad[] = {
        "", "{}", "[]", "{", "{}{}", "{} trailing",
        "{\"version\":2,\"type\":\"ECHO\",\"request_id\":\"r\",\"data\":{}}",
        "{\"version\":1,\"type\":\"ECHO\",\"request_id\":1,\"data\":{}}",
        "{\"version\":1,\"type\":\"ECHO\",\"request_id\":\"\",\"data\":{}}",
        "{\"version\":1,\"type\":\"echo\",\"request_id\":\"r\",\"data\":{}}",
        "{\"version\":1,\"type\":\"ECHO\",\"request_id\":\"r\",\"data\":[]}",
        "{\"version\":1,\"type\":\"ECHO\",\"type\":\"SYS_PING\",\"request_id\":\"r\",\"data\":{}}",
        "{\"version\":1,\"type\":\"ECHO\",\"request_id\":\"r\",\"data\":{\"x\":\"\\u0000\"}}",
        "{\"version\":1,\"type\":\"ECHO\",\"request_id\":\"r\",\"data\":{}} {}",
        "{\"version\":01,\"type\":\"ECHO\",\"request_id\":\"r\",\"data\":{}}",
        "{\"version\":1.,\"type\":\"ECHO\",\"request_id\":\"r\",\"data\":{}}",
        "{\"version\":1,\1\"type\":\"ECHO\",\"request_id\":\"r\",\"data\":{}}"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        expect_invalid(bad[i], strlen(bad[i]));
    }
    const char raw_nul[] = "{\"version\":1}\0garbage";
    expect_invalid(raw_nul, sizeof(raw_nul) - 1u);
    /* Literal backslash-u is text, not an embedded NUL. */
    const char *escaped = "{\"version\":1,\"type\":\"ECHO\",\"request_id\":\"r\",\"data\":{\"x\":\"\\\\u0000\"}}";
    assert(protocol_parse((const uint8_t *)escaped, strlen(escaped), &message, &error) == 0);
    protocol_message_destroy(&message);

    const char *numbers = "{\"version\":1.0,\"type\":\"ECHO\",\"request_id\":\"nums\",\"data\":{\"a\":[-12.5e+2,0,1E-3]}}";
    assert(protocol_parse((const uint8_t *)numbers, strlen(numbers), &message, &error) == 0);
    protocol_message_destroy(&message);

    char nested[100];
    memset(nested, '[', 40);
    memset(nested + 40, ']', 40);
    expect_invalid(nested, 80);
}

/*
 * unicode_and_builders
 * Y nghia : Thu UTF-8 loi, error khong request id va builder copy data.
 * Dau vao : Khong co.
 * Dau ra  : Khong tra ve; assert neu parser/builder lam sai du lieu/ownership.
 */
static void unicode_and_builders(void)
{
    const uint8_t invalid[][4] = {{0xc0, 0xaf, 0, 0}, {0xed, 0xa0, 0x80, 0},
                                  {0xf4, 0x90, 0x80, 0x80}, {0xe2, 0x82, 0, 0}};
    const size_t lengths[] = {2, 3, 4, 2};
    for (size_t i = 0; i < 4; ++i) assert(!protocol_valid_utf8(invalid[i], lengths[i]));
    const uint8_t valid[] = {0xc2, 0xa2, 0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x98, 0x80};
    assert(protocol_valid_utf8(valid, sizeof(valid)));
    char *json = protocol_error(NULL, "INVALID_MESSAGE", "Bad input");
    assert(json != NULL);
    protocol_message_t message;
    const char *error = NULL;
    assert(protocol_parse((const uint8_t *)json, strlen(json), &message, &error) == 0);
    assert(message.request_id == NULL && strcmp(message.type, MSG_ERROR) == 0);
    protocol_message_destroy(&message);
    cJSON_free(json);
    cJSON *data = cJSON_CreateObject();
    assert(cJSON_AddStringToObject(data, "text", "retained") != NULL);
    json = protocol_build(MSG_ECHO, "r-2", data);
    cJSON_Delete(data);
    assert(json != NULL);
    assert(protocol_parse((const uint8_t *)json, strlen(json), &message, &error) == 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(message.data, "text")->valuestring,
                  "retained") == 0);
    protocol_message_destroy(&message);
    cJSON_free(json);
}

/*
 * main
 * Y nghia : Chay cac ca protocol va thong bao PASS.
 * Dau vao : Khong co.
 * Dau ra  : 0 neu moi assert dung; process that bai neu assert sai.
 */
int main(void)
{
    valid_and_invalid_messages();
    unicode_and_builders();
    puts("protocol tests: PASS");
    return 0;
}
