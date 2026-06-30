# Kiến trúc hệ thống Claim Device (IoT + MQTTS)

Tài liệu mô tả kiến trúc **độc lập phần cứng** cho hệ thống **nhận quyền sở hữu (claim)**, **chia sẻ (share)** và **điều khiển thiết bị IoT** qua **MQTT over TLS (MQTTS)**.

> ⚠️ Tài liệu này mô tả **"thiết bị cần làm gì"**, KHÔNG gắn với một loại chip cụ thể.
> Mọi vi điều khiển có WiFi/Ethernet/4G + đủ RAM cho TLS đều triển khai được
> (ESP32, ESP8266, STM32 + module mạng, nRF, Raspberry Pi Pico W, ...).
> Phần ánh xạ sang từng nền tảng xem **Mục 11**.

---

## 1. Tổng quan

Hệ thống gồm **3 thành phần**, giao tiếp qua **1 broker MQTTS trung tâm**:

```
        ┌─────────────────────────────────────────────────────────┐
        │                    ☁️  CLOUD / SERVER                    │
        │   ┌──────────────┐         ┌──────────────────────┐     │
        │   │ BACKEND API  │◄───────►│  DATABASE             │     │
        │   │  claim/share │         │  users / devices      │     │
        │   │  auth (JWT)  │         │  device_users         │     │
        │   └──────┬───────┘         │  share_invites        │     │
        │          │                 └──────────────────────┘     │
        │   ┌──────▼───────────────────────────────────────┐     │
        │   │           📡  BROKER MQTTS (TLS 8883)         │     │
        │   │            auth + ACL theo database           │     │
        │   └──────▲───────────────────────────────▲───────┘     │
        └──────────┼───────────────────────────────┼─────────────┘
                   │ MQTTS                          │ MQTTS / REST
         ┌─────────┴─────────┐             ┌────────┴──────────┐
         │   🤖 THIẾT BỊ     │             │   📱 APP           │
         │   (MCU bất kỳ)    │             │   (web/mobile)     │
         │   device_id       │             │   user + token     │
         │   device_secret   │             │   điều khiển       │
         └───────────────────┘             └────────────────────┘
```

| Thành phần | Vai trò | Triển khai bằng |
|---|---|---|
| 🤖 **Thiết bị** | Định danh, nhận lệnh, báo trạng thái | **MCU bất kỳ** (firmware) |
| 📡 **Broker** | Trung chuyển tin, xác thực + phân quyền | EMQX / Mosquitto / HiveMQ / runagent... |
| ⚙️ **Backend** | Quản lý user/device, claim/share, cấp quyền | Ngôn ngữ bất kỳ (vd FastAPI, Node, Go) |
| 📱 **App** | Giao diện người dùng | Web / Mobile |

> **Nguyên tắc vàng:** Thiết bị và App **không nói chuyện trực tiếp** — mọi thứ qua **Broker** (real-time) và **Backend** (quản lý quyền). Vì giao tiếp chỉ qua **giao thức chuẩn (MQTT/HTTP)**, hệ thống **không phụ thuộc loại chip** dùng ở thiết bị.

---

## 2. Tại sao MQTTS?

| Lý do | Giải thích |
|---|---|
| **Real-time** | Giữ kết nối liên tục → bấm nút là thiết bị phản hồi tức thì |
| **Nhẹ** | Giao thức gọn, chạy được trên MCU RAM thấp |
| **Bảo mật (TLS)** | Mã hóa toàn bộ, chống nghe lén |
| **Vượt rào mạng** | Broker công khai trên internet → thiết bị & server **không cần cùng LAN** |
| **Chuẩn mở** | Mọi nền tảng đều có thư viện MQTT client → portable |

Cổng: `8883` = MQTTS (TLS, production) · `1883` = MQTT thường (chỉ test nội bộ).

---

## 3. HỢP ĐỒNG PHÍA THIẾT BỊ (Firmware Contract)

Đây là phần quan trọng nhất để **port sang chip khác**. Bất kỳ MCU nào, firmware chỉ cần đáp ứng **6 khả năng trừu tượng** sau — cách hiện thực tuỳ chip:

