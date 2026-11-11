# Chẩn đoán Ra-02/SX1278 trên bo STM32F103C8

## Kết luận theo thông tin hiện có

Bạn dùng **bo mạch thật**, PCB thiết kế bằng Proteus, module **Ra-02/SX1278 433 MHz**, LCD báo **RF:ERR**. Tần số là 433 MHz, không phải 433 Hz.

Trong firmware gốc, `RF:ERR` nghĩa là ứng dụng không khởi tạo được radio hoặc radio đã rơi vào trạng thái lỗi; nó không có nghĩa đơn thuần là không nhận được ACK. Ngoài ra, nếu `BootCounter_Next()` không ghi được journal Flash, chương trình bỏ qua khởi tạo radio và cũng báo RF:ERR. Cần phân biệt hai nhánh này trước khi thử thay SF, tăng công suất hoặc tăng timeout.

**Chưa thể kết luận dây nào sai hoặc module hỏng khi chưa có sơ đồ/netlist PCB và kết quả đọc thanh ghi trên bo.** Kiểm tra mã nguồn xác nhận hai phía có cùng thông số RF và cùng giao thức. Các lỗi ACK/timeout đã tái hiện được là lỗi ổn định phần mềm, chưa chứng minh là nguyên nhân trực tiếp của RF:ERR trên bo của bạn.

## Đối chiếu dây PCB trước tiên

Các net thực tế phải khớp bảng này, theo `main.h`, `MX_SPI1_Init()` và `HAL_SPI_MspInit()` trong ZIP gốc:

| Chân Ra-02 | STM32 Master | STM32 slave |
|---|---|---|
| SCK | PA5 — SPI1_SCK | PA5 — SPI1_SCK |
| MISO | PA6 — SPI1_MISO | PA6 — SPI1_MISO |
| MOSI | PA7 — SPI1_MOSI | PA7 — SPI1_MOSI |
| NSS/CS | PB12 | PB0 |
| RESET | PB13 | PB1 |
| DIO0 | PB14 | PB10 |
| VCC | 3,3 V | 3,3 V |
| GND | chung mass với STM32 | chung mass với STM32 |

**Đặc biệt:** code dùng **SPI1**, không dùng SPI2. Nếu PCB nối SCK/MISO/MOSI sang PB13/PB14/PB15 theo SPI2 thì không khớp firmware này. Trên Master, PB13 đang là RESET và PB14 đang là DIO0. Kiểm tra cả symbol → số pad → footprint và đo thông mạch khi tắt nguồn; tên net trên sơ đồ chưa chứng minh pad thực tế đúng.

Ra-02 dùng nguồn và logic 3,3 V. Đo VCC ngay trên module trong lúc reset/phát, dùng nguồn đủ dòng với tụ decoupling sát module theo thiết kế nhà sản xuất. Không cấp 5 V vào module. Kiểm tra mối hàn, chiều module, GND, RESET không bị giữ thấp; dùng anten 433 MHz phù hợp. Lỗi anten/khoảng cách không giải thích được việc đọc sai `RegVersion` qua SPI.

## Đọc nguyên nhân trực tiếp trên LCD bản chẩn đoán

Bản sửa giữ nguyên cấu hình RF và pinout. Khi radio lỗi, Master hiển thị ví dụ `E3 V00 R42 12/00`; slave hiển thị `N1 E3 V00 R42` và dòng `REG 42 12/00`.

- `E`: mã lỗi ở bảng dưới.
- `V`: giá trị gần nhất đọc từ `RegVersion` (địa chỉ `0x42`); SX1278 dự kiến `0x12`.
- `R`: địa chỉ thanh ghi liên quan.
- `12/00`: expected/actual, đều ở hệ hex.

Với E1/E2, giao dịch SPI thất bại nên actual=00 chỉ là giá trị giữ chỗ, không phải kết quả đọc thành công. So sánh expected/actual có ý nghĩa đọc lại với E3/E4/E6; không suy ra MISO bị kéo thấp chỉ từ actual=00 của E1/E2.

