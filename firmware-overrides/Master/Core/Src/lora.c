#include "lora.h"
#include <string.h>

extern SPI_HandleTypeDef hspi1;

#define REG_FIFO             0x00U
#define REG_MODE             0x01U
#define REG_FRF_MSB          0x06U
#define REG_FRF_MID          0x07U
#define REG_FRF_LSB          0x08U
#define REG_PA_CONFIG        0x09U
#define REG_LNA              0x0CU
#define REG_FIFO_PTR         0x0DU
#define REG_TX_BASE          0x0EU
#define REG_RX_BASE          0x0FU
#define REG_RX_CURRENT       0x10U
#define REG_IRQ_MASK         0x11U
#define REG_IRQ              0x12U
#define REG_RX_BYTES         0x13U
#define REG_MODEM_STATUS     0x18U
#define REG_SNR              0x19U
#define REG_RSSI             0x1AU
#define REG_HOP_CHANNEL      0x1CU
#define REG_CONFIG1          0x1DU
#define REG_CONFIG2          0x1EU
#define REG_SYMBOL_TIMEOUT   0x1FU
#define REG_PREAMBLE_MSB     0x20U
#define REG_PREAMBLE_LSB     0x21U
#define REG_PAYLOAD_LENGTH   0x22U
#define REG_MAX_LENGTH       0x23U
#define REG_CONFIG3          0x26U
#define REG_SYNC             0x39U
#define REG_DIO_MAPPING      0x40U
#define REG_VERSION          0x42U
#define REG_PA_DAC           0x4DU
#define IRQ_RX_DONE          0x40U
#define IRQ_CRC_ERROR        0x20U
#define IRQ_TX_DONE          0x08U
#define MODE_SLEEP           0x88U
#define MODE_STANDBY         0x89U
#define MODE_TX              0x8BU
#define MODE_RX              0x8DU
#define SPI_TIMEOUT_MS       5U
#define TX_TIMEOUT_MS        500U
#define RX_POLL_MS           2U

static volatile uint8_t dioEvent;
static uint8_t ready, txBusy, rxLength;
static int8_t txResult;
static uint32_t txStarted, lastPoll, lastHealth;
static uint8_t rxData[LORA_MAX_PACKET_LENGTH];
static int16_t lastRssi;
static int8_t lastSnr;
volatile uint32_t loraSpiErrorCount;
volatile uint32_t loraCrcErrorCount;
volatile uint32_t loraRxDropCount;
volatile uint32_t loraTxTimeoutCount;
volatile LoRaDiagnostics loraDiagnostics;

