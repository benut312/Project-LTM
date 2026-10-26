# Kế hoạch triển khai ứng dụng Chat — Lập trình mạng

> **Phiên bản:** 2.0 — cập nhật ngày 26/10/2026 sau review.  
> **Nhóm:** 2 người; thời gian dự kiến 8 tuần.  
> **Môi trường đã chọn:** Linux Ubuntu 22.04 qua WSL2; C11/GCC, POSIX TCP, MySQL, GTK3.  
> **Mục tiêu rubric:** 23 điểm cơ bản + 2–5 điểm bổ sung; điểm thực tế phụ thuộc nghiệm thu.  
> **Quyết định người dùng:** giữ SHA-256; dùng Linux qua WSL để sát bài giảng.

## 1. Phạm vi và quyết định thiết kế

Ứng dụng client–server bằng C, TCP length prefix + JSON UTF-8. Server quản lý tài khoản, phiên, presence, bạn bè, chat 1-1, nhóm và dữ liệu offline. Client cuối cùng có GUI GTK3; client terminal phục vụ phát triển/kiểm thử từ giai đoạn 1.

WSL là cách chạy Linux trên máy Windows của nhóm, không phải thành phần của giao thức. Có thể build trên máy/VM Ubuntu tương thích. Chạy trực tiếp Windows cần port socket sang Winsock và điều chỉnh build, ngoài phạm vi hiện tại.

| Vấn đề | Quyết định |
|---|---|
| Nhiều phiên | Một phiên/account; login mới thay phiên cũ. |
| Chat với người chưa kết bạn | Cho phép mọi user hợp lệ; kiểm tra quyền tại server. |
| Danh sách online | Sau login cung cấp **mọi user khác đang online**, độc lập danh sách bạn bè. |
| Dừng/chuyển chat | `DM_END` báo peer và kết thúc context; chuyển chat gửi END cũ rồi OPEN mới; giữ TCP/login. |
| Tin ở cuộc chat chưa mở | Vẫn lưu, nhận và cập nhật unread; context không quyết định quyền nhận offline. |
| Thêm vào nhóm | Owner mời; invitee accept mới có membership; hỗ trợ reject và invitation offline. |
| Owner rời | Chuyển cho member tham gia lâu nhất, phá hòa bằng membership id; không còn ai thì xóa nhóm; transaction. |
| Lịch sử nhóm | Chỉ từ lần tham gia hiện tại, theo `joined_after_message_id`; rejoin tạo mốc mới. |
| Delivered | Commit trước fan-out; chỉ đánh dấu delivered sau ACK client nhận, khác đã lưu/đã đọc. |
| Mật khẩu | **SHA-256 qua OpenSSL EVP**, salt ngẫu nhiên 16 byte/account; lưu salt/hash hex; không dùng hash cố định phía client thay password. |
| Phiên | Bind user_id vào connection, chưa làm token resume; actor lấy từ phiên server. |
| Presence | Registry RAM phản ánh phiên sống; DB giữ last_seen; không tin trạng thái online cũ sau restart. |
| Mất mạng | EOF/lỗi socket; heartbeat 15 giây, timeout 45 giây từ GĐ 2; keepalive hỗ trợ. |
| Tiếng Việt | Validate UTF-8, giới hạn byte; MySQL utf8mb4, GTK3 UTF-8. |

## 2. Ma trận yêu cầu và điểm

Chỉ đánh dấu đạt khi code và ca nghiệm thu tương ứng đã chạy; có tên trong kế hoạch chưa đồng nghĩa đã đạt điểm.