| Hiển thị | Ý nghĩa và bước tiếp theo |
|---|---|
| `BOOT:ERR` | Ghi journal Flash thất bại, chưa gọi LoRa_Init. Kiểm tra write protection, mã `masterBootFlashError`/`slaveBootFlashError`, chip STM32 thực tế. Không bỏ journal bằng cách luôn dùng session=1. |
| `E1` / `E2` | HAL SPI đọc/ghi thất bại. Kiểm tra clock, SPI1, chân nối, nguồn/reset và NSS. |
| `E3 V00` / `E3 VFF` | Giao dịch SPI hoàn tất nhưng không đọc được phiên bản dự kiến. Kiểm tra MISO, NSS, MOSI/SCK, GND, VCC, reset và loại chip. 00/FF không đủ để kết luận module hỏng. |
| `E3` với V khác | Không phải phiên bản SX1278 dự kiến hoặc dữ liệu SPI lỗi. Đối chiếu module và đo SPI. |
| `E4` | Ghi cấu hình rồi đọc lại khác. `R` và expected/actual chỉ điểm thất bại; đo SPI/nguồn trước. |
| `E5` | Sau 500 ms vẫn không có TxDone; kiểm tra radio/nguồn/clock. |
| `E6` | Kiểm tra định kỳ thấy mode/cấu hình bị đổi. Kiểm tra sụt nguồn/reset radio và truy cập SPI khác. |
| `CHIP:OK`, node vẫn `OFF` | Khởi tạo chip đã thành công. Chuyển sang kiểm tra ID, anten, RF và ACK. |

Đây là thông tin chẩn đoán, không phải tự động sửa lỗi PCB. Khi đã nạp, ghi lại nguyên dòng lỗi trên **cả Master và slave** để xác định nhánh tiếp theo. Với ST-Link, cũng có thể xem struct `loraDiagnostics` và các bộ đếm lỗi trong debugger. `lastError` được xóa ở lần thử khởi tạo tiếp theo; bộ đếm giữ qua các lần phục hồi radio và reset khi MCU khởi động lại.

## Các lỗi và rủi ro đã kiểm tra trong chương trình

1. **Slave chỉ có BIN mặc định node 1.** `app_config.h` mặc định NODE_ID=1; disassembly BIN/ELF đi kèm xác nhận điều này. Nếu nạp cùng BIN cho nhiều slave, tất cả trả lời node 1, gây va chạm, không có node 2/3. Bản build mới tạo ba ảnh riêng. Với chỉ một slave node 1, đây không phải nguyên nhân của RF:ERR.
2. **ACK có thể kẹt do buffer RX.** Slave cũ không lấy packet khi ACK pending, trong khi driver từ chối TX nếu buffer còn packet. Gói mới đến giữa lúc chuẩn bị ACK làm ACK phải chờ hết hạn. Bản sửa luôn lấy tối đa một packet/vòng, có một ô chờ request; request bị tràn được loại trước khi tác động relay.
3. **Timeout được kiểm tra trước TxDone.** Nếu vòng lặp trở lại sau 500 ms dù TxDone đã bật, driver cũ báo timeout và hủy radio. Kiểm thử đã tái hiện ở 501 ms. Bản sửa đọc cờ phần cứng trước, vẫn báo timeout nếu thực sự chưa có TxDone. Việc này không đảm bảo nhận được ACK đã trôi qua trong khoảng MCU bận; vẫn phải giữ vòng lặp phục vụ radio nhanh.
4. **Thông báo RF:OK gây nhầm lẫn.** Đó chỉ là khởi tạo chip thành công. Bản sửa dùng CHIP:OK, tách BOOT:ERR và hiển thị RX/TX trên slave; Master vẫn dùng ONL/OFF theo phản hồi hợp lệ. TX đếm TxDone cục bộ, không chứng minh phía bên kia nhận được.

Những phần đang khớp và được giữ lại: 433 MHz (`FRF=0x6C4000`), SF7, BW125 kHz, CR4/5, explicit header, CRC bật, preamble 8, sync word `0x12`, frame 34 byte/version 1/CRC16. Master điều phối từng giao dịch, POLL trước SET, thử SET lại với cùng sequence để chống thực thi trùng. Slave dùng polling, nên thiếu ngắt DIO0 riêng ở slave không tự nó giải thích mất liên lạc.