static void selectRadio(void)
{
    HAL_GPIO_WritePin(LORA_NSS_GPIO_Port, LORA_NSS_Pin, GPIO_PIN_RESET);
}
static void releaseRadio(void)
{
    HAL_GPIO_WritePin(LORA_NSS_GPIO_Port, LORA_NSS_Pin, GPIO_PIN_SET);
}
static uint8_t readReg(uint8_t address, uint8_t *value)
{
    uint8_t tx[2] = {(uint8_t)(address & 0x7FU), 0U}, rx[2];
    HAL_StatusTypeDef status;
    selectRadio();
    status = HAL_SPI_TransmitReceive(&hspi1, tx, rx, 2U, SPI_TIMEOUT_MS);
    releaseRadio();
    if (status != HAL_OK) {
        loraSpiErrorCount++;
        loraDiagnostics.lastError = LORA_ERROR_SPI_READ;
        loraDiagnostics.failedRegister = address;
        loraDiagnostics.expectedValue = loraDiagnostics.actualValue = 0U;
        return 0U;
    }
    *value = rx[1];
    return 1U;
}
static uint8_t writeReg(uint8_t address, uint8_t value)
{
    uint8_t tx[2] = {(uint8_t)(address | 0x80U), value};
    HAL_StatusTypeDef status;
    selectRadio();
    status = HAL_SPI_Transmit(&hspi1, tx, 2U, SPI_TIMEOUT_MS);
    releaseRadio();
    if (status != HAL_OK) {
        loraSpiErrorCount++;
        loraDiagnostics.lastError = LORA_ERROR_SPI_WRITE;
        loraDiagnostics.failedRegister = address;
        loraDiagnostics.expectedValue = value;
        loraDiagnostics.actualValue = 0U;
        return 0U;
    }
    return 1U;
}
static uint8_t writeChecked(uint8_t address, uint8_t value)
{
    uint8_t actual;
    if (!writeReg(address, value) || !readReg(address, &actual)) return 0U;
    if (actual == value) return 1U;
    loraDiagnostics.lastError = LORA_ERROR_READBACK;
    loraDiagnostics.failedRegister = address;
    loraDiagnostics.expectedValue = value;
    loraDiagnostics.actualValue = actual;
    return 0U;
}
static uint8_t readFifo(uint8_t *data, uint8_t length)
{
    uint8_t tx[LORA_MAX_PACKET_LENGTH + 1U] = {0U};
    uint8_t rx[LORA_MAX_PACKET_LENGTH + 1U];
    HAL_StatusTypeDef status;
    selectRadio();
    status = HAL_SPI_TransmitReceive(&hspi1, tx, rx, (uint16_t)length + 1U,
                                     SPI_TIMEOUT_MS);
    releaseRadio();
    if (status != HAL_OK) {
        loraSpiErrorCount++;
        loraDiagnostics.lastError = LORA_ERROR_SPI_READ;
        loraDiagnostics.failedRegister = REG_FIFO;
        loraDiagnostics.expectedValue = loraDiagnostics.actualValue = 0U;
        return 0U;
    }
    memcpy(data, rx + 1U, length);
    return 1U;
}
static uint8_t writeFifo(const uint8_t *data, uint8_t length)
{
    uint8_t tx[LORA_MAX_PACKET_LENGTH + 1U];
    HAL_StatusTypeDef status;
    tx[0] = REG_FIFO | 0x80U;
    memcpy(tx + 1U, data, length);
    selectRadio();
    status = HAL_SPI_Transmit(&hspi1, tx, (uint16_t)length + 1U, SPI_TIMEOUT_MS);
    releaseRadio();
    if (status != HAL_OK) {
        loraSpiErrorCount++;
        loraDiagnostics.lastError = LORA_ERROR_SPI_WRITE;
        loraDiagnostics.failedRegister = REG_FIFO;
        loraDiagnostics.expectedValue = loraDiagnostics.actualValue = 0U;
        return 0U;
    }
    return 1U;
}
/* Do not touch application relay GPIOs during recovery. */
static void fault(void)
{
    ready = 0U;
    txBusy = 0U;
    txResult = -1;
    releaseRadio();
}
static uint8_t enterRx(void)
{
    uint8_t mapping;
    /* Do not clear RX flags/FIFO here: a newly arrived frame must survive. */
    return writeReg(REG_MODE, MODE_STANDBY) &&
           readReg(REG_DIO_MAPPING, &mapping) &&
           writeReg(REG_DIO_MAPPING, (uint8_t)(mapping & 0x3FU)) &&
           writeReg(REG_MODE, MODE_RX);
}


static uint8_t enableLnaBoost(void)
{
    uint8_t before, requested, actual;
    if (!readReg(REG_LNA, &before)) return 0U;
    requested = (uint8_t)(before | 0x03U);
    if (!writeReg(REG_LNA, requested) ||
        !readReg(REG_LNA, &actual)) return 0U;
    if ((actual & 0x03U) == 0x03U) return 1U;
    loraDiagnostics.lastError = LORA_ERROR_READBACK;
    loraDiagnostics.failedRegister = REG_LNA;
    loraDiagnostics.expectedValue = requested;
    loraDiagnostics.actualValue = actual;
    return 0U;
}

