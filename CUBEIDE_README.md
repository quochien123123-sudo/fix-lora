# Dự án LoRa hoàn chỉnh cho STM32CubeIDE

Gói `LoRa-STM32CubeIDE.zip` gồm bốn dự án riêng, đầy đủ Core, Drivers, startup,
linker và cấu hình Debug/Release. Các sửa lỗi LoRa và chẩn đoán trên LCD đã được
tích hợp; không cần tự chép file C hay thêm include. Chip đích: STM32F103C8Tx,
module Ra-02/SX1278 433 MHz. Pinout giữ nguyên như bảng trong LORA_DIAGNOSIS.md.

| Dự án trong Projects | Nạp cho bo | ID |
|---|---|---|
| LoRa_Master | Master | Bộ điều phối |
| LoRa_Node1 | Slave 1 | 1 |
| LoRa_Node2 | Slave 2 | 2 |
| LoRa_Node3 | Slave 3 | 3 |

## Import cả bốn dự án

1. Giải nén ZIP vào thư mục mới trên máy, ví dụ `C:\STM32\LoRa`.
2. Mở STM32CubeIDE và chọn một workspace. Nếu workspace cũ có dự án trùng tên,
   dùng workspace mới để giữ nguyên công việc cũ.
3. Chọn **File → Import → General → Existing Projects into Workspace → Next**.
4. Chọn **Select root directory → Browse**, trỏ tới thư mục `Projects` trong gói.
5. Bật **Search for nested projects** nếu phiên bản IDE có tùy chọn đó. Chọn cả bốn
   dự án LoRa_Master, LoRa_Node1, LoRa_Node2, LoRa_Node3, rồi **Finish**.
6. Nếu IDE hỏi công cụ biên dịch, chọn **GNU Tools for STM32** đi kèm bản CubeIDE
   đang cài. Gói đã bỏ khóa phiên bản GCC cũ 7-2018-q2-update.

Thao tác này thực hiện trên máy tính của bạn. Môi trường đám mây không điều khiển
được STM32CubeIDE hay ST-LINK trên máy đó.

## Build và nạp từ IDE

1. Nhấp phải dự án đúng bo → **Build Configurations → Set Active → Debug**.
2. Chọn **Project → Build Project**, đợi báo **0 errors**. Các file sinh ra là:
   `Debug/LoRa_Master.elf`, `.hex`, `.bin` (node dùng tên LoRa_Node1/2/3 tương ứng).
3. Nối ST-LINK và cấp nguồn phù hợp cho bo:

   | ST-LINK | STM32F103C8 |
   |---|---|
   | SWDIO | PA13 |
   | SWCLK | PA14 |
   | GND | GND |
   | NRST | NRST |

   Cấu hình Debug đi kèm dùng **Connect under reset**, vì vậy **cần dây NRST**.
   Nối VREF với 3,3 V của bo nếu probe có chân VREF. Không cấp 5 V cho Ra-02.
4. Nhấp phải dự án → **Debug As → STM32 C/C++ Application**. Nếu IDE hỏi cấu hình,
   chọn `<tên dự án> Debug`. Cũng có thể chọn trong **Run → Debug Configurations**.
5. Trong tab Main, ELF phải là `Debug/<tên dự án>.elf`; tab Debugger dùng ST-LINK,
   giao tiếp SWD. Các đường dẫn đã đặt theo từng dự án, không trỏ tới máy cũ.
6. Bấm **Debug** để build/nạp. Khi dừng ở `main`, bấm **Resume (F8)** để chạy.
7. Kết thúc Debug rồi nhấn RESET để kiểm tra chạy độc lập. Trên bo hiện tại từng
   báo RF:ERR, ghi lại mã LCD như `E3 V00 R42`, `BOOT:ERR` hoặc `CHIP:OK`.

Chỉ nạp dự án tương ứng cho mỗi slave; không nạp LoRa_Node1 cho cả ba bo. Các ID
đã được đặt đồng nhất trong header và cả hai cấu hình Debug/Release.

## File HEX có sẵn

Thư mục `Prebuilt/<tên dự án>/` có HEX/BIN từ bản Debug đã build trên máy đám mây.
Có thể nạp HEX bằng STM32CubeProgrammer mà chưa cần build trong IDE. BIN bắt đầu
tại `0x08000000`. Journal Flash nằm tại `0x0800F800` và `0x0800FC00`; giữ chế độ
xóa các trang cần thiết, tránh mass erase khi cập nhật. Nếu đã xóa journal Master,
khởi động lại các slave trong lần thử trên bàn để tránh lịch sử session cũ.

## Chỉnh sửa và cấu hình chân

Các dự án là dự án C quản lý build bằng CubeIDE, đã tích hợp ứng dụng tùy biến.
Các file `.ioc` cũ nằm trong `Reference` để đối chiếu sơ đồ; chúng không phải đầu
vào đã đồng bộ để sinh lại ứng dụng. Không dùng **Generate Code** từ các file đó:
main.c hiện có nhiều logic bên ngoài vùng USER CODE và sẽ cần được chuyển sang
cấu trúc bảo toàn trước khi sử dụng quy trình CubeMX sinh mã. Import/build/debug
các dự án hiện tại không cần mở `.ioc`.

Nếu build báo không tìm thấy ARM GCC, kiểm tra **Project Properties → C/C++ Build
→ Settings/Tool Chain Editor** và chọn bộ GNU Tools for STM32 đã cài với IDE.
Nếu ST-LINK không kết nối được, kiểm tra nguồn, GND, PA13/PA14 và NRST trước.

## Phạm vi đã xác minh

- Kiểm tra cấu trúc XML, đường dẫn, ID dự án/config/launch và ID node.
- Build đủ **8 cấu hình**: Debug và Release của cả bốn dự án, dùng ARM GCC và
  các include/define/chuẩn C lấy từ chính `.cproject`. Không dùng cờ ép include
  để che lỗi sysmem.c; `sys/types.h` đã được thêm trực tiếp vào nguồn.
- Kiểm thử driver LoRa và hàng đợi ACK vẫn được giữ trong repository.
- `VALIDATION.json` ghi phiên bản GCC và kết quả build trong gói.

Máy đám mây không có STM32CubeIDE và không có bo/ST-LINK, nên chưa chạy thử thao
tác import, debugger trong GUI hay truyền RF thực tế. Các kiểm tra trên xác minh
cấu trúc dự án và build bằng toolchain, không thay thế thử nghiệm trên bo.

Tái tạo gói trên môi trường phát triển:

```bash
python tools/build_cubeide_bundle.py
python -m unittest discover -s tests -p 'test_cubeide_bundle.py'
```
