# Chat LTM
BTL chat C11/POSIX TCP trên Ubuntu/WSL2. Xem [kế hoạch](plan.md), [quy ước comment theo code mẫu](AGENTS.md) và [giao thức](docs/protocol.md).

Giai đoạn 1 có server `poll()` nonblocking, client terminal, frame TCP hai chiều, echo/ping JSON UTF-8, bộ kiểm thử mạng và GTK3 smoke. Đăng nhập, MySQL, bạn bè, DM/nhóm và GUI chat đầy đủ thuộc các giai đoạn tiếp theo.

## Chuẩn bị và build

Từ PowerShell mở Ubuntu:

```powershell
wsl -d Ubuntu-22.04
```

Sau đó chạy **trong Ubuntu**:

```bash
cd /mnt/d/2026.1/TH-LTM/Project-LTM
sudo apt update
sudo apt install -y build-essential pkg-config libcjson-dev libgtk-3-dev python3
make -j2
```

Không dùng GCC/MinGW Windows để build phần POSIX hiện tại. Có thể dùng Ubuntu native/VM tương thích thay WSL. Giai đoạn 1 chưa cần MySQL/OpenSSL; SHA-256 được giữ trong kế hoạch cho giai đoạn tài khoản.

## Mở VS Code và IntelliSense trong WSL

C/C++ IntelliSense phải chạy trong Ubuntu để dùng cùng compiler và header với build. Chỉ mở terminal WSL bên trong một cửa sổ VS Code chạy local Windows chưa chuyển extension sang Linux. [Hướng dẫn VS Code WSL](https://code.visualstudio.com/docs/remote/wsl).

1. Trong VS Code, nhấn **Ctrl+Shift+P** → **WSL: Reopen Folder in WSL**, chọn **Ubuntu-22.04** nếu được hỏi. Góc dưới trái cần hiện **WSL: Ubuntu-22.04**.
2. Mở Extensions, cài **C/C++** của Microsoft vào **WSL: Ubuntu-22.04** nếu chưa có; bản extension Windows và bản WSL là hai môi trường khác nhau.
3. Project có sẵn `.vscode/c_cpp_properties.json`: GCC `/usr/bin/gcc`, C11, POSIX define và include paths cJSON/GTK3. Nếu extension còn giữ lỗi cũ, chạy **C/C++: Reset IntelliSense Database**, rồi **Developer: Reload Window**.

Cũng có thể mở đúng workspace từ PowerShell:

```powershell
code --remote wsl+Ubuntu-22.04 /mnt/d/2026.1/TH-LTM/Project-LTM
```

Header cJSON đã cài trong Ubuntu ở `/usr/include/cjson/cJSON.h`; Makefile lấy `-I/usr/include/cjson` qua `pkg-config`. Nếu có lỗi `cannot open source file cJSON.h` ở IntelliSense nhưng `make` thành công, kiểm tra chế độ WSL, C/C++ extension phía WSL và cấu hình Linux trước khi cài lại thư viện. Các include paths trong cấu hình editor tương ứng môi trường Ubuntu 22.04 x86_64 hiện tại.

## Chạy server/client

Terminal Ubuntu thứ nhất:

```bash
./bin/ltm-server
```

Server mặc định `127.0.0.1:9090`


```bash
cd /mnt/d/2026.1/TH-LTM/Project-LTM
./bin/ltm-client
```

Nhập nội dung để nhận JSON echo; `/ping` nhận `SYS_PONG`, `/quit` hoặc Ctrl+D để thoát. Mở nhiều terminal để thử nhiều client. Gửi một lần:

```bash
./bin/ltm-client -m 'Xin chào Việt Nam 😀'
./bin/ltm-client -P
```

Đổi cổng và timeout:

```bash
./bin/ltm-server -b 127.0.0.1 -p 9091 -m 32 -t 10000 -w 10000
./bin/ltm-client -H 127.0.0.1 -p 9091 -m 'Thử cổng khác'
```

`-t`: deadline hoàn thành frame; `-w`: TX không tiến triển, đơn vị millisecond. `-p 0` để OS chọn cổng; server in `LISTENING <IP> <port>` cho công cụ test. Bind `0.0.0.0` khi chủ động muốn nhận từ interface khác; nhiều client trong cùng WSL dùng loopback là đủ.

## Kiểm thử

```bash
make test        # 3 bộ kiểm tra C + TCP integration qua executable thực
make sanitize    # Bản ASan/UBSan riêng ở bin/sanitize, chạy cùng bộ test
make gui-check   # GTK3 tạo native window qua display, không map cửa sổ
```

Integration tự mở server loopback trên cổng trống, kiểm tra 16 client đồng thời, chia/gộp, frame 64 KiB, UTF-8, protocol lỗi, half-close/EOF/RST, client chậm, timeout và giới hạn kết nối; thu hồi process/socket/file tạm khi xong. Không cần mở sẵn server cổng 9090.

Xem cửa sổ GTK3 kiểm tra môi trường:

```bash
make gui-smoke
./bin/ltm-gui-smoke
```

Đây là cửa sổ smoke, chưa có chức năng chat. `GTK display unavailable` nghĩa là cần sửa WSLg/display trước khi làm GUI; test TCP vẫn độc lập. GUI smoke không thuộc bản sanitizer của phần socket.

## Cấu trúc và tài liệu

```text
common/       Decoder/TX queue, validate protocol, helper POSIX
server/       Listener, poll loop, giới hạn, cleanup
client/       Terminal client và GTK3 smoke
tests/        Frame/protocol/partial I/O và TCP integration
docs/         Giao thức, kết quả kiểm thử, review
reports/       Nội dung báo cáo tiến độ
bin/, build/  Output build, được Git bỏ qua