| # | Tiêu chí | Điểm | Giai đoạn | Điều kiện nghiệm thu chính |
|---|---|---:|---|---|
| 1 | Xử lý truyền dòng | 1 | GĐ 1 | Chia/gộp header/payload, length sai, EOF; không mất ranh giới. |
| 2 | I/O socket server | 2 | GĐ 1 | poll nonblocking RX/TX, short write, client chậm, giới hạn tài nguyên. |
| 3 | Đăng ký, quản lý tài khoản | 2 | GĐ 2 | Validate, username duy nhất, SHA-256+salt, lưu/đọc thông tin; chưa yêu cầu admin. |
| 4 | Đăng nhập, quản lý phiên | 2 | GĐ 2 | Login/logout/kick; phân quyền; cleanup không làm hỏng phiên mới. |
| 5 | Gửi lời mời kết bạn | 1 | GĐ 3 | Realtime/offline; tự mời, trùng, lời mời chéo. |
| 6 | Chấp nhận/từ chối kết bạn | 1 | GĐ 3 | Chỉ người nhận xử lý; đồng bộ hai phía. |
| 7 | Hủy kết bạn | 1 | GĐ 3 | Cập nhật hai phía; vẫn cho DM theo chính sách. |
| 8 | Danh sách bạn bè/trạng thái | 1 | GĐ 3 | Snapshot + event; không thay thế danh sách toàn bộ online. |
| 9 | Gửi nhận tin 1-1 | 1 | GĐ 4 | Persist trước gửi, message id, đúng người nhận, retry không chèn trùng. |
| 10 | Ngắt kết nối | 1 | GĐ 4 | Báo peer kể cả chưa kết bạn; END/logout/crash/timeout. |
| 11 | Tạo nhóm | 1 | GĐ 5 | Tạo group + owner membership trong transaction. |
| 12 | Thêm người vào nhóm | 1 | GĐ 5 | Invitation → accept/reject; pending offline. |
| 13 | Xóa người khỏi nhóm | 1 | GĐ 5 | Chỉ owner; member bị kick không đọc/gửi tiếp. |
| 14 | Rời nhóm | 1 | GĐ 5 | Membership/owner thay đổi nguyên tử; nhóm rỗng. |
| 15 | Gửi nhận tin nhóm | 1 | GĐ 5 | Check membership; commit, fan-out, thứ tự/sync rõ ràng. |
| 16 | Tin offline | 1 | GĐ 4–5 | DM ACK, ACK/cursor nhóm, batch, dedup, restart. |
| 17 | Log hoạt động | 1 | GĐ 2+ | Actor/action/result/time, disconnect/lỗi; không ghi bí mật. |
| 18 | GUI | 3 | Từ GĐ 1, hoàn thiện GĐ 6 | Toàn bộ chức năng cơ bản qua GTK3; không blocking UI. |
| 19 | Bổ sung | 2–5 | GĐ 7 | Có đặc tả, UI, test, demo; không tự quy số tính năng thành điểm. |

Ngoài rubric cần nghiệm thu danh sách **toàn bộ online**, END/OPEN khi chuyển DM, thông báo peer không phải bạn bè và invitation tham gia nhóm. Tổng cơ bản 23, tổng có bổ sung 25–28 theo danh sách cung cấp.

## 3. Kiến trúc và công nghệ

```text
GTK3 UI ↔ client network loop ↔ POSIX TCP + framed JSON
                                      ↕
Server poll loop: RX/TX / timer / session-generation / presence / DM context
       ↓ bounded job queue (từ GĐ 2)       ↑ completion queue + wakeup fd
       Business + DB worker (một thread, một MYSQL*) ↔ MySQL/InnoDB
```

- Event loop sở hữu fd, RX/TX, registry và timer; không làm query/hash chậm trực tiếp trong loop. Worker xử lý nghiệp vụ tuần tự từ GĐ 2; completion đánh thức poll qua pipe/eventfd; mọi queue có giới hạn.
- Job/response mang connection id + generation, tránh gửi nhầm fd tái sử dụng và cleanup phiên cũ làm phiên mới offline. Mutation đã nhận hợp lệ có thể commit sau disconnect; retry có khóa chống lặp; lệnh sau revoke bị từ chối.
- Client GTK3 ưu tiên POSIX fd nonblocking + `g_unix_fd_add()` trên main loop; callback xử lý lượng dữ liệu giới hạn. Nếu dùng thread mạng, thread sở hữu socket, UI gửi queue và nhận qua `g_idle_add()`; không làm hai mô hình cùng lúc.
- poll phù hợp 5–128 client; chỉ tăng worker/đổi epoll sau khi đo được bottleneck.
- C11/GCC/Make; cJSON từ `libcjson-dev`, link bằng `pkg-config libcjson`; không cần vendor source. MySQL prepared statements/utf8mb4; SHA-256+salt qua OpenSSL EVP; GUI GTK3.

Trong Ubuntu WSL:

```bash
sudo apt update
sudo apt install -y build-essential pkg-config libcjson-dev libgtk-3-dev
# Khi làm GĐ 2:
# sudo apt install -y libmysqlclient-dev mysql-server libssl-dev
```

WSLg có DISPLAY chưa đủ chứng minh GUI chạy; phải smoke test tạo cửa sổ GTK thật. Ncurses chỉ là công cụ debug, không bảo đảm điểm GUI. MinGW Windows không build phần POSIX hiện tại.

