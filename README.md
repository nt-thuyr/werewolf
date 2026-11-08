# Werewolf

Trò chơi Ma Sói qua TCP, viết bằng C cho Linux. Bản hiện tại hỗ trợ đăng nhập, xem danh sách, tạo và vào phòng chờ.

## Cấu trúc dự án

- `src/` chứa mã nguồn server, client và phần giao thức dùng chung.
- `include/` chứa các header dùng chung.
- `docs/` chứa [kế hoạch và phân công](docs/PLAN.md), [ghi chú triển khai](docs/DEVELOPMENT.md) và [đặc tả Task 1](docs/TASK-01-LOBBY.md).

## Cài đặt

Trong máy ảo Ubuntu hoặc Debian, cài công cụ và clone repo.

```bash
sudo apt update
sudo apt install git build-essential
git clone https://github.com/nt-thuyr/werewolf.git
cd werewolf
make
```

## Chạy thử

Chạy server trong terminal thứ nhất.

```bash
./build/server
```

Trong terminal thứ hai, mở cùng thư mục dự án rồi chạy client.

```bash
./build/client 127.0.0.1 alice
```

Mở terminal thứ ba và chạy `./build/client 127.0.0.1 bob`. Sau khi cả hai đăng nhập, nhập `create LangSoi 5` ở Alice. Ở Bob, nhập `list`, thử `join 9999` nếu ID này chưa tồn tại, rồi nhập `join <ID phòng vừa hiện>`. Để yên terminal Alice khi Bob tham gia. Cả hai client tự hiển thị cùng ID phòng, ID Chủ phòng và danh sách thành viên.

Dùng `Ctrl+D` tại dấu nhắc để đóng client và `Ctrl+C` để dừng server. Để chạy lại kiểm thử tích hợp trên Linux, dừng server đang chạy thử rồi chạy `make` và `python3 tests/lobby.py` từ gốc repository.
