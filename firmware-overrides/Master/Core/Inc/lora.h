#ifndef DOAN2_LORA_H
#define DOAN2_LORA_H
#include "main.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define LORA_MAX_PACKET_LENGTH 128U
/* SX1278/RA-02, 433 MHz, SF7, BW125kHz, CR4/5, explicit header + CRC. */
uint8_t LoRa_Init(void);
void LoRa_Process(void);
uint8_t LoRa_IsReady(void);
uint8_t LoRa_IsBusy(void);
/* 1 = accepted; 0 = not accepted (RX pending/receiving, busy, or not ready).
 * A rejected call must not be counted as a transmitted command. */
uint8_t LoRa_SendAsync(const uint8_t *data, uint8_t length);
/* 0 pending, +1 TxDone, -1 SPI fault/TX timeout. Not an application ACK. */
int8_t LoRa_TxResult(void);
/* Returns complete frame only. Over-capacity frames are dropped, never cut. */
uint8_t LoRa_ReadPacket(uint8_t *buffer, uint8_t capacity);
int16_t LoRa_GetLastRSSI(void);
/* Signed raw SX1278 SNR: divide by 4.0 to obtain dB. */
int8_t LoRa_GetLastSNR(void);
/* ISR-safe: records an event only, no SPI. Polling also works without EXTI. */
void LoRa_OnDio0Interrupt(void);

/* Debugger-visible: lastError is cleared by the next initialization attempt;
 * traffic and initialization counters survive radio recovery. */
typedef enum {
    LORA_INIT_NONE = 0, LORA_INIT_RESET, LORA_INIT_VERSION,
    LORA_INIT_SLEEP, LORA_INIT_CONFIG, LORA_INIT_RX, LORA_INIT_READY
} LoRaInitStage;
typedef enum {
    LORA_ERROR_NONE = 0, LORA_ERROR_SPI_READ, LORA_ERROR_SPI_WRITE,
    LORA_ERROR_VERSION, LORA_ERROR_READBACK, LORA_ERROR_TX_TIMEOUT,
    LORA_ERROR_HEALTH
} LoRaError;
typedef struct {
    uint32_t initAttempts, initSuccesses;
    uint32_t txAccepted, txDone, rxAccepted;
    uint8_t initStage, lastError, lastVersion;
    uint8_t failedRegister, expectedValue, actualValue;
} LoRaDiagnostics;
extern volatile LoRaDiagnostics loraDiagnostics;

extern volatile uint32_t loraSpiErrorCount;
extern volatile uint32_t loraCrcErrorCount;
extern volatile uint32_t loraRxDropCount;
extern volatile uint32_t loraTxTimeoutCount;
#ifdef __cplusplus
}
#endif
#endif