## 4. Giao thức và I/O

### 4.1. Framing, RX/TX và giới hạn

- Header unsigned 4 byte big endian; length tính theo byte payload, không gồm header/NUL. Payload 1–65.536 byte JSON UTF-8.
- Length 0/quá max: đóng connection. RX incremental tích lũy header/payload, parse nhiều frame trong một recv, giữ phần dở. Cấp thêm byte NUL khi cần API chuỗi.
- TX queue giữ wire frame + offset, gửi tiếp khi POLLOUT; bỏ POLLOUT khi queue rỗng. Không gọi blocking send_all trên event loop/UI.
- Retry EINTR; EAGAIN/EWOULDBLOCK giữ state và quay lại poll; recv==0 là EOF. Half-close: xử lý frame đủ còn lại, flush response rồi đóng; loại frame dở.
- Xử lý bit POLLIN/HUP/ERR/NVAL kết hợp, cleanup một lần; chặn SIGPIPE khi gửi vào peer đã đóng.
- Giới hạn đầu: 128 client, TX 512 KiB và 256 frame/connection, đọc/ghi tối đa 64 KiB/vòng, accept tối đa 16/vòng. Client vượt queue bị ngắt có log; client khác tiếp tục.
- GĐ 1: frame hoàn thành trong 10 giây từ byte đầu; TX không tiến triển 10 giây thì cleanup. Đồng hồ monotonic, CLI có cấu hình timeout để test.
- Chat content tối đa 16 KiB UTF-8; JSON serialize vẫn <= frame max. History/list/offline phân trang theo cursor **và** ngân sách byte.
- Validate UTF-8, root object, kiểu/trường, parse hết payload; từ chối NUL/chuỗi `\u0000` và trailing data. Kiểm tra NULL, độ dài byte trước copy/string API.

### 4.2. Envelope, lệnh và state

```json
{"version":1,"type":"DM_SEND","request_id":"req-1","data":{"to_user_id":"42","client_message_id":"msg-uuid","content":"Xin chào"}}
```

- Version 1; type ASCII tối đa 48 byte; request_id ASCII 1–64 byte; data object. Response gắn request_id, lỗi có code ổn định; push event không cần request_id.
- ID DB BIGINT truyền chuỗi thập phân; timestamp server tạo theo UTC. Mutation retry qua reconnect dùng khóa chống lặp riêng, không chỉ request_id.
- Actor lấy từ connection; check quyền mọi lệnh, kể cả history/info và ACK. Chỉ receiver/invitee được ACK/accept/reject; owner được invite/kick; member được đọc/gửi nhóm.
- Auth FSM: CONNECTED_UNAUTH → AUTH_PENDING → AUTHENTICATED → CLOSING; lỗi về unauth, kết quả cũ kiểm tra generation. Chưa có token resume.
- DM: NONE → OPEN(peer) → NONE; END báo peer với reason user_request/switched/disconnected; offline độc lập context.
- Invitation: pending → accepted/rejected/cancelled; accept nhóm tạo membership trong transaction; xử lý lặp nhất quán.
- Message: validate → INSERT/COMMIT → báo sender **đã lưu** → enqueue receiver → ACK hợp lệ → **delivered**. Read là trạng thái riêng. Mất ACK có thể giao lại; client dedup bằng message id. Không hứa exactly-once.
- Client không lưu local phải tải history sau restart. Snapshot/presence events được xếp thứ tự qua event loop.

| Nhóm | Lệnh/sự kiện |
|---|---|
| GĐ 1 | ECHO (trả nguyên payload đã validate), SYS_PING/SYS_PONG, SYS_ERROR. Dùng dựng nền tảng, chưa có auth/chat nghiệp vụ. |
| Auth/account | AUTH_REGISTER/LOGIN/LOGOUT, response, AUTH_KICKED, account info; password là đầu vào để server hash. |
| Presence | USER_ONLINE_LIST/RESULT, USER_PRESENCE_CHANGED, USER_SEARCH/RESULT. |
| Friends | FRIEND_REQUEST/ACCEPT/REJECT/REMOVE/LIST/PENDING_LIST, response, notify/status event. |
| DM | DM_OPEN/END/ENDED, SEND/RECEIVE/SEND_OK, DELIVERY_ACK, HISTORY/RESULT, offline sync. |
| Nhóm | GROUP_CREATE/LIST/INFO, INVITE/INVITE_ACCEPT/INVITE_REJECT/INVITE_PENDING_LIST, REMOVE_MEMBER/LEAVE, SEND/RECEIVE/HISTORY, sync/ACK. |
| System | GĐ 1 trả lời ping client; heartbeat 15/45 giây bắt đầu GĐ 2; lỗi và mã bận. |

