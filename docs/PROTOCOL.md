# Giao thức client–server

**Thiết kế tham chiếu:** [Google Doc, ver2](https://docs.google.com/document/d/1o8jz59_vBk5IG-NB3HvIKDmAgUrok2Aztk2Ux5sSsJY/edit?tab=t.s0u7ebxdy021).

## Quy ước chung

- Mỗi frame gồm header 8 byte và body tối đa 4096 byte.
- Header: `msg_type:u8`, `length:u16`, `request_id:u32`, `status_code:u8`. Số nhiều byte dùng big-endian; `length` chỉ tính body. Không gửi nguyên struct C.
- Request có `request_id != 0`, `status_code = 0`. Response giữ nguyên request ID. Server push dùng ID 0, status 0.
- `Name` là `len:u8` và 1–32 byte tên, không chứa NUL. Sức chứa phòng được cấu hình từ 5 đến 12 người, tính cả Chủ phòng.

## Phòng chờ

| Type | Hướng | Body | Kết quả chính |
| --- | --- | --- | --- |
| `0x01 LOGIN` | C→S | Tên thô, 1–32 byte | Response rỗng; tên phải chưa được dùng. |
| `0x02 ROOM_LIST_REQ` | C→S | Rỗng | `0x03 ROOM_LIST_RESP`: `count:u16`, rồi các `RoomInfo`. |
| `0x04 CREATE_ROOM_REQ` | C→S | `room_name:Name`, `max_players:u8` | Tạo phòng, người tạo làm host. |
| `0x05 JOIN_ROOM_REQ` | C→S | `room_id:u32` | Vào phòng đang chờ nếu còn chỗ. Phòng đang chơi trả `ERR_GAME_RUNNING` (`0x07`). |
| `0x06 LEAVE_ROOM_REQ` | C→S | Rỗng | Rời phòng, kết nối vẫn ở sảnh. |
| `0x07 ROOM_UPDATE` | S→phòng | `room_id:u32`, `host_id:u32`, `count:u8`, `PlayerEntry[]` | Snapshot đầy đủ thành viên. |
| `0x10 INVITE_REQ` | C→S | `target_user:Name` | Chủ phòng mời người đang ở sảnh khi phòng đang chờ. |
| `0x11 INVITE_NOTIF` | S→C | `room_id:u32`, `host_name:Name` | Người nhận có thể JOIN. |
| `0x12 KICK_REQ` | C→S | `target_user_id:u32` | Chỉ Chủ phòng được loại thành viên khác khi phòng đang chờ. |
| `0x13 KICK_NOTIF` | S→C | `reason:u8` (`0x01`: host loại) | Client về sảnh. |
| `0x14 START_GAME_REQ` | C→S | Rỗng | Chủ phòng bắt đầu ván khi phòng đang chờ và có ít nhất 5 thành viên còn kết nối, tính cả Chủ phòng. |

`RoomInfo = room_id:u32, room_name:Name, player_count:u8, max_players:u8`.
`PlayerEntry = player_id:u32, username:Name`.

Sau khi kiểm tra cấu trúc và đăng nhập, server kiểm tra quyền Chủ phòng trước khi xét trạng thái phòng. Người gửi `START`, `INVITE` hoặc `KICK` mà không có quyền Chủ phòng nhận `ERR_UNAUTHORIZED` (`0x02`). Chủ phòng gửi các lệnh này khi phòng đang chơi nhận `ERR_GAME_RUNNING` (`0x07`). START hợp lệ khi phòng đang chờ nhưng thiếu người trả `ERR_NOT_ENOUGH_PLAYERS` (`0x0A`).

Khi START thành công, server xếp `MSG_RESPONSE` với body rỗng và cùng `request_id` vào hàng gửi của Chủ phòng trước các sự kiện mở ván. Mỗi người, kể cả Chủ phòng, nhận một `ROLE_ASSIGN` riêng. Server gửi `WOLF_TEAM_INFO` cho các thành viên phe Sói, rồi gửi cùng một `PHASE_CHANGE` tới cả phòng để mở đêm đầu tiên.

## Trong ván

Request hành động nhận response rỗng nếu thành công. Server gửi event tới người chơi được phép nhận.

| Type | Hướng | Body |
| --- | --- | --- |
| `0x20 ROLE_ASSIGN` | S→C riêng | `role_id:u8`: 1 Sói, 2 Dân, 3 Tiên tri, 4 Bảo vệ |
| `0x22 WOLF_TEAM_INFO` | S→Sói | `count:u8`, `wolf_ids:u32[]` |
| `0x30 PHASE_CHANGE` | S→phòng | `phase:u8`, `duration_sec:u16`, `deadline_ms:u64` |
| `0x31 NIGHT_ACTION_REQ` | C→S | `target_id:u32` |
| `0x32 SEER_RESULT` | S→Tiên tri | `target_id:u32`, `is_werewolf:u8` |
| `0x33 WOLF_TARGET_UPDATE` | S→Sói sống | `wolf_id:u32`, `target_id:u32` |
| `0x34 NIGHT_RESULT` | S→phòng | `killed_id:u32`; 0 là không ai chết |
| `0x35 DAY_VOTE_REQ` | C→S | `target_id:u32`; 0 là phiếu trắng |
| `0x36 DAY_RESULT` | S→phòng | `executed_id:u32`; 0 là không ai bị treo |
| `0x37 CHAT_MSG` | C→S | `scope:u8`, `message:bytes` |
| `0x38 GAME_OVER` | S→phòng | `winner:u8`, `summary:bytes`; 0 hủy, 1 Dân, 2 Sói |
| `0x39 CHAT_NOTIF` | S→C | `scope:u8`, `from_id:u32`, `message:bytes` |

Pha: 1 Đêm, 2 Thảo luận, 3 Bỏ phiếu. Scope chat: 0 Làng ban ngày, 1 Sói ban đêm. Chat là 1–512 byte; summary tối đa 256 byte. Text chiếm phần body còn lại, không có NUL hay trường độ dài riêng.

Người chết không chat/vote và vẫn nhận chat làng để theo dõi. Chat Sói chỉ trao đổi giữa Sói sống ban đêm. Server giữ phiếu hợp lệ đầu tiên của mỗi người. Khi hết 30 giây, người chưa gửi phiếu được tính là bỏ phiếu trắng. Nếu hòa hoặc toàn phiếu trắng, không ai bị treo.

Lõi game hiện tại chốt sớm khi mọi người chơi còn sống và còn kết nối đã bỏ phiếu. Mục 6.4 của ver2 mô tả việc chốt sau 30 giây. Nhóm cần thống nhất thời điểm chốt phiếu.

Khi có phe thắng, server gửi `GAME_OVER` rồi đưa phòng còn thành viên về trạng thái chờ và gửi `ROOM_UPDATE`. Chủ phòng hiện tại có thể bắt đầu ván mới khi đủ điều kiện. Server hiện phát `winner = 1` khi Dân thắng và `winner = 2` khi Sói thắng.

Khi nhận LEAVE hợp lệ, server xếp phản hồi cho người rời trước. LEAVE và mất kết nối đều gỡ phiên khỏi danh sách thành viên. Nếu người rời là Chủ phòng, quyền chuyển cho người vào sớm nhất còn lại và `ROOM_UPDATE` chứa `host_id` mới. Phòng được xóa khi hết thành viên. Trong ván, server đã gọi `game_handle_player_left()`. Việc cập nhật trạng thái người rời, bỏ hành động hoặc phiếu liên quan và xét lại điều kiện thắng đang chờ `SERVER-11`.

## Lỗi và mã dùng chung

Response lỗi dùng `MSG_RESPONSE (0xFF)` với body rỗng. Các mã thường gặp gồm `0x01` chưa đăng nhập, `0x02` không đủ quyền, `0x03` sai pha, `0x04` mục tiêu sai, `0x05` phòng đầy, `0x06` tên trùng, `0x07` phòng đang chơi, `0x08` payload sai, `0x09` không tìm thấy phòng, `0x0A` thiếu người và `0x0C` lỗi nội bộ.

Client và server cùng dùng [`protocol.h`](../include/protocol.h) và [`protocol.c`](../src/common/protocol.c) để mã hóa header, số nguyên và tên.

## Cần làm rõ

- Chốt giới hạn byte chat/summary và charset hiển thị
