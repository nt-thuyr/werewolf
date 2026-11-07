# Werewolf

Trò chơi Ma Sói qua TCP, viết bằng C cho Linux. Bản hiện tại hỗ trợ kết nối và đăng nhập.

## Cấu trúc dự án

- `src/` chứa mã nguồn server, client và phần giao thức dùng chung.
- `include/` chứa các header dùng chung.
- `docs/` chứa [kế hoạch và phân công](docs/PLAN.md) cùng [ghi chú triển khai](docs/DEVELOPMENT.md).

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

Khi đăng nhập thành công, client in `Logged in as alice` rồi thoát. Dùng `Ctrl+C` để dừng server.