`docs/protocol.md` mô tả hợp đồng thực tế theo giai đoạn: field, điều kiện, response/error và event gửi cho ai; không coi type tương lai là đã implement.

## 5. Dữ liệu MySQL (GĐ 2 trở đi)

| Bảng | Thiết kế/ràng buộc |
|---|---|
| users | id, username UNIQUE (ASCII 1–50 byte), password_salt CHAR(32), password_hash CHAR(64), display_name UTF-8, created_at, last_seen. |
| friendships | Một record/cặp user_low < user_high, UNIQUE cặp; requested_by thuộc cặp; status pending/accepted/rejected; cấm tự kết bạn, xử lý mời chéo/reinvite. |
| direct_messages | BIGINT id, sender/receiver FK, client_message_id, content, delivered_at/read_at nullable, created_at; UNIQUE(sender_id,client_message_id). |
| chat_groups | id, name, owner_id FK, created_at; owner_id là nguồn quyền chính, không có role owner thứ hai mâu thuẫn. |
| group_invitations | id, group/inviter/invitee FK, status, timestamps; tối đa một pending/group/invitee; accept kiểm tra lại group/member/quyền. |
| group_members | id, group_id/user_id FK, UNIQUE(group_id,user_id), joined_at, joined_after_message_id, last_acked_message_id. |
| group_messages | BIGINT id, group/sender FK, client_message_id, content, created_at; UNIQUE(sender_id,client_message_id). |
| activity_logs | id, nullable user_id, action/result/details/ip/time, request/connection id; không ghi password/token. |

- InnoDB, FK, prepared statements, utf8mb4 và charset connection. Schema SQL thực hiện ở GĐ 2.
- Transaction cho group+owner membership, accept+membership, kick/leave/chuyển owner và thay đổi nhiều bước.
- Join dùng mốc id tin cuối nhóm trong thứ tự job/transaction serialize; không dùng timestamp đến giây làm ranh giới quyền đọc. Rejoin tạo mốc mới.
- DM query hai chiều + cursor id; index theo query thực tế. Offline index receiver/delivery/id; group index (group_id,id); membership lookup (user_id,group_id).
- ACK cursor nhóm chỉ tiến sau mọi tin hợp lệ trước mốc đã nhận, không lấy MAX ACK rời rạc. Bị kick/leave thì mất quyền đọc/gửi.
- DB lỗi không trả send thành công. Log lỗi DB ra file khi chính DB không ghi log được.

## 6. Cấu trúc và triển khai

```text
common/include/{frame.h,protocol.h,net.h}
common/src/{frame.c,protocol.c,net.c}
server/include/server.h
server/src/{main.c,server.c}
client/src/{main.c,gui_smoke.c}
tests/{test_frame.c,test_protocol.c,integration.py}
docs/{protocol.md,phase1-results.md,plan-review.md}
Makefile, README.md, .gitignore
```

frame.c độc lập socket, dùng chung decoder/TX để test short write có kiểm soát. net.c chứa helper POSIX. Module auth/friend/chat/group/db/logger, schema.sql và GUI đầy đủ tạo ở giai đoạn tương ứng.

Quy ước comment theo code TCP mẫu trong `D:\2026.1\TH-LTM\HW\Code tham khảo`: mỗi hàm C có khối tên hàm, `Y nghia`, `Dau vao`, `Dau ra`; nêu rõ ownership/mã lỗi và giải thích các bước socket quan trọng. Quy tắc chi tiết lưu ở `AGENTS.md`.

Người A phụ trách server/DB; người B common framing, client/GUI và công cụ test. Cả hai chốt protocol/schema và review phần giao tiếp chung, tích hợp liên tục.