uint8_t LoRa_Init(void)
{
    uint8_t version;
    /* A live transaction must never be silently cancelled by initialization. */
    if (txBusy) return 0U;
    loraDiagnostics.initAttempts++;
    loraDiagnostics.initStage = LORA_INIT_RESET;
    loraDiagnostics.lastError = LORA_ERROR_NONE;
    loraDiagnostics.lastVersion = 0U;
    loraDiagnostics.failedRegister = 0U;
    loraDiagnostics.expectedValue = loraDiagnostics.actualValue = 0U;
    ready = 0U;
    txResult = 0;
    rxLength = 0U;
    releaseRadio();
    HAL_GPIO_WritePin(LORA_RST_GPIO_Port, LORA_RST_Pin, GPIO_PIN_RESET);
    HAL_Delay(10U);
    HAL_GPIO_WritePin(LORA_RST_GPIO_Port, LORA_RST_Pin, GPIO_PIN_SET);
    HAL_Delay(20U);
    loraDiagnostics.initStage = LORA_INIT_VERSION;
    if (!readReg(REG_VERSION, &version)) { fault(); return 0U; }
    loraDiagnostics.lastVersion = version;
    if (version != 0x12U) {
        loraDiagnostics.lastError = LORA_ERROR_VERSION;
        loraDiagnostics.failedRegister = REG_VERSION;
        loraDiagnostics.expectedValue = 0x12U;
        loraDiagnostics.actualValue = version;
        fault();
        return 0U;
    }
    loraDiagnostics.initStage = LORA_INIT_SLEEP;
    /* Enter FSK sleep before selecting LoRa, as required by SX127x. */
    if (!writeReg(REG_MODE, 0x00U)) { fault(); return 0U; }
    HAL_Delay(1U);
    if (!writeChecked(REG_MODE, MODE_SLEEP)) { fault(); return 0U; }
    loraDiagnostics.initStage = LORA_INIT_CONFIG;
    /* FRF = 433000000 * 2^19 / 32000000 = 0x6C4000. */
    if (!writeChecked(REG_FRF_MSB, 0x6CU) ||
        !writeChecked(REG_FRF_MID, 0x40U) ||
        !writeChecked(REG_FRF_LSB, 0x00U) ||
        !writeChecked(REG_TX_BASE, 0x00U) ||
        !writeChecked(REG_RX_BASE, 0x00U) ||
        !enableLnaBoost() ||
        !writeChecked(REG_PA_CONFIG, 0x8FU) ||
        !writeChecked(REG_PA_DAC, 0x84U) ||
        !writeChecked(REG_CONFIG1, 0x72U) ||
        !writeChecked(REG_CONFIG2, 0x74U) ||
        !writeChecked(REG_CONFIG3, 0x04U) ||
        !writeChecked(REG_SYMBOL_TIMEOUT, 100U) ||
        !writeChecked(REG_PREAMBLE_MSB, 0U) ||
        !writeChecked(REG_PREAMBLE_LSB, 8U) ||
        !writeChecked(REG_MAX_LENGTH, LORA_MAX_PACKET_LENGTH) ||
        !writeChecked(REG_SYNC, 0x12U) ||
        !writeChecked(REG_IRQ_MASK, 0x00U) ||
        !writeReg(REG_IRQ, 0xFFU))
    {
        fault();
        return 0U;
    }
    loraDiagnostics.initStage = LORA_INIT_RX;
    if (!enterRx()) { fault(); return 0U; }
    dioEvent = 0U;
    lastPoll = HAL_GetTick();
    lastHealth = lastPoll;
    ready = 1U;
    loraDiagnostics.initStage = LORA_INIT_READY;
    loraDiagnostics.initSuccesses++;
    return 1U;
}

/* Called only in foreground. Retain the first unread software frame. */
static uint8_t receiveFrame(uint8_t flags)
{
    uint8_t length, current, hop, rssi, snr;
    if ((flags & IRQ_CRC_ERROR) != 0U)
    {
        loraCrcErrorCount++;
        return writeReg(REG_IRQ, flags);
    }
    if (!readReg(REG_HOP_CHANNEL, &hop)) return 0U;
    /* In explicit-header mode bit 6 reports CRC presence in the received header. */
    if ((hop & 0x40U) == 0U)
    {
        loraCrcErrorCount++;
        return writeReg(REG_IRQ, flags);
    }
    if (!readReg(REG_RX_BYTES, &length)) return 0U;
    if (!length || length > LORA_MAX_PACKET_LENGTH || rxLength)
    {
        loraRxDropCount++;
        return writeReg(REG_IRQ, flags);
    }
    if (!readReg(REG_RX_CURRENT, &current) ||
        !writeReg(REG_FIFO_PTR, current) ||
        !readFifo(rxData, length) ||
        !readReg(REG_RSSI, &rssi) || !readReg(REG_SNR, &snr)) return 0U;
    lastSnr = (int8_t)snr;
    lastRssi = (int16_t)rssi - 164;
    if (lastSnr < 0) lastRssi += (int16_t)lastSnr / 4;
    if (!writeReg(REG_IRQ, flags)) return 0U;
    rxLength = length;
    loraDiagnostics.rxAccepted++;
    return 1U;
}

