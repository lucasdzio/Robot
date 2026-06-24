# ESP32-C3 ROB Motor + WiFi Setup (ESP-IDF)

Project này điều khiển 2 motor theo chu kỳ test và có trang cấu hình WiFi bằng điện thoại.

Khi thiết bị chưa có WiFi đã lưu, hoặc WiFi cũ kết nối thất bại, ESP32-C3 sẽ quét các WiFi xung quanh trước, sau đó phát mạng:

- Tên WiFi: `ROB-SETUP`
- Mật khẩu: `12345678`
- Trang cấu hình: `http://192.168.4.1`

Bạn kết nối điện thoại vào mạng `ROB-SETUP`, mở trình duyệt vào địa chỉ trên, chọn WiFi nhà trong danh sách rồi nhập mật khẩu. Nếu WiFi bị ẩn hoặc không hiện trong danh sách, vẫn có ô nhập tên WiFi thủ công. Thiết bị sẽ lưu cấu hình vào NVS rồi tự khởi động lại.

## 1) Chuẩn bị môi trường

Đảm bảo bạn đã cài ESP-IDF (ví dụ trong `~/esp/esp-idf`).

Nếu là lần đầu setup ESP-IDF trên máy, cài toolchain trước:

```bash
cd ~/esp/esp-idf
./install.sh esp32c3
```

Nạp environment cho terminal trước khi build:

```bash
. ~/esp/esp-idf/export.sh
```

> Nếu dùng đường dẫn khác, đổi lại cho đúng thư mục cài ESP-IDF của bạn.

Kiểm tra compiler đã sẵn sàng:

```bash
which riscv32-esp-elf-gcc
which xtensa-esp32-elf-gcc
```

Nếu 2 lệnh trên không ra path, chạy lại `export.sh` hoặc mở terminal mới rồi chạy lại.

## 2) Build project

Trong thư mục project (`rob`), chạy:

```bash
idf.py set-target esp32c3
idf.py build
```

## 3) Flash firmware lên board

### Cách nhanh (tự tìm cổng)

```bash
idf.py -p /dev/cu.usbmodem* flash
```

### Hoặc chỉ định cổng cụ thể

Ví dụ:

```bash
idf.py -p /dev/cu.usbmodem1101 flash
```

## 4) Mở serial monitor

```bash
idf.py -p /dev/cu.usbmodem1101 monitor
```

Thoát monitor bằng tổ hợp phím:

- `Ctrl + ]`

## 5) Build + flash + monitor một lệnh

```bash
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

## 6) Kết quả mong đợi

Lần đầu chạy, bạn sẽ thấy log kiểu:

```text
No saved WiFi, starting config portal
Scanning WiFi networks before starting config portal
Found WiFi: Ten_WiFi_Cua_Ban (-45 dBm)
Config portal ready: connect to ROB-SETUP, open http://192.168.4.1
```

Sau khi cấu hình thành công và thiết bị reboot, log sẽ báo địa chỉ IP:

```text
Connecting to saved WiFi: Ten_WiFi_Cua_Ban
WiFi connected, IP: 192.168.x.x
Start internet ping to 103.82.21.138 every 5 seconds
Internet ping OK: 103.82.21.138 seq=1 bytes=64 ttl=52 time=12ms
```

Nếu WiFi có IP nhưng không ra internet hoặc server không phản hồi ping, log sẽ báo:

```text
Internet ping TIMEOUT: 103.82.21.138 seq=1
```

Motor vẫn chạy chu kỳ test:

```text
Both motors left
Both motors stop
Both motors right
Both motors stop
```

## 7) Cấu hình lại WiFi

Nếu đổi WiFi nhà, giữ nút `BOOT` trên board trong 5 giây khi thiết bị đang chạy.

Thiết bị sẽ xóa WiFi đã lưu, tự khởi động lại, quét WiFi xung quanh rồi phát lại mạng `ROB-SETUP`. Sau đó dùng điện thoại cấu hình lại như lần đầu.

Serial monitor sẽ có log kiểu:

```text
Keep holding BOOT for 5 seconds to reset WiFi
BOOT held for 5 seconds, erasing saved WiFi and restarting
```

## 8) Clean build khi gặp lỗi lạ

```bash
idf.py fullclean
idf.py build
```

## 9) Lỗi thường gặp

- `CMAKE_C_COMPILER ... was not found in the PATH`:
	- Bạn chưa nạp đúng môi trường ESP-IDF trong terminal hiện tại.
	- Chạy lại:

```bash
. ~/esp/esp-idf/export.sh
idf.py build
```
