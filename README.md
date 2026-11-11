# LoRa STM32F103C8 — Master và ba slave

Gói dành cho STM32CubeIDE: [LoRa-STM32CubeIDE.zip](downloads/LoRa-STM32CubeIDE.zip).
Hướng dẫn đầy đủ: [CUBEIDE_README.md](CUBEIDE_README.md).

Giải nén gói, vào **File → Import → General → Existing Projects into Workspace**,
chọn thư mục `Projects`, rồi import cả bốn dự án: `LoRa_Master`, `LoRa_Node1`,
`LoRa_Node2`, `LoRa_Node3`. Chọn đúng dự án cho từng bo, Build, sau đó Debug qua
ST-LINK/SWD. Cấu hình Debug đi kèm dùng Connect under reset và cần nối NRST.

Đã tích hợp sửa lỗi ACK/TxDone, chẩn đoán LCD, ID node riêng và include sysmem
cho newlib mới. Bộ tạo gói kiểm tra cấu hình XML và build Debug/Release của cả
bốn dự án bằng ARM GCC. Chưa thử GUI CubeIDE, ST-LINK hay truyền RF trên bo thật.

[Báo cáo LoRa và bảng pin](LORA_DIAGNOSIS.md) giải thích `RF:ERR`, mã lỗi LCD và
những phép đo cần làm trên PCB. Các file ZIP gốc được giữ nguyên.

Chạy kiểm tra và tạo lại gói:

```bash
python tests/run_radio_tests.py
python tests/run_slave_queue_tests.py
python -m unittest discover -s tests -p 'test_cubeide_bundle.py'
python tools/build_cubeide_bundle.py
```