void LoRa_Process(void)
{
    uint8_t flags;
    uint32_t now = HAL_GetTick();
    if (!ready) return;
    /* A late foreground service can see TxDone after the software deadline.
     * Read the hardware flag before deciding whether transmission timed out. */
    if (!dioEvent && (uint32_t)(now - lastPoll) < RX_POLL_MS &&
        !(txBusy && (uint32_t)(now - txStarted) >= TX_TIMEOUT_MS)) return;
    dioEvent = 0U;
    lastPoll = now;
    if (!readReg(REG_IRQ, &flags)) { fault(); return; }
    if (txBusy)
    {
        if ((flags & IRQ_TX_DONE) != 0U)
        {
            loraDiagnostics.txDone++;
            if (!writeReg(REG_IRQ, IRQ_TX_DONE) || !enterRx()) { fault(); return; }
            txBusy = 0U;
            txResult = 1;
        }
        else if ((uint32_t)(now - txStarted) >= TX_TIMEOUT_MS)
        {
            loraTxTimeoutCount++;
            loraDiagnostics.lastError = LORA_ERROR_TX_TIMEOUT;
            loraDiagnostics.failedRegister = REG_IRQ;
            loraDiagnostics.expectedValue = IRQ_TX_DONE;
            loraDiagnostics.actualValue = flags;
            /* Abort only when the hardware still reports no completion. */
            (void)writeReg(REG_MODE, MODE_STANDBY);
            fault();
            return;
        }
    }
    else if ((flags & IRQ_RX_DONE) != 0U)
    {
        if (!receiveFrame(flags)) fault();
    }
    if (ready && !txBusy && (uint32_t)(now - lastHealth) >= 1000U)
    {
        uint8_t mode, config1, config2, config3, sync;
        lastHealth = now;
        /* A radio-only brownout can leave SPI responsive but reset all modem
         * settings. Require a full idle reinitialization rather than only RX. */
        if (!readReg(REG_MODE, &mode) || !readReg(REG_CONFIG1, &config1) ||
            !readReg(REG_CONFIG2, &config2) || !readReg(REG_CONFIG3, &config3) ||
            !readReg(REG_SYNC, &sync)) { fault(); return; }
        if (mode != MODE_RX || config1 != 0x72U || config2 != 0x74U ||
            config3 != 0x04U || sync != 0x12U) {
            loraDiagnostics.lastError = LORA_ERROR_HEALTH;
            loraDiagnostics.failedRegister = mode != MODE_RX ? REG_MODE :
                config1 != 0x72U ? REG_CONFIG1 : config2 != 0x74U ? REG_CONFIG2 :
                config3 != 0x04U ? REG_CONFIG3 : REG_SYNC;
            loraDiagnostics.expectedValue = mode != MODE_RX ? MODE_RX :
                config1 != 0x72U ? 0x72U : config2 != 0x74U ? 0x74U :
                config3 != 0x04U ? 0x04U : 0x12U;
            loraDiagnostics.actualValue = mode != MODE_RX ? mode :
                config1 != 0x72U ? config1 : config2 != 0x74U ? config2 :
                config3 != 0x04U ? config3 : sync;
            fault();
        }
    }
}

uint8_t LoRa_SendAsync(const uint8_t *data, uint8_t length)
{
    uint8_t flags, modem;
    if (!data || !length || length > LORA_MAX_PACKET_LENGTH || !ready || txBusy || rxLength)
        return 0U;
    /* Catch RxDone even on the polling-only slave before switching half-duplex. */
    dioEvent = 1U;
    LoRa_Process();
    if (!ready || rxLength) return 0U;
    if (!readReg(REG_MODEM_STATUS, &modem)) { fault(); return 0U; }
    if ((modem & 0x04U) != 0U) return 0U; /* A valid frame is already in reception. */
    if (!writeReg(REG_MODE, MODE_STANDBY) || !readReg(REG_IRQ, &flags))
    { fault(); return 0U; }
    /* Close the RxDone-vs-Standby race; never clear a just-completed RX frame. */
    if ((flags & IRQ_RX_DONE) != 0U)
    {
        if (!receiveFrame(flags) || !enterRx()) fault();
        return 0U;
    }
    if (!writeReg(REG_DIO_MAPPING, 0x40U) ||
        !writeReg(REG_IRQ, IRQ_TX_DONE) ||
        !writeReg(REG_FIFO_PTR, 0U) ||
        !writeFifo(data, length) ||
        !writeReg(REG_PAYLOAD_LENGTH, length) ||
        !writeReg(REG_MODE, MODE_TX))
    { fault(); return 0U; }
    txResult = 0;
    txBusy = 1U;
    txStarted = HAL_GetTick();
    loraDiagnostics.txAccepted++;
    return 1U;
}

uint8_t LoRa_IsReady(void) { return ready; }
uint8_t LoRa_IsBusy(void) { return txBusy; }
int8_t LoRa_TxResult(void) { return txResult; }
void LoRa_OnDio0Interrupt(void) { dioEvent = 1U; }
uint8_t LoRa_ReadPacket(uint8_t *buffer, uint8_t capacity)
{
    uint8_t length = rxLength;
    if (!buffer || !capacity || !length) return 0U;
    rxLength = 0U;
    if (length > capacity) { loraRxDropCount++; return 0U; }
    memcpy(buffer, rxData, length);
    return length;
}
int16_t LoRa_GetLastRSSI(void) { return lastRssi; }
int8_t LoRa_GetLastSNR(void) { return lastSnr; }
