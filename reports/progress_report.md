---
marp: true
paginate: true
title: Chat LTM — Báo cáo giai đoạn 1
---

# Chat LTM

Báo cáo giai đoạn 1 — 26/10/2026

C11 · POSIX TCP · Ubuntu 22.04/WSL2

- Hoàn thành nền tảng echo/ping nhiều client.

---

# Kiến trúc nền tảng

```text
Client terminal ── framed JSON/TCP ── Server poll loop
       │                                  │
       └──── common frame/protocol/net ────┘
                          │
                RX decoder + TX queue riêng/client
```

- Nonblocking cả listener và accepted sockets.
- POLLIN đọc theo quota; POLLOUT gửi tiếp phần còn lại.
- Timeout, giới hạn queue/client, cleanup EOF/RST/SIGTERM.
- GTK3 smoke kiểm tra WSLg; GUI chat thuộc mốc sau.

---

# Xử lý truyền dòng

```text
4 byte length (big endian) | N byte JSON UTF-8
```

- TCP là dòng byte: header/payload có thể bị chia hoặc gộp.
- Decoder giữ frame dở, chỉ dispatch frame đủ, max 64 KiB.
- TX giữ offset khi short write/EAGAIN, không gửi lại prefix.
- Client chậm bị giới hạn, các client khác tiếp tục chạy.

---

# Demo

```bash
make
./bin/ltm-server
```

Terminal Ubuntu khác:

```bash
./bin/ltm-client -m 'Xin chào Việt Nam 😀'
./bin/ltm-client -P
./bin/ltm-client
# /ping, /quit
```

ECHO trả nguyên payload; SYS_PING trả SYS_PONG cùng request_id.

---

# Kết quả kiểm thử

- 3 bộ C: frame, protocol, socket partial I/O — PASS.
- 15/15 TCP integration — PASS.
- 16 client × 20 request, không lẫn dữ liệu.
- Frame max, chia/gộp, JSON/UTF-8 lỗi, EOF/RST/half-close.
- Slow reader, TX cap, frame/write timeout, tái dùng slot.
- ASan/UBSan chạy cùng bộ test — PASS.
- GTK3 3.24.33 native window qua WSLg — PASS.

---

# Bài học và mốc tiếp theo

- send thành công không có nghĩa đã gửi đủ byte hay app đích đã nhận.
- RX/TX phải có state riêng; kiểm tra cú pháp trước khi tin payload.
- GĐ 2: MySQL + worker, SHA-256+salt, auth/session, online list, log/heartbeat.
- Tiếp theo: DM có persist/ACK, kết bạn, invitation nhóm, GUI từng bước.
- Điểm chấm thực tế phụ thuộc nghiệm thu; giai đoạn này chưa có chat nghiệp vụ.
