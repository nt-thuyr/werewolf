# Giao thức client–server

**Thiết kế tham chiếu:** [Google Doc, ver2](https://docs.google.com/document/d/1o8jz59_vBk5IG-NB3HvIKDmAgUrok2Aztk2Ux5sSsJY/edit?tab=t.s0u7ebxdy021).

## Quy ước chung

- Mỗi frame gồm header 8 byte và body tối đa 4096 byte.
- Header: `msg_type:u8`, `length:u16`, `request_id:u32`, `status_code:u8`. Số nhiều byte dùng big-endian; `length` chỉ tính body. Không gửi nguyên struct C.
- Request có `request_id != 0`, `status_code = 0`. Response giữ nguyên request ID. Server push dùng ID 0, status 0.
- `Name` là `len:u8` và 1–32 byte tên, không chứa NUL. Phòng nhận 5–12 người, tính cả host.

## Phòng chờ

| Type | Hướng | Body | Kết quả chính |
| --- | --- | --- | --- |
| `0x01 LOGIN` | C→S | Tên thô, 1–32 byte | Response rỗng; tên phải chưa được dùng. |
| `0x02 ROOM_LIST_REQ` | C→S | Rỗng | `0x03 ROOM_LIST_RESP`: `count:u16`, rồi các `RoomInfo`. |
| `0x04 CREATE_ROOM_REQ` | C→S | `room_name:Name`, `max_players:u8` | Tạo phòng, người tạo làm host. |
| `0x05 JOIN_ROOM_REQ` | C→S | `room_id:u32` | Vào phòng nếu còn chỗ. |
| `0x06 LEAVE_ROOM_REQ` | C→S | Rỗng | Rời phòng, kết nối vẫn ở sảnh. |
| `0x07 ROOM_UPDATE` | S→phòng | `room_id:u32`, `host_id:u32`, `count:u8`, `PlayerEntry[]` | Snapshot đầy đủ thành viên. |
| `0x10 INVITE_REQ` | C→S | `target_user:Name` | Host mời người đang ở sảnh. |
| `0x11 INVITE_NOTIF` | S→C | `room_id:u32`, `host_name:Name` | Người nhận có thể JOIN. |
| `0x12 KICK_REQ` | C→S | `target_user_id:u32` | Chỉ host được loại thành viên khác. |
| `0x13 KICK_NOTIF` | S→C | `reason:u8` (`0x01`: host loại) | Client về sảnh. |
| `0x14 START_GAME_REQ` | C→S | Rỗng | Chỉ host bắt đầu ván đủ người. |

`RoomInfo = room_id:u32, room_name:Name, player_count:u8, max_players:u8`.
`PlayerEntry = player_id:u32, username:Name`.

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

Người chết không chat/vote; vẫn nhận chat làng để theo dõi. Chat Sói chỉ trao đổi giữa Sói sống ban đêm. Phiếu hợp lệ đầu tiên được giữ, không đổi phiếu. Chốt sớm khi mọi người chơi sống và kết nối đã vote; hết 30 giây thì phiếu chưa gửi tính trắng. Hòa hoặc toàn phiếu trắng: không ai bị treo.

## Lỗi và mã dùng chung

Response lỗi dùng `MSG_RESPONSE (0xFF)`, body rỗng. Các mã chính: `0x01` chưa đăng nhập, `0x02` không đủ quyền, `0x04` mục tiêu sai, `0x05` phòng đầy, `0x06` tên trùng, `0x08` payload sai, `0x09` không tìm thấy phòng. Logic game dùng thêm `ERR_WRONG_PHASE`, `ERR_NOT_ENOUGH_PLAYERS`, `ERR_INTERNAL` trong `protocol.h`.

Client và server cùng dùng [`protocol.h`](../include/protocol.h) và [`protocol.c`](../src/common/protocol.c) để mã hóa header, số nguyên và tên.

## Cần làm rõ

- Host rời ván (chuyển host? hủy ván?)
- Mất kết nối giữa ván (vote và điều kiện thắng)
- Chốt giới hạn byte chat/summary và charset hiển thị
