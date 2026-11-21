# Phân công và danh sách task Ma Sói

## Phân công tổng

| Người | Phụ trách |
|---|---|
| **Người 1 · NND** | Server mạng, bộ đóng/mở khung dùng chung, phiên đăng nhập, phòng chờ và quyền Chủ phòng. |
| **Người 2 · NMX** | Lõi game: API/trạng thái ván, chia vai, xử lý đêm, xét thắng và người rời. |
| **Người 3 · NTT** | Client terminal, đồng hồ và luồng ban ngày trên server (chat, vote, kết quả ngày). |

Dùng mục 4–8 của ver2 làm chuẩn khi triển khai và đối chiếu. Các task dưới đây ghi phần việc và tiêu chí bàn giao. [Đặc tả thiết kế ver2](https://docs.google.com/document/d/1o8jz59_vBk5IG-NB3HvIKDmAgUrok2Aztk2Ux5sSsJY/edit?tab=t.s0u7ebxdy021)

## Phần việc còn lại sau mốc login và lobby

NND đã setup server và client hiện có nên nhận phạm vi còn lại nhỏ nhất. NMX phụ trách lõi game; NTT nhận client và SERVER-09 để chia tải logic game. Kiểm thử là trách nhiệm của cả ba. Dấu hoàn thành giữ nguyên theo tiến độ tính năng, không thể hiện tác giả phần đã làm.

Thiết kế và luật chơi tiếp tục theo Google Doc ver2 ở trên. Phần cập nhật này chỉ phân công và sắp thứ tự công việc, không thay đổi thiết kế.

| Người | Task còn lại | Đầu ra |
|---|---|---|
| **NND · Người 1** | **SERVER-04, SERVER-05** | PING/PONG, timeout và dọn phiên; gọi game đúng hạn; kiểm tra quyền/số người bắt đầu; chuyển ID, lệnh, timeout, người rời sang game; gửi sự kiện đúng người nhận; đưa phòng về chờ. Bổ sung hằng số giao thức khi ghép. |
| **NMX · Người 2** | **SERVER-06, 07, 08, 10, 11** | API/trạng thái từng ván; chia vai và thông báo riêng; xử lý đêm; xét thắng/kết thúc; người rời giữa ván. Định nghĩa API/trạng thái để NTT triển khai SERVER-09. |
| **NTT · Người 3** | **CLIENT-03, 04, 05 và SERVER-09** | Hoàn thiện lệnh lobby/quyền Chủ phòng; hiển thị và thao tác trong ván; heartbeat client, đồng hồ, mất kết nối; chat theo pha, nhận/chốt vote, hòa phiếu/hết giờ và kết quả ngày theo ver2. CLIENT-03 đã có nền. |
| **Cả ba** | **CLIENT-06, COMMON-03, 04, 05** | Mỗi người viết/chạy test phần mình; cùng ghép ván 5 người, hai phòng, lỗi kết nối; hoàn thiện script, log, tài liệu và demo. CLIENT-06 đã có bộ test lobby. |

Số task không tương đương thời gian: NND có phạm vi nhỏ nhất nhưng vẫn cần phối hợp ghép game. NMX tập trung lõi luật chơi; NTT chia tải bằng luồng ngày. Không giao toàn bộ kiểm thử hoặc demo cho một người.

## Thứ tự công việc

`→` chỉ phụ thuộc đầu ra. Các nhánh song song có thể triển khai cùng lúc; số task là mã nhận diện, không phải thứ tự bắt buộc.

### Mốc đã có

SERVER-01 + COMMON-01 + CLIENT-01 + CLIENT-02 → SERVER-02 → phần tạo/vào phòng của CLIENT-03 → COMMON-02. SERVER-03 đã xong phía server; CLIENT-03 còn lệnh/quyền và bắt đầu ván. Dùng lại phần nền, không làm lại.

### Bước 1 · Làm song song

- **SERVER-06 (NMX chủ trì, NND và NTT phối hợp):** chốt API khởi tạo/nhận lệnh/hết hạn/người rời, trạng thái và sự kiện/người nhận; cung cấp giao diện luồng ngày cho SERVER-09. Cả ba đối chiếu mã thông điệp/payload với ver2.
- **SERVER-04 (NND):** heartbeat, timeout và đánh thức vòng lặp theo hạn.
- **CLIENT-03 (NTT):** hoàn thiện mời/kick/rời, cập nhật Chủ phòng theo SERVER-03; thêm lệnh bắt đầu theo giao thức đã chốt. Nghiệm thu bắt đầu sau khi SERVER-05/07 sẵn sàng.

### Bước 2 · Triển khai theo từng nhánh

| Người | Thứ tự | Phụ thuộc và cách ghép |
|---|---|---|
| **NND** | SERVER-04 + API SERVER-06 → SERVER-05 | Viết phần chuyển lệnh/sự kiện sau khi chốt API; ghép khởi tạo khi SERVER-07 sẵn sàng; ghép tiếp đêm/ngày/kết thúc/người rời khi từng phần được bàn giao. |
| **NMX** | SERVER-06 → SERVER-07 → SERVER-08 → hoàn thiện SERVER-10 → SERVER-11 | Có thể viết hàm xét thắng SERVER-10 sớm sau khi chốt trạng thái, rồi gọi từ kết quả đêm, kết quả ngày của SERVER-09 và người rời. SERVER-11 phối hợp NTT cập nhật phiếu và NND dọn phiên/chuyển Chủ phòng. |
| **NTT** | CLIENT-03 → SERVER-09 → CLIENT-04 | SERVER-09 dùng API SERVER-06, làm song song với SERVER-08 của NMX; ghép đêm → ngày khi SERVER-08 có đầu ra. CLIENT-04 có thể dựng từng phần bằng sự kiện mẫu đã chốt, nghiệm thu với server thật khi SERVER-05/07/08/09/10 sẵn sàng. |
| **NTT** | SERVER-04 (NND) + định dạng hạn pha trong SERVER-06 (NMX) → CLIENT-05 | Làm xen kẽ với SERVER-09/CLIENT-04 sau khi chốt PING/PONG và hạn pha, không cần chờ toàn bộ luật chơi hoàn tất. |

### Bước 3 · Ghép ván hoàn chỉnh

SERVER-05 (NND) + SERVER-07/08/10 (NMX) + SERVER-09 và CLIENT-03/04/05 (NTT) → **COMMON-03 (cả ba)**: năm client bắt đầu, nhận vai, qua đêm/ngày/vote và kết thúc; thử pha không ai thao tác và chơi ván mới.

### Bước 4 · Lỗi kết nối và bàn giao

COMMON-03 + SERVER-11 (NMX) + SERVER-04/05 (NND) + CLIENT-05 (NTT) → **COMMON-04 (cả ba)**: hai phòng song song, ngắt client và Chủ phòng, phòng còn lại tiếp tục → hoàn tất **CLIENT-06 (cả ba)** → **COMMON-05 (cả ba)**.

Viết/chạy test cùng từng task; cuối cùng tổng hợp và chạy lại bộ tích hợp:

| Người | Kiểm thử phụ trách |
|---|---|
| **NND** | Heartbeat/timeout, quyền/số người bắt đầu, dọn phiên, chuyển Chủ phòng, gửi sự kiện đúng người nhận và hai phòng độc lập. |
| **NMX** | Chia vai/thông tin riêng, hành động đêm, sai pha/vai/mục tiêu, hết hạn, xét thắng và người rời ảnh hưởng game. |
| **NTT** | Lệnh/hiển thị client, quyền lobby, chat theo pha, vote hợp lệ/sai/hòa/hết giờ, đồng hồ và báo mất kết nối. |
| **Cả ba** | Ván 5 người, hai phòng, lỗi kết nối; script chạy lại, kết quả mong đợi, log và demo. |

## Breakdown task

Các ô chưa đánh dấu còn phần việc hoặc tiêu chí nghiệm thu. [Giao thức và triển khai server phòng chờ](PROTOCOL.md) ghi chi tiết giao thức, gói mẫu và cách chạy kiểm thử.

CLIENT-03 và CLIENT-06 đã có các lệnh xem, tạo, vào phòng và bộ kiểm thử tích hợp phòng chờ.

### Người 1 · NND · Server mạng, phiên và phòng chờ

| Task | Việc cần làm | Hoàn thành khi |
|---|---|---|
| [x] **SERVER-01 · Nghiệm thu mã TCP và đăng nhập** | Rà vòng lặp `poll()`, bộ đệm từng kết nối, giải mã header, gửi từng phần, xử lý EOF và timeout khung dở. Giữ phần định danh đã có trong `src/server/session.c`. | Hai client kết nối đồng thời. Khung bị chia nhỏ hoặc đến liền nhau vẫn được xử lý đủ. Sai khung theo ver2 chỉ ảnh hưởng kết nối gửi lỗi. Tên trùng bị từ chối khi phiên trước còn kết nối và dùng lại được sau khi phiên đó rời. |
| [x] **COMMON-01 · Bàn giao bộ đóng khung và gói mẫu** | Chốt API C trong `include/protocol.h`. Cung cấp byte mẫu cho một yêu cầu đăng nhập và một phản hồi. Cập nhật hằng số dùng chung theo các nhóm thông điệp khi ghép tính năng. | Người 3 đọc và ghi được cùng cặp gói mẫu. Header và mã lệnh không bị định nghĩa riêng ở client. |
| [x] **SERVER-02 · Danh sách phòng, tạo và vào phòng** | Thêm danh sách phòng và thao tác tạo/vào phòng. Quản lý ID phòng, ID thành viên, sức chứa và quyền của người tạo. Trả phản hồi và gửi `MSG_ROOM_UPDATE` theo ver2. | Hai client đăng nhập, xem danh sách, vào cùng phòng và nhận cùng danh sách thành viên. Ca phòng không tồn tại hoặc đã đầy trả mã lỗi theo ver2. |
| [x] **SERVER-03 · Mời, loại, rời và chuyển Chủ phòng** | Xử lý yêu cầu mời, loại và rời phòng. Kiểm tra quyền tại server. Cập nhật danh sách và Chủ phòng khi một phiên rời hoặc mất kết nối. | Thành viên không đủ quyền bị từ chối. Khi Chủ phòng rời, những client còn lại nhận được `host_id` mới. Phòng rỗng được dọn. |
| [ ] **SERVER-04 · Heartbeat và thời hạn** | Xử lý `MSG_PING` / `MSG_PONG` với mốc monotonic. Theo dõi lần nhận dữ liệu, đóng kết nối quá hạn và dùng chung đường dọn phiên với EOF hoặc lỗi socket. Cho `poll()` thức dậy đúng hạn để gọi mô-đun game. | Client nhận PONG đúng dữ liệu và `request_id`. Một client hết hạn bị dọn mà phòng khác vẫn hoạt động. Server gọi game khi đến hạn dù không nhận thêm dữ liệu mạng. |
| [ ] **SERVER-05 · Nối phòng chờ với game** | Sau khi kiểm tra quyền và số người, truyền danh sách ID vào hàm khởi tạo của Người 2. Chuyển lệnh, sự kiện hết hạn và người rời tới game. Mã hóa và gửi sự kiện theo danh sách người nhận Người 2 trả về. | Sau SERVER-02 và SERVER-06, ghép được nút Bắt đầu với một ván thật. Khi Chủ phòng mất kết nối, đường dọn phiên chỉ gọi xử lý rời ván một lần và chuyển quyền phòng đúng chỗ. |

### Người 2 · NMX · Luật chơi trên server

| Task | Việc cần làm | Hoàn thành khi |
|---|---|---|
| [x] **SERVER-06 · Giao diện C và trạng thái ván** | NMX chủ trì, cùng NND và NTT chốt các hàm khởi tạo, nhận lệnh, xử lý hết hạn và xử lý người rời. Game nhận ID và thời điểm monotonic, trả sự kiện và người nhận. Giữ trạng thái từng ván riêng; cung cấp API/trạng thái để NTT triển khai SERVER-09. | Chạy được mô-đun với danh sách người chơi và đồng hồ giả lập, không cần socket. Người 1 có chữ ký hàm để viết phần ghép. |
| [x] **SERVER-07 · Khởi tạo ván và chia vai** | Tạo trạng thái chơi từ danh sách của phòng, chia vai, tạo thông báo riêng và mở pha đầu tiên. | Ván 5 người có đủ trạng thái, kể cả Chủ phòng. Mỗi thông báo vai có đúng người nhận. |
| [x] **SERVER-08 · Nhận và phân xử hành động đêm** | Kiểm tra pha, vai, trạng thái sống và mục tiêu. Ghi hành động hợp lệ, xử lý các hiệu ứng theo ver2 và phát kết quả. | Ca có hành động hợp lệ và ca hết giờ không ai hành động đều đi tiếp đúng pha. Thông tin riêng chỉ đến đúng vai. |
| [x] **SERVER-10 · Xét thắng và kết thúc ván** | Xét kết quả sau các sự kiện có thể đổi số người chơi còn sống. Trả kết quả cuối ván và tín hiệu để Người 1 đưa phòng về trạng thái chờ. | Ván kết thúc đúng phe thắng theo ver2. Phòng có thể khởi tạo ván mới với trạng thái game mới. |
| [ ] **SERVER-11 · Người rời giữa ván** | Xử lý người chơi rời ở từng pha, kể cả Chủ phòng. Cập nhật hành động hoặc phiếu liên quan, xét thắng và trả các sự kiện cần gửi. | Ca người rời giữa đêm hoặc lúc bỏ phiếu không làm kẹt ván. Nếu ván tiếp tục, giữ hạn pha hiện tại theo ver2. Người 1 xử lý quyền Chủ phòng ở lớp phòng. |

### Người 3 · NTT · Client và luồng ban ngày trên server

| Task | Việc cần làm | Hoàn thành khi |
|---|---|---|
| [x] **CLIENT-01 · Nghiệm thu client đăng nhập** | Dùng client dòng lệnh hiện có để kiểm tra gửi `MSG_LOGIN_REQ`, ghép phản hồi theo `request_id` và hiển thị lỗi. Cho client giữ phiên sau khi đăng nhập để nhận lệnh tiếp theo. | Đăng nhập thành công hoặc thất bại được hiển thị rõ. Client giữ kết nối sau khi đăng nhập thành công. |
| [x] **CLIENT-02 · Vòng lặp client và xử lý thông báo đẩy** | Tách đọc lệnh terminal khỏi đọc socket. Dùng bộ đóng khung chung và phân biệt phản hồi yêu cầu với thông báo server đẩy. Bắt đầu bằng byte mẫu hoặc server giả lập. | Client vẫn nhận được thông báo khi đang chờ người dùng nhập lệnh. Hai phản hồi liên tiếp ghép đúng yêu cầu của chúng. |
| [ ] **SERVER-09 · Thảo luận, bỏ phiếu và kết quả ngày** | Lọc người gửi và người nhận tin theo pha. Nhận phiếu, kiểm tra tính hợp lệ, chốt phiếu và phát kết quả ngày. | Kiểm tra được ca có kết quả, hòa phiếu, hết giờ không có phiếu hợp lệ và thao tác sai pha. |
| [ ] **CLIENT-03 · Lệnh phòng chờ và trạng thái thành viên** | Thêm lệnh xem danh sách, tạo/vào phòng, mời, loại, rời và bắt đầu ván. Cập nhật danh sách và quyền từ thông báo server. | Sau SERVER-02, hai client ghép được luồng tạo/vào phòng. Sau SERVER-03, client hiển thị đúng Chủ phòng mới và thông báo lỗi quyền. |
| [ ] **CLIENT-04 · Trạng thái và thao tác trong ván** | Hiển thị vai riêng, pha, người còn sống và kết quả. Gửi lệnh thao tác đêm, chat và bỏ phiếu. Cập nhật trạng thái khi nhận sự kiện từ server. | Sau SERVER-05 và SERVER-07 đến SERVER-10, chạy được một ván đủ 5 client từ chia vai tới kết quả. |
| [ ] **CLIENT-05 · Đồng hồ và mất kết nối** | Gửi PING định kỳ, dùng PONG để ước lượng lệch đồng hồ và hiển thị đếm ngược theo hạn server gửi. Xử lý đóng kết nối, dọn trạng thái và báo cho người dùng. | Sau SERVER-04, hai client nhận cùng hạn pha. Khi client hết thời gian hiển thị, pha chỉ đổi khi có thông báo từ server. Ngắt server thì client báo mất kết nối. |

### Task chung của nhóm

| Task | Ai làm | Hoàn thành khi |
|---|---|---|
| [ ] **CLIENT-06 · Kịch bản tích hợp và demo** | Cả ba; mỗi người kiểm thử phần mình theo bảng trên | Bổ sung từ bộ test lobby hiện có: ván 5 người, hai phòng và lỗi kết nối. Có lệnh chạy lại, kết quả mong đợi và log; cùng kiểm tra luật và vận hành theo barem. |
| [x] **COMMON-02 · Phòng chờ** | Người 1 + Người 3 | Ghép ngay khi hai bên có phần đăng nhập và tạo/vào phòng. Hai client thật đăng nhập, vào cùng phòng và nhận cùng bản cập nhật. |
| [ ] **COMMON-03 · Một ván hoàn chỉnh** | Người 1 + Người 2 + Người 3 | Sau SERVER-05 và CLIENT-04, năm client chơi qua đêm, ngày, bỏ phiếu và kết thúc. Kiểm tra thêm một pha không ai thao tác. |
| [ ] **COMMON-04 · Hai phòng và lỗi kết nối** | Cả ba: NND kiểm tra mạng, NMX kiểm tra game, NTT kiểm tra client và luồng ngày | Sau heartbeat và xử lý rời ván, ngắt một client ở một phòng. Phòng còn lại tiếp tục. Kiểm tra cả lúc Chủ phòng mất kết nối. |
| [ ] **COMMON-05 · Rà tài liệu và nộp** | Cả nhóm | Người 1 đối chiếu phần mạng và luồng phòng, NMX đối chiếu lõi game, NTT đối chiếu luồng ngày, client và cách chạy; cả ba chuẩn bị demo. Build trên Linux bằng GCC và Makefile với `-std=c11 -Wall -Wextra`. Lưu log, bằng chứng và script theo barem. |
