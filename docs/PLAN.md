# Phân công và danh sách task Ma Sói

## Phân công tổng

| Người | Phụ trách |
|---|---|
| **Người 1 · NTT** | Server mạng, bộ đóng/mở khung dùng chung, phiên đăng nhập, phòng chờ và quyền Chủ phòng. |
| **Người 2 · NMX** | Mô-đun game trên server, từ chia vai đến phân xử và kết thúc ván. |
| **Người 3 · NND** | Client terminal, trạng thái phía client, đồng hồ hiển thị và kiểm thử tích hợp. |

Dùng mục 4–8 của ver2 làm chuẩn khi triển khai và đối chiếu. Các task dưới đây ghi phần việc và tiêu chí bàn giao. [Đặc tả thiết kế ver2](https://docs.google.com/document/d/1o8jz59_vBk5IG-NB3HvIKDmAgUrok2Aztk2Ux5sSsJY/edit?tab=t.s0u7ebxdy021)

## Breakdown task

Các ô chưa đánh dấu còn phần việc hoặc tiêu chí nghiệm thu. Task 1 đã qua luồng hai client thật, JOIN phòng không tồn tại, cập nhật tự động và dọn phiên trên Ubuntu WSL. Chạy lại bằng `make` và `python3 tests/lobby.py`. [Đặc tả và gói mẫu Task 1](TASK-01-LOBBY.md).

SERVER-02 đã có kiểm tra sức chứa. Ca phòng đầy được hoãn. COMMON-01 đã có codec và gói mẫu để bàn giao. CLIENT-03, SERVER-03 và CLIENT-06 đã có phần xem/tạo/vào phòng, dọn thành viên khi ngắt kết nối và kiểm thử tích hợp phòng chờ tương ứng.

### Người 1 · Server mạng, phiên và phòng chờ

| Task | Việc cần làm | Hoàn thành khi |
|---|---|---|
| [x] **SERVER-01 · Nghiệm thu mã TCP và đăng nhập** | Rà vòng lặp `poll()`, bộ đệm từng kết nối, giải mã header, gửi từng phần, xử lý EOF và timeout khung dở. Giữ phần định danh đã có trong `src/server/session.c`. | Hai client kết nối đồng thời. Khung bị chia nhỏ hoặc đến liền nhau vẫn được xử lý đủ. Sai khung theo ver2 chỉ ảnh hưởng kết nối gửi lỗi. Tên trùng bị từ chối khi phiên trước còn kết nối và dùng lại được sau khi phiên đó rời. |
| [ ] **COMMON-01 · Bàn giao bộ đóng khung và gói mẫu** | Chốt API C trong `include/protocol.h`. Cung cấp byte mẫu cho một yêu cầu đăng nhập và một phản hồi. Cập nhật hằng số dùng chung theo các nhóm thông điệp khi ghép tính năng. | Người 3 đọc và ghi được cùng cặp gói mẫu. Header và mã lệnh không bị định nghĩa riêng ở client. |
| [ ] **SERVER-02 · Danh sách phòng, tạo và vào phòng** | Thêm danh sách phòng và thao tác tạo/vào phòng. Quản lý ID phòng, ID thành viên, sức chứa và quyền của người tạo. Trả phản hồi và gửi `MSG_ROOM_UPDATE` theo ver2. | Hai client đăng nhập, xem danh sách, vào cùng phòng và nhận cùng danh sách thành viên. Ca phòng không tồn tại hoặc đã đầy trả mã lỗi theo ver2. |
| [ ] **SERVER-03 · Mời, loại, rời và chuyển Chủ phòng** | Xử lý yêu cầu mời, loại và rời phòng. Kiểm tra quyền tại server. Cập nhật danh sách và Chủ phòng khi một phiên rời hoặc mất kết nối. | Thành viên không đủ quyền bị từ chối. Khi Chủ phòng rời, những client còn lại nhận được `host_id` mới. Phòng rỗng được dọn. |
| [ ] **SERVER-04 · Heartbeat và thời hạn** | Xử lý `MSG_PING` / `MSG_PONG` với mốc monotonic. Theo dõi lần nhận dữ liệu, đóng kết nối quá hạn và dùng chung đường dọn phiên với EOF hoặc lỗi socket. Cho `poll()` thức dậy đúng hạn để gọi mô-đun game. | Client nhận PONG đúng dữ liệu và `request_id`. Một client hết hạn bị dọn mà phòng khác vẫn hoạt động. Server gọi game khi đến hạn dù không nhận thêm dữ liệu mạng. |
| [ ] **SERVER-05 · Nối phòng chờ với game** | Sau khi kiểm tra quyền và số người, truyền danh sách ID vào hàm khởi tạo của Người 2. Chuyển lệnh, sự kiện hết hạn và người rời tới game. Mã hóa và gửi sự kiện theo danh sách người nhận Người 2 trả về. | Sau SERVER-02 và SERVER-06, ghép được nút Bắt đầu với một ván thật. Khi Chủ phòng mất kết nối, đường dọn phiên chỉ gọi xử lý rời ván một lần và chuyển quyền phòng đúng chỗ. |

### Người 2 · Luật chơi trên server

| Task | Việc cần làm | Hoàn thành khi |
|---|---|---|
| [ ] **SERVER-06 · Giao diện C và trạng thái ván** | Cùng Người 1 chốt các hàm khởi tạo, nhận lệnh, xử lý hết hạn và xử lý người rời. Game nhận ID và thời điểm monotonic, trả sự kiện và người nhận. Giữ trạng thái từng ván riêng. | Chạy được mô-đun với danh sách người chơi và đồng hồ giả lập, không cần socket. Người 1 có chữ ký hàm để viết phần ghép. |
| [ ] **SERVER-07 · Khởi tạo ván và chia vai** | Tạo trạng thái chơi từ danh sách của phòng, chia vai, tạo thông báo riêng và mở pha đầu tiên. | Ván 5 người có đủ trạng thái, kể cả Chủ phòng. Mỗi thông báo vai có đúng người nhận. |
| [ ] **SERVER-08 · Nhận và phân xử hành động đêm** | Kiểm tra pha, vai, trạng thái sống và mục tiêu. Ghi hành động hợp lệ, xử lý các hiệu ứng theo ver2 và phát kết quả. | Ca có hành động hợp lệ và ca hết giờ không ai hành động đều đi tiếp đúng pha. Thông tin riêng chỉ đến đúng vai. |
| [ ] **SERVER-09 · Thảo luận, bỏ phiếu và kết quả ngày** | Lọc người gửi và người nhận tin theo pha. Nhận phiếu, kiểm tra tính hợp lệ, chốt phiếu và phát kết quả ngày. | Kiểm tra được ca có kết quả, hòa phiếu, hết giờ không có phiếu hợp lệ và thao tác sai pha. |
| [ ] **SERVER-10 · Xét thắng và kết thúc ván** | Xét kết quả sau các sự kiện có thể đổi số người chơi còn sống. Trả kết quả cuối ván và tín hiệu để Người 1 đưa phòng về trạng thái chờ. | Ván kết thúc đúng phe thắng theo ver2. Phòng có thể khởi tạo ván mới với trạng thái game mới. |
| [ ] **SERVER-11 · Người rời giữa ván** | Xử lý người chơi rời ở từng pha, kể cả Chủ phòng. Cập nhật hành động hoặc phiếu liên quan, xét thắng và trả các sự kiện cần gửi. | Ca người rời giữa đêm hoặc lúc bỏ phiếu không làm kẹt ván. Nếu ván tiếp tục, giữ hạn pha hiện tại theo ver2. Người 1 xử lý quyền Chủ phòng ở lớp phòng. |

### Người 3 · Client và kiểm thử tích hợp

| Task | Việc cần làm | Hoàn thành khi |
|---|---|---|
| [x] **CLIENT-01 · Nghiệm thu client đăng nhập** | Dùng client dòng lệnh hiện có để kiểm tra gửi `MSG_LOGIN_REQ`, ghép phản hồi theo `request_id` và hiển thị lỗi. Cho client giữ phiên sau khi đăng nhập để nhận lệnh tiếp theo. | Đăng nhập thành công hoặc thất bại được hiển thị rõ. Client giữ kết nối sau khi đăng nhập thành công. |
| [x] **CLIENT-02 · Vòng lặp client và xử lý thông báo đẩy** | Tách đọc lệnh terminal khỏi đọc socket. Dùng bộ đóng khung chung và phân biệt phản hồi yêu cầu với thông báo server đẩy. Bắt đầu bằng byte mẫu hoặc server giả lập. | Client vẫn nhận được thông báo khi đang chờ người dùng nhập lệnh. Hai phản hồi liên tiếp ghép đúng yêu cầu của chúng. |
| [ ] **CLIENT-03 · Lệnh phòng chờ và trạng thái thành viên** | Thêm lệnh xem danh sách, tạo/vào phòng, mời, loại, rời và bắt đầu ván. Cập nhật danh sách và quyền từ thông báo server. | Sau SERVER-02, hai client ghép được luồng tạo/vào phòng. Sau SERVER-03, client hiển thị đúng Chủ phòng mới và thông báo lỗi quyền. |
| [ ] **CLIENT-04 · Trạng thái và thao tác trong ván** | Hiển thị vai riêng, pha, người còn sống và kết quả. Gửi lệnh thao tác đêm, chat và bỏ phiếu. Cập nhật trạng thái khi nhận sự kiện từ server. | Sau SERVER-05 và SERVER-07 đến SERVER-10, chạy được một ván đủ 5 client từ chia vai tới kết quả. |
| [ ] **CLIENT-05 · Đồng hồ và mất kết nối** | Gửi PING định kỳ, dùng PONG để ước lượng lệch đồng hồ và hiển thị đếm ngược theo hạn server gửi. Xử lý đóng kết nối, dọn trạng thái và báo cho người dùng. | Sau SERVER-04, hai client nhận cùng hạn pha. Khi client hết thời gian hiển thị, pha chỉ đổi khi có thông báo từ server. Ngắt server thì client báo mất kết nối. |
| [ ] **CLIENT-06 · Kịch bản tích hợp và demo** | Viết kịch bản hai client vào phòng, một ván 5 người, hai phòng song song và lỗi một client. Phối hợp với Người 1 chạy tải và với Người 2 kiểm tra đúng kết quả. | Có lệnh chạy lại từng kịch bản, kết quả mong đợi và bằng chứng vận hành theo barem. |

### Task chung của nhóm

| Task | Ai làm | Hoàn thành khi |
|---|---|---|
| [x] **COMMON-02 · Phòng chờ** | Người 1 + Người 3 | Ghép ngay khi hai bên có phần đăng nhập và tạo/vào phòng. Hai client thật đăng nhập, vào cùng phòng và nhận cùng bản cập nhật. |
| [ ] **COMMON-03 · Một ván hoàn chỉnh** | Người 1 + Người 2 + Người 3 | Sau SERVER-05 và CLIENT-04, năm client chơi qua đêm, ngày, bỏ phiếu và kết thúc. Kiểm tra thêm một pha không ai thao tác. |
| [ ] **COMMON-04 · Hai phòng và lỗi kết nối** | Người 3 điều phối, Người 1 và Người 2 cùng kiểm tra | Sau heartbeat và xử lý rời ván, ngắt một client ở một phòng. Phòng còn lại tiếp tục. Kiểm tra cả lúc Chủ phòng mất kết nối. |
| [ ] **COMMON-05 · Rà tài liệu và nộp** | Cả nhóm | Người 1 đối chiếu phần mạng và luồng phòng, Người 2 đối chiếu sơ đồ game, Người 3 đối chiếu cách chạy và demo. Build trên Linux bằng GCC và Makefile với `-std=c11 -Wall -Wextra`. Lưu log, bằng chứng và script theo barem. |
