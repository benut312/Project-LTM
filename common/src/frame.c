/*
 * frame.c - Dong goi va tach frame TCP dung chung client/server.
 * Luong: tich luy header big endian -> payload -> callback;
 *        enqueue wire frame -> gui tung phan -> advance offset.
 * Module khong goi socket; transport phai bao lai so byte thuc su da gui.
 */
#include "frame.h"

#include <stdlib.h>
#include <string.h>

struct frame_tx_node {
    struct frame_tx_node *next;
    size_t length;
    size_t offset;
    uint8_t bytes[];
};

/*
 * frame_decoder_init
 * Y nghia : Khoi tao decoder chua co header/payload.
 * Dau vao : decoder - vung state hop le, chua giu payload can giai phong.
 * Dau ra  : Khong tra ve; state ve rong, khong cap phat bo nho.
 */
void frame_decoder_init(frame_decoder_t *decoder)
{
    memset(decoder, 0, sizeof(*decoder));
}

/*
 * frame_decoder_destroy
 * Y nghia : Giai phong payload dang nhan va reset decoder, co the goi lai.
 * Dau vao : decoder - state da khoi tao, khong NULL.
 * Dau ra  : Khong tra ve; decoder rong va co the tai su dung.
 */
void frame_decoder_destroy(frame_decoder_t *decoder)
{
    free(decoder->payload);
    frame_decoder_init(decoder);
}

/*
 * frame_decoder_in_progress
 * Y nghia : Kiem tra con frame chua nhan du de quan ly EOF/deadline.
 * Dau vao : decoder - state da khoi tao, khong NULL.
 * Dau ra  : 1 neu da nhan mot phan header/payload; 0 neu o ranh gioi frame.
 */
int frame_decoder_in_progress(const frame_decoder_t *decoder)
{
    return decoder->header_used != 0;
}

/*
 * frame_decoder_feed
 * Y nghia : Tich luy byte, tach moi frame day du va goi handler theo thu tu.
 * Dau vao : decoder - state rieng cho connection; data/length - byte vua doc.
 *           handler/context - callback va du lieu ngu canh; callback khong giu
 *           pointer payload sau khi tra ve, phai copy neu can su dung tiep.
 * Dau ra  : FRAME_OK hoac ma loi FRAME_*; frame loi/callback loi duoc reset.
 *           Frame chua du duoc giu lai de tiep tuc o lan feed sau.
 */
int frame_decoder_feed(frame_decoder_t *decoder, const void *data, size_t length,
                       frame_handler_fn handler, void *context)
{
    const uint8_t *input = data;

    if (handler == NULL || (length != 0 && data == NULL)) {
        return FRAME_INVALID;
    }
    while (length != 0) {
        if (decoder->header_used < FRAME_HEADER_SIZE) {
            size_t take = FRAME_HEADER_SIZE - decoder->header_used;
            if (take > length) {
                take = length;
            }
            memcpy(decoder->header + decoder->header_used, input, take);
            decoder->header_used += take;
            input += take;
            length -= take;
            if (decoder->header_used != FRAME_HEADER_SIZE) {
                continue;
            }
            decoder->payload_length = ((uint32_t)decoder->header[0] << 24) |
                                      ((uint32_t)decoder->header[1] << 16) |
                                      ((uint32_t)decoder->header[2] << 8) |
                                      (uint32_t)decoder->header[3];
            if (decoder->payload_length == 0 ||
                decoder->payload_length > FRAME_MAX_PAYLOAD) {
                frame_decoder_destroy(decoder);
                return FRAME_INVALID;
            }
            decoder->payload = malloc((size_t)decoder->payload_length + 1u);
            if (decoder->payload == NULL) {
                frame_decoder_destroy(decoder);
                return FRAME_NO_MEMORY;
            }
        }
        if (length != 0) {
            size_t take = decoder->payload_length - decoder->payload_used;
            if (take > length) {
                take = length;
            }
            memcpy(decoder->payload + decoder->payload_used, input, take);
            decoder->payload_used += take;
            input += take;
            length -= take;
        }
        if (decoder->payload_used == decoder->payload_length) {
            int handled;
            decoder->payload[decoder->payload_length] = '\0';
            handled = handler(decoder->payload, decoder->payload_length, context);
            frame_decoder_destroy(decoder);
            if (handled != 0) {
                return FRAME_HANDLER_ERROR;
            }
        }
    }
    return FRAME_OK;
}