| # | Khả năng cần có | Mô tả (độc lập chip) |
|---|---|---|
| C1 | **ID phần cứng duy nhất** | Đọc 1 số định danh duy nhất của chip (MAC / serial / UID) để sinh `device_id` |
| C2 | **Bộ nhớ không mất khi tắt** | Lưu/đọc `device_id`, `device_secret`, `claim_code`, `claimed` (flash/EEPROM/KV-store) |
| C3 | **Sinh số ngẫu nhiên** | Tạo `device_secret` + `claim_code` (ưu tiên RNG phần cứng) |
| C4 | **Kết nối mạng** | WiFi / Ethernet / 4G — miễn ra được internet |
| C5 | **MQTT client + TLS** | Kết nối broker `:8883`, đăng nhập, subscribe/publish |
| C6 | **Cơ chế reset vật lý** | 1 nút/giắc để factory reset (xoá định danh) |

Nếu chip đáp ứng đủ C1–C6 thì **chạy được hệ thống này**, không cần quan tâm hãng chip.

### Pseudo-code firmware (áp dụng cho mọi chip)

```text
function on_boot():
    # ----- Định danh (C1, C2, C3) -----
    id     = storage.load("device_id")  or  ("DEV_" + hw_unique_id())
    secret = storage.load("secret")     or  random_bytes(16)
    code   = storage.load("claim_code") or  random_code(6)        # vd "MDE6CS"
    claimed= storage.load("claimed")    or  false
    storage.save_all(id, secret, code, claimed)

    show_provisioning_ui(id, code)        # QR / màn hình / web nội bộ

    # ----- Kết nối (C4, C5) -----
    network.connect()                     # WiFi/Eth/4G
    mqtt.connect_tls(host, 8883,
                     username = id,
                     password = secret)    # broker xác thực
    mqtt.subscribe("dev/" + id + "/cmd")
    mqtt.set_last_will("dev/"+id+"/online", "offline")  # LWT

function on_mqtt_message(topic, payload):
    cmd = json_parse(payload)
    execute(cmd)                          # motor / LED / relay ...

function loop():
    every 5s:  mqtt.publish("dev/"+id+"/status", json(read_status()))
    if mqtt_disconnected_for > T:  safe_stop()    # watchdog an toàn

function on_reset_button_held(3s):        # C6
    storage.erase_all()                   # device_id giữ nguyên (từ hw id),
    reboot()                              # secret/claim_code sinh mới
```

> Lưu ý: phần `execute(cmd)`, `read_status()`, `safe_stop()` là **chỗ đấu nối phần cứng cụ thể** (motor, cảm biến...) — phần duy nhất khác nhau giữa các thiết bị/chip.

---

## 4. Định danh thiết bị (Device Identity)

| Thông tin | Sinh từ (C1/C3) | Công khai? | Vai trò |
|---|---|---|---|
| `device_id` | ID phần cứng duy nhất (MAC/serial/UID) | ✅ Có | "Biển số" — định danh thiết bị |
| `device_secret` | 16 byte ngẫu nhiên | ❌ Tuyệt mật | "Mật khẩu" đăng nhập MQTT, chứng minh thật |
| `claim_code` | 6 ký tự ngẫu nhiên | ✅ Hiện cho chủ | Mã kích hoạt để nhận máy |

- `device_id` **không đổi** (gắn với phần cứng) → dù reset vẫn là thiết bị đó.
- `device_secret` **không bao giờ gửi ra ngoài** ngoài việc làm mật khẩu MQTT (qua TLS).
- `claim_code` + `device_id` thường encode vào **QR** để App quét.
- **Factory reset (C6):** xoá vùng lưu → `claim_code`/`secret` mới, `device_id` giữ nguyên.