| Tuần | Kết quả |
|---|---|
| 1–2 — GĐ 1 | Makefile, RX/TX framing, poll nonblocking, terminal client, TCP tests, GTK3/WSLg smoke, protocol và báo cáo. |
| 3 — GĐ 2 | DB worker, auth SHA-256+salt, session/generation, toàn bộ online, heartbeat, log, GUI login tối thiểu. |
| 4 — GĐ 4 | DM persist/ACK/retry/offline/history, END/OPEN, peer chưa kết bạn, GUI chat tối thiểu. |
| 5 — GĐ 3 | Kết bạn, pending offline/mời chéo/status, GUI quản lý. |
| 6 — GĐ 5 | Nhóm/invitation/quyền/owner/history/sync offline, tích hợp GUI. |
| 7 — GĐ 6–7 | Hoàn thiện GUI, tích hợp và bonus đã chốt; đóng phạm vi cuối tuần. |
| 8 — GĐ 8 | Regression, sanitizer/memory, build sạch, README, slide và diễn tập demo. |

### Giai đoạn 1 — checklist

- [x] Build server/client C11 riêng; README lệnh WSL.
- [x] Incremental RX, frame max 64 KiB, TX queue/offset/cap.
- [x] cJSON/envelope/UTF-8, ECHO/PING/error.
- [x] Server poll nonblocking, quota/connection cap, POLLOUT, EOF/HUP/error/timeout, shutdown sạch.
- [x] Client terminal dùng frame/protocol chung; gửi tiếng Việt, nhận echo/pong.
- [x] Test chia/gộp/header từng byte, max/sai length, short-write, EOF giữa frame, half-close, slow reader, nhiều client, timeout.
- [x] Cảnh báo compiler và ASan/UBSan không có lỗi chưa giải thích.
- [x] GTK3 tạo native window qua WSLg; smoke --check không map cửa sổ.
- [x] protocol.md, phase1-results.md, nội dung slide tiến độ khớp kết quả thật.

**Tiến độ ngày 26/10/2026:** GĐ 1 hoàn thành; 3 bộ C và 15/15 TCP integration PASS ở bản thường lẫn ASan/UBSan; kiểm tra 16 client đồng thời, GTK3 smoke PASS. Chi tiết ở `docs/phase1-results.md`. Chưa triển khai auth/MySQL/chat nghiệp vụ của các giai đoạn sau.

Bonus ưu tiên đổi mật khẩu, chỉnh/xem profile và tìm lịch sử theo quyền. Mục tiêu ba bonus nếu đủ thời gian; mỗi cái có protocol/UI/test/demo. Tìm user phục vụ kết bạn không mặc định được tính lại bonus. File/base64, sửa/xóa/thu hồi tin chưa thuộc mặc định vì cần đặc tả chunk/quyền/delivery. Không hứa số điểm cho tổ hợp tính năng.

## 7. Kiểm thử, rủi ro và hoàn thành

| Nhóm | Ca bắt buộc |
|---|---|
| TCP/I/O | Header/payload chia gộp, max/0/quá lớn, EOF/RST, slow reader/short-write/EAGAIN, frame/TX timeout, nhiều client. |
| Protocol | JSON sai/thiếu field/trailing data, UTF-8/NUL lỗi, type/version/id sai; error đúng request. |
| Auth/session | Trùng username, sai password, pre-login command, login thay phiên; cleanup/completion cũ không hỏng phiên mới. |
| Presence/DM | Online chưa kết bạn; END/chuyển chat; logout/crash/blackhole; persist/ACK/retry sau commit; backlog nhiều batch. |
| Friends/group | Mời chéo/sai người/unfriend/reinvite; nhóm pending offline/kick/leave/owner; rollback giữa bước; join cùng giây/rejoin. |
| GUI/storage | Tiếng Việt/emoji, DB chậm, logout khi event chờ, restart, log không bí mật, không crash/leak. |

kill -9 chỉ thử process chết; blackhole cần ca riêng để xác minh heartbeat. Test/sanitizer chạy theo từng giai đoạn. Ưu tiên chức năng bắt buộc trước bonus; không dồn test vào nửa tuần cuối.

Definition of Done: từng hàng rubric và yêu cầu bổ sung có ca demo qua GUI; ít nhất 5 client; client lỗi/chậm không treo server; offline/quyền/phiên có test lỗi; build sạch trên Ubuntu; tài liệu/schema/demo tái lập được. Mỗi mốc có slide tiến độ và bằng chứng test, cuối tuần 8 có demo tổng/phân công/giới hạn thực tế.

docs/plan-review.md là review lịch sử của bản 1.0. Đề xuất thay thuật toán password không áp dụng vì người dùng đã chọn giữ SHA-256. Checklist GĐ 1 chỉ đánh dấu hoàn thành theo kết quả chạy thực tế.