/*
 * frame_tx_init
 * Y nghia : Khoi tao hang doi gui voi ngan sach wire byte toi da.
 * Dau vao : queue - state rong/chua khoi tao; limit - gioi han byte, gom header.
 * Dau ra  : Khong tra ve; queue rong, chua cap phat node.
 */
void frame_tx_init(frame_tx_queue_t *queue, size_t limit)
{
    memset(queue, 0, sizeof(*queue));
    queue->limit = limit;
}

/*
 * frame_tx_destroy
 * Y nghia : Giai phong moi frame chua gui het; giu nguyen cau hinh limit.
 * Dau vao : queue - hang doi da khoi tao, khong NULL.
 * Dau ra  : Khong tra ve; head/tail va cac bo dem ve rong.
 */
void frame_tx_destroy(frame_tx_queue_t *queue)
{
    size_t limit = queue->limit;
    while (queue->head != NULL) {
        frame_tx_node_t *next = queue->head->next;
        free(queue->head);
        queue->head = next;
    }
    frame_tx_init(queue, limit);
}

/*
 * frame_tx_push
 * Y nghia : Copy header va payload thanh mot wire frame, them vao cuoi queue.
 * Dau vao : queue - state TX; payload - byte can gui; length - 1..65536 byte.
 * Dau ra  : FRAME_OK, FRAME_INVALID, FRAME_QUEUE_FULL hoac FRAME_NO_MEMORY.
 *           Queue so huu ban copy; caller van so huu payload dau vao.
 */
int frame_tx_push(frame_tx_queue_t *queue, const void *payload, size_t length)
{
    frame_tx_node_t *node;
    size_t wire_length;
    if (payload == NULL || length == 0 || length > FRAME_MAX_PAYLOAD) {
        return FRAME_INVALID;
    }
    wire_length = FRAME_HEADER_SIZE + length;
    if (queue->allocated_bytes > queue->limit ||
        wire_length > queue->limit - queue->allocated_bytes ||
        queue->frame_count >= FRAME_MAX_QUEUED) {
        return FRAME_QUEUE_FULL;
    }
    node = malloc(sizeof(*node) + wire_length);
    if (node == NULL) {
        return FRAME_NO_MEMORY;
    }
    node->next = NULL;
    node->length = wire_length;
    node->offset = 0;
    node->bytes[0] = (uint8_t)(length >> 24);
    node->bytes[1] = (uint8_t)(length >> 16);
    node->bytes[2] = (uint8_t)(length >> 8);
    node->bytes[3] = (uint8_t)length;
    memcpy(node->bytes + FRAME_HEADER_SIZE, payload, length);
    if (queue->tail == NULL) {
        queue->head = node;
    } else {
        queue->tail->next = node;
    }
    queue->tail = node;
    queue->allocated_bytes += wire_length;
    queue->frame_count++;
    return FRAME_OK;
}

/*
 * frame_tx_peek
 * Y nghia : Lay phan wire frame dau tien chua gui, khong thay doi queue.
 * Dau vao : queue - state TX; data - noi nhan pointer, khong duoc free/sua.
 * Dau ra  : So byte con lai; 0 va *data=NULL neu rong. Pointer chi hop le den
 *           lan advance/destroy tiep theo lam giai phong node hien tai.
 */
size_t frame_tx_peek(const frame_tx_queue_t *queue, const uint8_t **data)
{
    if (queue->head == NULL) {
        *data = NULL;
        return 0;
    }
    *data = queue->head->bytes + queue->head->offset;
    return queue->head->length - queue->head->offset;
}

/*
 * frame_tx_advance
 * Y nghia : Ghi nhan byte transport da gui; chi free node khi gui het frame.
 * Dau vao : queue - TX da khoi tao; written - byte thuc su gui cua node dau.
 * Dau ra  : FRAME_OK hoac FRAME_INVALID neu vuot phan con lai/queue rong.
 *           allocated_bytes chi giam khi node duoc giai phong hoan toan.
 */
int frame_tx_advance(frame_tx_queue_t *queue, size_t written)
{
    frame_tx_node_t *node = queue->head;
    if (node == NULL || written > node->length - node->offset) {
        return FRAME_INVALID;
    }
    node->offset += written;
    if (node->offset == node->length) {
        queue->head = node->next;
        if (queue->head == NULL) {
            queue->tail = NULL;
        }
        queue->allocated_bytes -= node->length;
        queue->frame_count--;
        free(node);
    }
    return FRAME_OK;
}