### Vòng đời `claim_code` (như OTP — ngắn hạn, dùng 1 lần)
- ⏱️ **Có hạn (TTL):** mỗi `claim_code` chỉ sống 1 khoảng (mẫu: **10 phút**). Quá hạn → phải làm mới.
- 🔄 **Làm mới (refresh):** sinh mã + hạn mới khi chưa có chủ — thiết bị tự làm (re-register) hoặc gọi `POST /devices/{id}/refresh-claim-code`. Mã **cũ vô hiệu ngay**.
- 1️⃣ **Dùng 1 lần:** claim thành công → server **xoá trắng** `claim_code` (không thể dùng lại).

---

## 5. Luồng CLAIM (nhận quyền sở hữu)

```
 📱 App                  ⚙️ Backend              🤖 Thiết bị
   │ 1. Quét QR (đọc        │                       │
   │    device_id+claim_code)│                       │
   │ 2. POST /devices/claim  │                       │
   │    {device_id,code}+JWT ├──► 3. Kiểm tra:        │
   │                         │   • owner = NULL?      │
   │                         │   • claim_code đúng?   │
   │                         │   → owner = user       │
   │                         │   → device_users(owner)│
   │◄─── 4. claimed ✅ ───────┤                       │
   │                         │ 5. báo qua MQTT ──────►│ claimed=1
```

Quy tắc: chỉ claim khi `owner = NULL`; `claim_code` nên **dùng 1 lần + có hạn**.

---

## 6. Luồng SHARE (chia sẻ thiết bị — có hạn & thu hồi)

```
 Owner ──POST /devices/{id}/share {hours}──► Backend tạo invite_code (hạn = now+hours)
        (có thể lấy QR mã mời: GET /invites/{code}/qr)
 Owner ──gửi mã/QR──► Người khác ──POST /invites/accept──► Backend
        → device_users(role=member, expire_at = hạn mã) + đánh dấu mã đã dùng

 Owner ──POST /invites/{code}/revoke──► Backend
        → mã.revoked = true  +  XOÁ quyền đã cấp qua mã → member MẤT QUYỀN ngay
```

**Quyền điều khiển có 2 cách hết hiệu lực:**
- ⏰ **Hết hạn:** `device_users.expire_at < now` → tự mất quyền.
- 🚫 **Owner thu hồi:** revoke mã mời → xoá quyền tức thì.

| Role | Quyền | Hạn |
|---|---|---|
| `owner` | Điều khiển + tạo/hủy mã mời + xoá/chuyển thiết bị | Vĩnh viễn (`expire_at = NULL`) |
| `member` | Chỉ điều khiển | Có hạn (`expire_at`), bị thu hồi được |

---

## 7. PHƯƠNG PHÁP ĐIỀU KHIỂN THIẾT BỊ (qua MQTT)

### 7.1 Quy ước Topic (theo `device_id`, độc lập chip)

| Topic | Hướng | Publisher | Subscriber | Nội dung |
|---|---|---|---|---|
| `dev/<device_id>/cmd` | App → Thiết bị | App/Backend | **Thiết bị** | Lệnh điều khiển |
| `dev/<device_id>/status` | Thiết bị → App | **Thiết bị** | App/Backend | Trạng thái/telemetry |
| `dev/<device_id>/online` | Thiết bị → All | Thiết bị (LWT) | App/Backend | online/offline |

### 7.2 Định dạng tin nhắn (JSON — chuẩn chung, không phụ thuộc chip)

Lệnh (App → `.../cmd`):
```json
{ "action": "move", "dir": "forward", "speed": 150 }
{ "action": "stop" }
{ "action": "set", "pin": "relay1", "value": 1 }
```
Trạng thái (Thiết bị → `.../status`):
```json
{ "battery": 85, "moving": true, "rssi": -62, "ts": 1782900000 }
```

> Khuyến nghị: dùng **JSON + trường `action`** để mở rộng dễ. Thiết bị chỉ cần hiểu các `action` nó hỗ trợ, bỏ qua phần còn lại → **tương thích ngược** khi App thêm lệnh mới.

### 7.3 Luồng điều khiển real-time

Có **2 cách** App gửi lệnh — chọn theo nhu cầu:

