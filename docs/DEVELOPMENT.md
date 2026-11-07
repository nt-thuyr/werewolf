# Ghi chú triển khai

Mốc đăng nhập dùng header 8 byte, thứ tự byte mạng và trả lại `request_id` theo ver2. Body của `MSG_LOGIN_REQ` là tên dài 1–32 byte, không có tiền tố độ dài riêng hay byte NUL. Tên được so sánh theo byte và phân biệt hoa thường. Bản hiện tại nhận tối đa 64 kết nối đồng thời. [Đặc tả giao thức ver2](https://docs.google.com/document/d/1o8jz59_vBk5IG-NB3HvIKDmAgUrok2Aztk2Ux5sSsJY/edit?tab=t.s0u7ebxdy021)

## Làm việc với Git và máy ảo

Windows và máy ảo Linux dùng hai bản clone của cùng một repo. Commit và push nhánh đang làm từ Windows. Trong máy ảo, dùng `git fetch origin`, chuyển sang nhánh đó rồi chạy `git pull --ff-only` để cập nhật trước khi build.
