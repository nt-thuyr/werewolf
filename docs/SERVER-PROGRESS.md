# Tiến độ Người 1 · Server mạng, phiên và phòng chờ

Thiết kế tham chiếu ở [ver2](https://docs.google.com/document/d/1o8jz59_vBk5IG-NB3HvIKDmAgUrok2Aztk2Ux5sSsJY/edit?tab=t.s0u7ebxdy021), mục 4–5 về giao thức và mục 8 về xử lý lỗi. Phân công và tiêu chí từng task nằm trong [PLAN.md](PLAN.md).

## Tiến độ

| Task | Kết quả hiện tại | Phần còn lại |
| --- | --- | --- |
| SERVER-01 · TCP và đăng nhập | Đã nghiệm thu dồn kênh, đóng/mở khung, định danh theo phiên, cô lập lỗi và dọn kết nối. Server ghi log khi độ dài vượt giới hạn hoặc khung dở hết hạn. | Hoàn tất. |
| COMMON-01 · Codec dùng chung | API và mã thông điệp hiện tại nằm trong `include/protocol.h`. Hai chương trình đã được kiểm tra với các gói LOGIN và LIST mẫu. | Bổ sung hằng số tương ứng khi tích hợp thông điệp mới. |
| SERVER-02 · Xem, tạo và vào phòng | Quản lý ID và sức chứa. Tạo/vào phòng trả phản hồi rồi đẩy danh sách thành viên. Có test JOIN sai ID và JOIN khi phòng đầy. | Hoàn tất các luồng hiện tại. |
| SERVER-03 · Quản lý thành viên và Chủ phòng | Khi mất kết nối, server gỡ thành viên, chuyển Chủ phòng cho người vào sớm nhất còn kết nối và xóa phòng rỗng. | Mời, loại và rời phòng chủ động. |
| SERVER-04 · Heartbeat và thời hạn | Có đồng hồ monotonic dùng cho timeout khung dở của SERVER-01. | PING/PONG, timeout mất kết nối và lịch chốt pha. |
| SERVER-05 · Nối phòng chờ với game | | Ghép lệnh bắt đầu, luồng sự kiện ván và đường xử lý người rời với mô-đun game. |

## Luồng xử lý trên server

- `src/server/main.c` mở socket lắng nghe IPv4. Mặc định là `127.0.0.1:5555`, giới hạn 64 kết nối đồng thời.
- `src/server/session.c` dồn kênh với các socket nonblocking. `ppoll()` cho phép mở chặn SIGINT/SIGTERM ngay khi bắt đầu chờ. Hai tín hiệu này yêu cầu vòng lặp đóng kết nối rồi thoát. Mỗi phiên giữ số byte đã đọc, vị trí gửi và hạn khung dở riêng. Server chỉ xử lý yêu cầu sau khi đọc đủ header và body, rồi tiếp tục từ vị trí đã gửi nếu `send()` trả về số byte ngắn hơn yêu cầu.
- Khi `accept()` báo cạn tài nguyên, server đợi 250 ms trước lần nhận mới và tiếp tục phục vụ các phiên hiện có. Log khung lỗi ghi theo khả năng của đích nhận. Với pipe hoặc socket đang đầy, log có thể bị bỏ để vòng lặp vẫn xử lý được kết nối.
- `src/server/lobby.c` xử lý nghiệp vụ phòng và tạo thông báo `ROOM_UPDATE`. Vòng lặp I/O đánh dấu phiên lỗi rồi dọn socket và cập nhật phòng sau khi duyệt các descriptor sẵn sàng.

Cơ sở kỹ thuật trong slide gồm Lec03, slide 13–15 về byte order, Lec04 TCP, slide 14–18 và 28–37 về I/O và framing, Lec06, slide 22–25 về `poll()`, và Lec07, slide 5–9 về nonblocking I/O.

## Đặc tả giao thức

Header 8 byte, theo thứ tự `msg_type:u8` (offset 0), `length:u16` (1), `request_id:u32` (3), `status_code:u8` (7). `length` chỉ đếm body, tối đa 4096 byte như mã hiện tại. `u8/u16/u32` lần lượt là 1/2/4 byte. Mọi số nhiều byte dùng big-endian. Mã hóa từng trường, không gửi nguyên struct C. Request có status 0. `max_players` từ 5 đến 12, tính cả Host. Phòng chờ có thể có ít hơn 5 thành viên.

`Name` gồm `len:u8` và đúng `len` byte tên, với `1 ≤ len ≤ 32`, giữ nguyên byte và không chứa NUL. `RoomInfo` và `PlayerEntry` dùng bố cục dưới đây.

Mọi yêu cầu từ client, kể cả `LOGIN`, phải có `request_id` khác `0` và không trùng ID của yêu cầu đang chờ trên cùng socket. Phản hồi giữ nguyên `request_id` của yêu cầu gốc. `ROOM_UPDATE` là thông báo đẩy có `request_id = 0` và `status_code = 0`.

`MSG_LOGIN_REQ (0x01)` gửi tên thô dài 1–32 byte. Phản hồi `MSG_RESPONSE` có body rỗng. Client hiện dùng `request_id = 1` cho LOGIN. Sau khi đăng nhập thành công, server cấp ID phiên. Client tìm ID của mình trong `ROOM_UPDATE` bằng tên đăng nhập, so sánh theo byte và phân biệt hoa thường.

| Nhóm | Chiều và mã | Body theo thứ tự | Phản hồi và hiệu ứng |
| --- | --- | --- | --- |
| LIST | C → S `MSG_ROOM_LIST_REQ (0x02)`, S → C `MSG_ROOM_LIST_RESP (0x03)` | Request rỗng. Thành công: `count:u16` rồi `count` bản ghi `RoomInfo`. Mỗi bản ghi: `room_id:u32`, `room_name:Name`, `player_count:u8`, `max_players:u8` (7 + độ dài tên byte). | Chỉ gửi cho người yêu cầu. Danh sách rỗng có `count=0` và body 2 byte. Khi lỗi, dùng `MSG_RESPONSE`. |
| CREATE | C → S `MSG_CREATE_ROOM_REQ (0x04)` | `room_name:Name`, `max_players:u8` (2 + độ dài tên byte). | Tạo phòng chờ, thêm người tạo làm Host. Gửi `MSG_RESPONSE` thành công rồi `ROOM_UPDATE` trên socket của Host. |
| JOIN | C → S `MSG_JOIN_ROOM_REQ (0x05)` | `room_id:u32` (4 byte). | Vào phòng theo ID. Khi thành công, xếp `MSG_RESPONSE` trước `ROOM_UPDATE` trên socket người vào. Gửi cùng snapshot cho mọi thành viên còn kết nối, kể cả người vừa vào. |
| ROOM_UPDATE | S → thành viên phòng `MSG_ROOM_UPDATE (0x07)` | `room_id:u32`, `host_id:u32`, `player_count:u8`, rồi `player_count` bản ghi `PlayerEntry`. Mỗi bản ghi: `player_id:u32`, `username:Name` (5 + độ dài tên byte). | Snapshot đầy đủ theo thứ tự vào phòng. `host_id` phải trùng ID của một thành viên trong snapshot. Client thay toàn bộ trạng thái phòng bằng snapshot hợp lệ. Không có ACK từ client. |

`MSG_RESPONSE (0xFF)` có body rỗng. Thành công dùng `0x00`. Với frame trong giới hạn, kiểm tra cấu trúc trước, rồi đến đăng nhập, trạng thái sảnh và điều kiện của phòng:

| Status | Khi trả |
| --- | --- |
| `0x08 ERR_INVALID_PAYLOAD` | Sai loại/chiều thông điệp, status request khác 0, `request_id` bằng 0, body thiếu/thừa byte, tên sai định dạng hoặc sức chứa ngoài 5–12. |
| `0x01 ERR_UNAUTHENTICATED` | Gửi LIST/CREATE/JOIN hợp lệ trước LOGIN. |
| `0x02 ERR_UNAUTHORIZED` | Gửi LIST/CREATE/JOIN hợp lệ khi phiên đã ở trong phòng. |
| `0x09 ERR_ROOM_NOT_FOUND` | JOIN từ sảnh với ID không tồn tại. |
| `0x05 ERR_ROOM_FULL` | JOIN từ sảnh vào phòng đã đủ chỗ. |
| `0x06 ERR_USERNAME_EXIST` | Tên LOGIN trùng tên của một phiên còn kết nối. Tên dùng lại được sau khi phiên cũ được dọn. |

Lệnh thất bại không đổi trạng thái phòng và không phát thông báo đẩy. Với yêu cầu có ID `0`, server trả `MSG_RESPONSE (0xFF)` với ID `0` và status `0x08` mà không thực hiện lệnh. Client phân biệt phản hồi lỗi với `ROOM_UPDATE` bằng `msg_type`. Server đóng riêng kết nối nếu `length > 4096` hoặc khung dở quá 10 giây. Sau 5 frame lỗi liên tiếp, gửi phản hồi lỗi cuối rồi đóng. Các mã lỗi nghiệp vụ không tính là frame lỗi.

### API dùng chung cho client và server

Hai chương trình cùng include [protocol.h](../include/protocol.h) và dịch với [protocol.c](../src/common/protocol.c). Dùng `PROTOCOL_MAX_FRAME` cho bộ đệm chứa một khung tối đa, gồm 8 byte header và 4096 byte body.

- `protocol_encode_header()` và `protocol_decode_header()` đọc hoặc ghi đúng `PROTOCOL_HEADER_SIZE` byte. Bên nhận gom đủ header rồi mới giải mã và kiểm tra độ dài, loại thông điệp cùng các trường liên quan trước khi xử lý body.
- `protocol_put_u16()`, `protocol_put_u32()` và hai hàm đọc tương ứng chuyển đổi số nguyên theo thứ tự byte mạng. Bên gọi bảo đảm bộ đệm còn đủ 2 hoặc 4 byte.
- `protocol_put_name()` ghi trường `Name` đã được kiểm tra độ dài và trả số byte đã ghi. `protocol_get_name()` kiểm tra trường theo độ dài body, thêm NUL vào bộ đệm đích và cập nhật offset khi thành công. LOGIN gửi tên thô theo độ dài header.

### Gói mẫu trên đường truyền

| Frame | Hex |
| --- | --- |
| LOGIN của `alice`, ID yêu cầu 1 | `01 00 05 00 00 00 01 00 61 6C 69 63 65` |
| Phản hồi LOGIN thành công, cùng ID | `FF 00 00 00 00 00 01 00` |
| LIST yêu cầu, ID 2 | `02 00 00 00 00 00 02 00` |
| LIST thành công với danh sách rỗng, cùng ID | `03 00 02 00 00 00 02 00 00 00` |

Phòng 7 có Host 101 (`alice`) và thành viên 102 (`bob`). `ROOM_UPDATE` có 27 byte body và 35 byte cả frame:

```text
07 00 1B 00 00 00 00 00
00 00 00 07  00 00 00 65  02
00 00 00 65  05 61 6C 69 63 65
00 00 00 66  03 62 6F 62
```

`test_protocol_samples()` trong [tests/lobby.py](../tests/lobby.py) chuyển tiếp gói thật giữa client và server qua loopback. Ca kiểm thử so từng byte của LOGIN và LIST trên cùng kết nối để phát hiện byte dư sau LOGIN ở cả hai chiều.

## Nghiệm thu SERVER-01

Bản hiện tại đã build và qua bộ kiểm thử tích hợp trên Ubuntu 22.04 WSL với GCC 11.4.

Từ gốc repository trên Linux, dừng server demo nếu đang chạy rồi thực hiện:

```sh
make -B CFLAGS='-std=c11 -Wall -Wextra -Werror'
python3 -B tests/lobby.py
```

Bộ test tự mở server và hai client thật để kiểm tra đăng nhập đồng thời, luồng phòng chờ, cập nhật Chủ phòng khi EOF và dùng lại tên. Các socket thử giao thức kiểm tra khung gửi từng byte, 100 khung gửi liền, ghép phản hồi theo ID, tên trùng, header và body sai, ngưỡng năm khung lỗi, timeout header/body dở và reset kết nối. Sau khi các client thoát, số descriptor của server phải trở về mức ban đầu.

Các ca hồi quy kiểm tra pipe log đầy, cạn và khôi phục descriptor, dừng server bằng SIGINT/SIGTERM, từ chối JOIN phòng đầy và hiện tên chứa ký tự điều khiển dưới dạng escape. Tên trên giao thức vẫn được truyền theo byte gốc.

Nguồn kiểm thử nằm trong [tests/lobby.py](../tests/lobby.py). Cách demo bằng tay có trong [README](../README.md).