**Cách A — App publish thẳng lên Broker** (real-time nhất, cần ACL ở broker):
```
 📱 App ─publish dev/<id>/cmd {"action":"move"}─► 📡 Broker ─deliver─► 🤖 Thiết bị
 📱 App ◄────────── deliver ◄── 📡 Broker ◄──publish dev/<id>/status──── 🤖 Thiết bị
```

**Cách B — App gọi Backend, Backend publish hộ** (Backend kiểm tra quyền bằng DB):
```
 📱 App ─POST /devices/<id>/control {command}─► ⚙️ Backend
         (kiểm: user là owner? hoặc member chưa hết hạn?)  ── can_control() ──┐
                                                                              ▼
                                          ⚙️ Backend ─publish dev/<id>/cmd─► 📡 Broker ─► 🤖 Thiết bị
```
- Ưu điểm cách B: **quyền kiểm tập trung ở Backend (DB)** → đơn giản, không cần cấu hình ACL phức tạp ở broker; Backend dùng creds riêng để vào broker.
- Bản hiện thực mẫu dùng **cách B** (endpoint `/devices/{id}/control`, publish qua MQTTS).

Phía firmware (mọi chip): `subscribe(cmd)` → `on_message` parse JSON → `execute()` → `publish(status)`.

### 7.4 Phân quyền điều khiển (ACL — broker enforce)

| Chủ thể | Được phép |
|---|---|
| 🤖 Thiết bị `X` | Sub `dev/X/cmd`, Pub `dev/X/status` (chỉ topic của nó) |
| 📱 User là owner/member của `X` | Pub `dev/X/cmd`, Sub `dev/X/status` |
| ❌ Người lạ | Không pub/sub được |

Broker kiểm ACL qua bảng `device_users` (cùng nguồn với backend claim/share) → thiết bị A không điều khiển/nghe lén được thiết bị B.

### 7.5 An toàn khi mất kết nối
- **LWT:** broker tự đẩy "offline" khi thiết bị rớt đột ngột.
- **Watchdog:** mất MQTT/mạng quá ngưỡng → thiết bị tự về trạng thái an toàn (`safe_stop`).

---

## 8. Xác thực MQTT (Authentication)

| Client | Username | Password | Broker kiểm với |
|---|---|---|---|
| 🤖 Thiết bị | `device_id` | `device_secret` / token | bảng `devices` |
| 📱 App/User | user id / token | JWT / token | `users` + `device_users` |

- Sai mật khẩu → broker trả **CONNACK code 5 "not authorized"**.
- Mọi thứ trên nền **TLS** → username/password được mã hóa.

---

## 9. Cơ sở dữ liệu (4 bảng — phía server, độc lập thiết bị)

```sql
users         (id, email, password_hash, created_at)
devices       (id, device_id, device_secret, claim_code, owner_id, claimed, created_at)
device_users  (device_id, user_id, role)            -- owner / member → ACL
share_invites (invite_code, device_id, created_by, expire_at, used)
```
Backend ghi/sửa khi claim/share; Broker đọc để xác thực + ACL.

---

## 10. API Backend (tóm tắt)

| Method | Endpoint | Mô tả |
|---|---|---|
| POST | `/auth/register` `/auth/login` | Đăng ký / đăng nhập → token |
| POST | `/devices/register` | Thiết bị báo danh tính |
| POST | `/devices/claim` | User claim thiết bị |
| GET | `/devices` | Thiết bị của user (kèm role, hạn) |
| POST | `/devices/{id}/refresh-claim-code` | **Làm mới** claim_code (TTL mới) khi chưa claim |
| GET | `/devices/{id}/claim-qr` | **QR claim** (device_id + claim_code) → ảnh SVG |
| POST | `/devices/{id}/control` | **Điều khiển** (kiểm quyền → publish MQTTS) |
| POST | `/devices/{id}/share` | Tạo mã mời (body `{hours}`) |
| GET | `/invites/{code}/qr` | **QR mã mời** → ảnh SVG |
| POST | `/invites/accept` | Nhập mã mời → member (có hạn) |
| GET | `/devices/{id}/invites` | Owner xem các mã mời |
| POST | `/invites/{code}/revoke` | **Hủy mã mời** → thu hồi quyền |
| GET | `/devices/{id}/members` | Danh sách người dùng (kèm hạn) |
| GET | `/mqtt/health` | Kiểm tra backend nối được broker MQTTS |

