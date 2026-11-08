# Task 1 · Phòng chờ qua TCP

## Phạm vi đã triển khai

- Client giữ kết nối sau khi đăng nhập và hỗ trợ `list`, `create <tên_phòng> <sức_chứa>` và `join <room_id>`. Client dùng `poll()` để nhận cập nhật ngay cả khi đang chờ nhập lệnh.
- Server quản lý phòng trong bộ nhớ. Người tạo phòng là Chủ phòng và là thành viên đầu tiên. Khi có người tham gia, server gửi cùng một danh sách thành viên cho cả phòng. Khi kết nối đóng, server gỡ thành viên, chuyển quyền cho người vào sớm nhất còn kết nối nếu Chủ phòng rời, và xóa phòng khi rỗng.
- Server dùng TCP nonblocking với `poll()`, giữ dữ liệu đọc dở và hàng đợi gửi có giới hạn cho từng kết nối. Địa chỉ mặc định là `127.0.0.1:5555`, tối đa 64 kết nối đồng thời.

## Đặc tả giao thức

Header 8 byte, theo thứ tự `msg_type:u8` (offset 0), `length:u16` (1), `request_id:u32` (3), `status_code:u8` (7). `length` chỉ đếm body, tối đa 4096 byte như mã hiện tại. `u8/u16/u32` lần lượt là 1/2/4 byte. Mọi số nhiều byte dùng big-endian. Mã hóa từng trường, không gửi nguyên struct C. Request có status 0. `max_players` từ 5 đến 12, tính cả Host. Phòng chờ có thể có ít hơn 5 thành viên.

`Name` gồm `len:u8` và đúng `len` byte tên, với `1 ≤ len ≤ 32`, giữ nguyên byte và không chứa NUL. `RoomInfo` và `PlayerEntry` dùng bố cục dưới đây.

Mọi yêu cầu từ client, kể cả `LOGIN`, phải có `request_id` khác `0` và không trùng ID của yêu cầu đang chờ trên cùng socket. Phản hồi giữ nguyên `request_id` của yêu cầu gốc. `ROOM_UPDATE` là thông báo đẩy có `request_id = 0` và `status_code = 0`.

`LOGIN` dùng tên thô làm body của request và body rỗng trong phản hồi. Client gửi `request_id = 1` cho `LOGIN`. Sau khi đăng nhập thành công, server cấp ID phiên. Client tìm ID của mình trong `ROOM_UPDATE` bằng username duy nhất, so sánh theo byte như `LOGIN`.

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

Lệnh thất bại không đổi trạng thái phòng và không phát thông báo đẩy. Với yêu cầu có ID `0`, server trả `MSG_RESPONSE (0xFF)` với ID `0` và status `0x08` mà không thực hiện lệnh. Client phân biệt phản hồi lỗi với `ROOM_UPDATE` bằng `msg_type`. Server đóng riêng kết nối nếu `length > 4096` hoặc khung dở quá 10 giây. Sau 5 frame lỗi liên tiếp, gửi phản hồi lỗi cuối rồi đóng. Các mã lỗi nghiệp vụ không tính là frame lỗi.

Các gói mẫu dùng để kiểm tra codec và ghép phản hồi:

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

## Chạy và kiểm tra

Từ gốc repository trên Linux, chạy `make`. Mở ba terminal cho server, Alice và Bob, rồi chạy lần lượt:

```sh
./build/server
./build/client 127.0.0.1 alice
./build/client 127.0.0.1 bob
```

1. Alice nhập `create LangSoi 5` rồi để nguyên terminal. Alice thấy mình là thành viên duy nhất và `host_id` trùng với ID của mình.
2. Bob nhập `list` để lấy ID phòng. Thử `join 9999` nếu ID đó chưa tồn tại. Bob nhận `ERR_ROOM_NOT_FOUND (0x09)` và vẫn kết nối.
3. Bob nhập `join <room_id>` với ID vừa lấy. Cả hai terminal tự hiển thị cùng `room_id`, `host_id` của Alice và hai thành viên Alice, Bob.

Để chạy kiểm thử tự động, dừng server demo rồi chạy `python3 tests/lobby.py` từ gốc repository. Script tự khởi động server và các client.

## Tiến độ bàn giao

| Hạng mục | Trạng thái |
| --- | --- |
| Build và kiểm thử Task 1 | Đã qua trên Ubuntu WSL. GCC với `-std=c11 -Wall -Wextra` không phát cảnh báo. Kiểm thử xác nhận luồng Alice–Bob, JOIN sai, ghép phản hồi theo ID, frame TCP bị chia hoặc gửi liền, cô lập lỗi, timeout, chuyển Chủ phòng và dọn descriptor. |
| SERVER-01, CLIENT-01, CLIENT-02, COMMON-02 | Đã nghiệm thu và đánh dấu trong `PLAN.md`. |
| COMMON-01 | Codec dùng chung và gói mẫu đã có để bàn giao. |
| SERVER-02 | Đã triển khai và kiểm thử luồng phòng chờ. Kiểm tra sức chứa đã có trong code, ca phòng đầy được hoãn. |
| SERVER-03, CLIENT-03, CLIENT-06 | Đã có phần liên quan Task 1. Các lệnh mời, loại, rời phòng chủ động, bắt đầu ván và các kịch bản tích hợp ngoài phòng chờ thuộc các mốc tiếp theo. |