Payload 34 byte ở cấu hình này có airtime khoảng 77,1 ms. Thời gian chờ phản hồi hiện tại 250/350 ms được tính sau TxDone; chưa có căn cứ quy lỗi hoàn toàn không liên lạc cho timeout quá ngắn. Nếu sau này thay SF/BW, phải tính lại airtime, TX timeout và cửa sổ ACK đồng bộ ở cả hai đầu.

## Build, nạp và xác minh

ZIP gốc được giữ nguyên. Các file sửa có thể review trong `firmware-overrides/`. Công cụ giải nén vào thư mục nguồn mới trong `build/`, chồng các file sửa, bỏ Debug objects cũ rồi build đủ bốn target:

```bash
python tests/run_radio_tests.py
python tests/run_slave_queue_tests.py
python tools/build_firmware.py
```

Cần Python 3, native GCC cho test, ARM GNU GCC/binutils và newlib-nano cho build. Trong môi trường hiện tại script tìm bộ ARM GCC 14.2.1 đã cài ở `/workspace/firmware-dev/toolchain/usr/bin`; máy khác có thể truyền `--toolchain /path/to/bin`. Build giữ GNU11/cờ CPU từ dự án, thêm `-include sys/types.h` riêng cho sysmem.c để dùng newlib mới. Sửa nguồn trong `firmware-overrides/`; thư mục `build/sources-*` là đầu ra tạo tự động.

| Bo cần nạp | HEX/BIN/ELF trong thư mục |
|---|---|
| Master | `build/Master/` |
| Slave 1 | `build/relay32-node1/` |
| Slave 2 | `build/relay32-node2/` |
| Slave 3 | `build/relay32-node3/` |

Nạp HEX bằng công cụ STM32 phù hợp; BIN dùng địa chỉ bắt đầu `0x08000000`. Journal nằm tại `0x0800F800` và `0x0800FC00`, linker giữ 62 KiB đầu cho ứng dụng. Tránh mass erase làm mất journal khi cập nhật. Nếu đã xóa journal Master, nên khởi động lại các slave trong lần thử trên bàn để tránh session/sequence cũ; một lần nạp mới không nên được thực hiện khi relay đang điều khiển tải cần giữ trạng thái. Chưa tự động nạp gì lên bo.

Mỗi thư mục target có `*-source.zip` chứa dự án đã áp dụng sửa và ID tương ứng để kiểm tra/import CubeIDE. Ba dự án slave có thể giữ tên Eclipse nội bộ relay32 như bản gốc; import từng dự án hoặc đổi tên khi import. Với CubeIDE dùng newlib mới, giữ cờ tương thích sysmem.c ở trên. Không dùng lại file `Debug/relay32.bin` trong ZIP gốc cho cả ba node.

Trình tự kiểm tra bo: bắt đầu một Master + một slave node 1 ở khoảng cách vài mét với anten phù hợp; kiểm tra CHIP:OK trước, rồi RX/TX tăng và N1 ONL. Sau đó thêm node 2/3 với đúng ảnh firmware. Ghi số giao dịch và phản hồi hợp lệ để tính tỉ lệ mất gói; thử liên tục, khởi động lại từng bo và đóng/cắt tải relay để quan sát nhiễu nguồn. Không tăng công suất hay SF để che lỗi khởi tạo SPI. Chỉ đổi RF sau khi đã đọc/ghi chip ổn định, có thống kê RSSI/SNR và tuân thủ băng tần/công suất được phép tại nơi sử dụng.

## Bằng chứng xác minh trên máy phát triển

- Driver cũ: 15 case đạt, một lỗi delayed-TxDone tái hiện (không phải 16 case đạt).
- Driver sửa: 16/16 case HAL mock đạt, gồm SPI/version/readback, RX/CRC, TX→RX, timeout thật và tick wrap.
- Hàng đợi slave: 5 kịch bản hồi quy đạt với AddressSanitizer/UndefinedBehaviorSanitizer, gồm packet tới khi ACK pending, SET trùng, tràn hàng đợi và boot sai.
- Master + ba slave ID 1/2/3 đã biên dịch và liên kết thành công bằng ARM GCC 14.2.1; linker giữ giới hạn Flash/RAM.
- Các kiểm thử trên xác minh luồng phần mềm. Chưa có bo nối vào môi trường này, chưa đo SPI/VCC và chưa chứng minh truyền RF hoặc độ ổn định ngoài thực tế.