---

## 11. ÁNH XẠ SANG TỪNG NỀN TẢNG CHIP

Cùng 1 "hợp đồng" (Mục 3), mỗi chip hiện thực bằng API riêng:

| Khả năng | ESP32 (ESP-IDF) | ESP32/ESP8266 (Arduino) | STM32 + RTOS | RPi Pico W |
|---|---|---|---|---|
| **C1** ID phần cứng | `esp_read_mac()` | `WiFi.macAddress()` | UID 96-bit (`0x1FFF7590`) | `pico_get_unique_board_id()` |
| **C2** Lưu non-volatile | NVS | `Preferences`/EEPROM | Flash/EEPROM-emul | Flash (lib) |
| **C3** Random | `esp_random()` | `ESP.random()`/RNG | RNG peripheral | `get_rand_32()` |
| **C4** Mạng | `esp_wifi` | `WiFi.h` | LwIP / module 4G | `cyw43`/lwIP |
| **C5** MQTT + TLS | `esp-mqtt` + mbedTLS | `PubSubClient`+`WiFiClientSecure` | Paho embedded + mbedTLS | `lwip` MQTT + mbedTLS |
| **C6** Nút reset | GPIO + giữ nút | `digitalRead()` | HAL GPIO | GPIO |
| **JSON** | cJSON | ArduinoJson | cJSON / jsmn | cJSON |

> Chỉ **6 dòng API** này thay đổi giữa các chip. Toàn bộ logic claim/share/điều khiển (topic, JSON, luồng) **giữ nguyên** vì là chuẩn MQTT.

---

## 12. Vòng đời đầy đủ (end-to-end)

```
1. Thiết bị boot → sinh device_id + claim_code + secret → hiện QR
2. Thiết bị register lên server (device_id, claim_code)
3. User đăng nhập App → quét QR → claim → owner
4. Owner share → người khác nhập mã → member
5. Thiết bị kết nối Broker MQTTS (TLS) bằng device_id + secret
6. App kết nối Broker bằng tài khoản user
7. App publish lệnh → dev/<id>/cmd → Thiết bị thực thi
8. Thiết bị publish trạng thái → dev/<id>/status → App hiển thị
9. ACL đảm bảo chỉ owner/member mới điều khiển được
```

---

## 13. Triển khai tham chiếu (Reference Implementation)

Dự án `clam-devices` này là **một bản hiện thực mẫu** của kiến trúc trên:

| Phần | Bản mẫu dùng | Ghi chú |
|---|---|---|
| Thiết bị | **ESP32-C3 (ESP-IDF)** | Chỉ là ví dụ C1–C6; chip khác thay theo Mục 11 |
| Broker | EMQX (local) / MQTTS công khai (`mqtt.runagent.io:8883`) | TLS cổng 8883 |
| Backend | FastAPI + PostgreSQL + MQTTS client (stdlib) | Ngôn ngữ khác triển khai cùng API Mục 10 |
| App | Web tĩnh | — |

**Trạng thái:**
- ✅ Firmware: identity / QR / factory reset / WiFi provisioning + quét WiFi
- ✅ Backend: auth (JWT), claim, **share có hạn + thu hồi (revoke)**, **QR (claim & mã mời)**
- ✅ Backend: **điều khiển qua MQTTS** (`/devices/{id}/control` → publish lệnh, kiểm quyền bằng DB)
- ✅ App web: đăng nhập / claim / share
- 🔶 Broker MQTTS: kết nối + TLS OK với `mqtt.runagent.io`, **chờ username/password đúng** (hiện trả code 5)
- ⬜ Nối firmware thật ↔ broker (cần creds đúng + thiết bị online cùng broker)

---

*Tài liệu kiến trúc tham chiếu — độc lập phần cứng. Bản hiện thực mẫu: ESP32-C3.*
