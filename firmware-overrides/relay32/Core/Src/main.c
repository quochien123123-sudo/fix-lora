/* STM32F103C8 slave firmware; hardware pins follow relay32.ioc.
 * Build NODE_ID=1/2/3. Relay holds its state on RF/Wi-Fi/UART loss.
 * Power-on/reset policy is OFF; no watchdog-induced state change is enabled.
 */
#include "main.h"
#include "app_config.h"
#include "protocol.h"
#include "node_state.h"
#include "boot_counter.h"
#include "lora.h"
#include "sht30.h"
#include "lcd_i2c.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

ADC_HandleTypeDef hadc1;
I2C_HandleTypeDef hi2c1;
SPI_HandleTypeDef hspi1;

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_ADC1_Init(void);
static void MX_SPI1_Init(void);

static NodeControl control;
static uint8_t radioOK, replyPending, replySending;
static uint32_t radioRetryTime, replyQueuedAt;
static ProtoFrame pendingReply;
/* One bounded software slot keeps RX drained while a previous ACK owns TX.
 * A request is applied only after the previous reply has completed/expired.
 */
static ProtoFrame queuedRequest;
static uint8_t requestPending;
static uint8_t adcReady, sensorState, sensorMask, nextMask;
static uint8_t adcCount, adcValidCount, soilPercent, humidity;
static int16_t temperature10;
static uint32_t adcSum, sensorDue, sensorStarted, sampleAt;
static uint8_t sampleSeen;
static uint8_t buttonRaw, buttonStable;
static uint32_t buttonChangedAt, displayAt;
volatile uint32_t slaveRxCount, slaveBadPacketCount, slaveTxErrorCount;
volatile uint32_t slaveRequestDropCount, slaveTxDoneCount, slaveAckExpiredCount;
volatile uint32_t slaveBootFlashError;

static void Relay_Write(void)
{
    HAL_GPIO_WritePin(RELAY_GPIO_Port, RELAY_Pin,
                      control.relay ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void Button_Process(uint32_t now)
{
    uint8_t raw = (uint8_t)HAL_GPIO_ReadPin(BUTTON_GPIO_Port, BUTTON_Pin);
    if (raw != buttonRaw) {
        buttonRaw = raw;
        buttonChangedAt = now;
    }
    if (raw != buttonStable && now - buttonChangedAt >= BUTTON_DEBOUNCE_MS) {
        buttonStable = raw;
        if (raw == GPIO_PIN_RESET) {
            NodeControl_LocalToggle(&control);
            Relay_Write();
        }
    }
}

static uint8_t Soil_Percent(uint16_t raw)
{
    int32_t percent;
    if (SOIL_DRY_VALUE == SOIL_WET_VALUE) return 0U;
    percent = ((int32_t)SOIL_DRY_VALUE - raw) * 100L /
              ((int32_t)SOIL_DRY_VALUE - (int32_t)SOIL_WET_VALUE);
    if (percent < 0L) percent = 0L;
    if (percent > 100L) percent = 100L;
    return (uint8_t)percent;
}

/* A single bounded operation per pass, with the SHT30 conversion wait outside
 * the CPU. Failed ADC samples clear validity; they are never turned into 100%.
 */
static void Sensors_Process(uint32_t now)
{
    if (sensorState == 0U) {
        if (now - sensorDue < SENSOR_PERIOD_MS) return;
        sensorDue = now;
        sensorStarted = now;
        nextMask = 0U;
        adcCount = adcValidCount = 0U;
        adcSum = 0U;
        sensorState = (SHT30_Start() == HAL_OK) ? 1U : 2U;
        return;
    }
    if (sensorState == 1U) {
        int16_t nextTemperature;
        uint8_t nextHumidity;
        if (now - sensorStarted < 20U) return;
        if (SHT30_Finish(&nextTemperature, &nextHumidity) == HAL_OK) {
            temperature10 = nextTemperature;
            humidity = nextHumidity;
            nextMask |= 0x01U;
        }
        sensorState = 2U;
        return;
    }
    if (adcCount < 8U) {
        if (adcReady && HAL_ADC_Start(&hadc1) == HAL_OK) {
            if (HAL_ADC_PollForConversion(&hadc1, 1U) == HAL_OK) {
                adcSum += HAL_ADC_GetValue(&hadc1);
                ++adcValidCount;
            }
            (void)HAL_ADC_Stop(&hadc1);
        }
        ++adcCount;
        return;
    }
    if (adcValidCount == 8U && SOIL_DRY_VALUE != SOIL_WET_VALUE) {
        soilPercent = Soil_Percent((uint16_t)(adcSum / 8U));
        nextMask |= 0x02U;
    }
    sensorMask = nextMask;
    sampleSeen = 1U;
    sampleAt = now;
    sensorState = 0U;
}

static void QueueReply(const ProtoFrame *request, uint8_t result, uint32_t now)
{
    memset(&pendingReply, 0, sizeof(pendingReply));
    pendingReply.type = PROTO_REPLY;
    pendingReply.node = NODE_ID;
    pendingReply.masterSession = request->masterSession;
    pendingReply.seq = request->seq;
    pendingReply.nodeBoot = control.nodeBoot;
    pendingReply.revision = control.revision;
    pendingReply.relay = control.relay;
    pendingReply.valid = sensorMask;
    pendingReply.result = result;
    pendingReply.temp10 = temperature10;
    pendingReply.humidity = humidity;
    pendingReply.soil = soilPercent;
    pendingReply.sampleAgeMs = sampleSeen ? now - sampleAt : UINT32_MAX;
    replyPending = 1U;
    replyQueuedAt = now;
}

static void Radio_Process(uint32_t now)
{
    uint8_t bytes[34], length;
    ProtoFrame request;
    if (!control.nodeBoot) return; /* Persistent session storage failed. */
    if (!radioOK) {
        if (now - radioRetryTime >= RADIO_RETRY_MS) {
            radioRetryTime = now;
            radioOK = LoRa_Init();
        }
        return;
    }
    LoRa_Process();
    if (!LoRa_IsReady()) {
        radioOK = replyPending = replySending = requestPending = 0U;
        radioRetryTime = now;
        ++slaveTxErrorCount;
        return;
    }
    if (replySending && !LoRa_IsBusy()) {
        if (LoRa_TxResult() < 0) ++slaveTxErrorCount;
        else if (LoRa_TxResult() > 0) ++slaveTxDoneCount;
        replySending = 0U;
    }
    /* Drain at most one packet per foreground pass even while an ACK is queued.
     * Otherwise rxLength makes LoRa_SendAsync refuse TX while the application
     * refuses to read RX, and the ACK expires without ever reaching the air.
     */
    length = LoRa_ReadPacket(bytes, sizeof(bytes));
    if (length) {
        if (!Proto_Decode(&request, bytes, length)) {
            ++slaveBadPacketCount;
        } else if (request.node != NODE_ID) {
            /* Other addressed nodes share this channel; this is not an error. */
        } else if (request.masterSession != 0U && request.seq != 0U &&
                   (request.type == PROTO_POLL || request.type == PROTO_SET)) {
            if (!requestPending) {
                queuedRequest = request;
                requestPending = 1U;
            } else {
                /* Bounded overload: discard BEFORE changing any relay state.
                 * The master can retry its absolute SET with the same sequence.
                 */
                ++slaveRequestDropCount;
            }
        } else ++slaveBadPacketCount;
    }
    if (requestPending && !replyPending && !replySending) {
        uint8_t previous = control.relay;
        uint8_t result = NodeControl_Handle(&control, &queuedRequest);
        ++slaveRxCount;
        if (control.relay != previous) Relay_Write();
        QueueReply(&queuedRequest, result, now);
        requestPending = 0U;
    }
    if (replyPending && !LoRa_IsBusy()) {
        if (now - replyQueuedAt > 200U) {
            replyPending = 0U; /* Never transmit an indefinitely delayed ACK. */
            ++slaveAckExpiredCount;
            return;
        }
        /* Re-read current relay/revision in case the local button changed while
         * waiting for the radio, without re-executing the remote command.
         */
        pendingReply.relay = control.relay;
        pendingReply.revision = control.revision;
        if (Proto_Encode(bytes, &pendingReply) == sizeof(bytes) &&
            LoRa_SendAsync(bytes, sizeof(bytes))) {
            replyPending = 0U;
            replySending = 1U;
        }
    }
}

static void Display_Update(uint32_t now)
{
    char line[32]; /* LCD_SetLine clips/pads to the physical 16 columns. */
    int32_t absTemp;
    if (now - displayAt < 500U) return;
    displayAt = now;
    if (!control.nodeBoot)
        snprintf(line, sizeof(line), "N%u BOOT:ERR", (unsigned)NODE_ID);
    else if (!LoRa_IsReady())
        snprintf(line, sizeof(line), "N%u E%u V%02X R%02X", (unsigned)NODE_ID,
                 (unsigned)loraDiagnostics.lastError, (unsigned)loraDiagnostics.lastVersion,
                 (unsigned)loraDiagnostics.failedRegister);
    else
        snprintf(line, sizeof(line), "N%u R:%u CHIP:OK", (unsigned)NODE_ID,
                 (unsigned)control.relay);
    LCD_SetLine(0U, line);
    if (!control.nodeBoot) {
        snprintf(line, sizeof(line), "FLASH JOURNAL ERR");
    } else if (!LoRa_IsReady()) {
        snprintf(line, sizeof(line), "REG %02X %02X/%02X",
                 (unsigned)loraDiagnostics.failedRegister, (unsigned)loraDiagnostics.expectedValue,
                 (unsigned)loraDiagnostics.actualValue);
    } else if ((now / 1000U) & 1U) {
        /* TX counts local TxDone, RX counts valid addressed requests. These
         * are link evidence, unlike successful register initialization alone.
         */
        snprintf(line, sizeof(line), "RX%lu TX%lu",
                 (unsigned long)(slaveRxCount % 100000U),
                 (unsigned long)(slaveTxDoneCount % 100000U));
    } else if ((sensorMask & 0x03U) == 0x03U) {
        absTemp = temperature10 < 0 ? -(int32_t)temperature10 : temperature10;
        snprintf(line, sizeof(line), "%s%ld.%ldC H%u S%u", temperature10 < 0 ? "-" : "",
                 (long)(absTemp / 10L), (long)(absTemp % 10L),
                 (unsigned)humidity, (unsigned)soilPercent);
    } else {
        snprintf(line, sizeof(line), "SHT:%s ADC:%s", (sensorMask & 1U) ? "OK" : "ERR",
                 (sensorMask & 2U) ? "OK" : "ERR");
    }
    LCD_SetLine(1U, line);
}

int main(void)
{
    uint32_t now, boot;
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_I2C1_Init();
    MX_ADC1_Init();
    MX_SPI1_Init();
    boot = BootCounter_Next();
    if (!boot) slaveBootFlashError = HAL_FLASH_GetError();
    NodeControl_Init(&control, boot);
    Relay_Write();
    adcReady = HAL_ADCEx_Calibration_Start(&hadc1) == HAL_OK;
    buttonRaw = buttonStable = (uint8_t)HAL_GPIO_ReadPin(BUTTON_GPIO_Port, BUTTON_Pin);
    buttonChangedAt = HAL_GetTick();
    LCD_Init();
    now = HAL_GetTick();
    sensorDue = now - SENSOR_PERIOD_MS;
    radioRetryTime = now;
    radioOK = control.nodeBoot ? LoRa_Init() : 0U;
    while (1) {
        now = HAL_GetTick();
        /* No timeout/offline path ever calls Relay_Write(). */
        Button_Process(now);
        Radio_Process(now);
        if (!replyPending && !replySending && (!radioOK || !LoRa_IsBusy())) {
            Sensors_Process(now);
            Display_Update(now);
            LCD_Process();
        }
    }
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
    RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK |
                                  RCC_CLOCKTYPE_SYSCLK |
                                  RCC_CLOCKTYPE_PCLK1 |
                                  RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
    {
        Error_Handler();
    }

    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
    PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV6;

    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
        Error_Handler();
    }
}

static void MX_ADC1_Init(void)
{
    ADC_ChannelConfTypeDef sConfig = {0};

    hadc1.Instance = ADC1;
    hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
    hadc1.Init.ContinuousConvMode = DISABLE;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hadc1.Init.NbrOfConversion = 1;

    if (HAL_ADC_Init(&hadc1) != HAL_OK)
    {
        Error_Handler();
    }

    sConfig.Channel = ADC_CHANNEL_2;
    sConfig.Rank = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = ADC_SAMPLETIME_71CYCLES_5;

    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
    {
        Error_Handler();
    }
}

static void MX_I2C1_Init(void)
{
    hi2c1.Instance = I2C1;
    hi2c1.Init.ClockSpeed = 100000;
    hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
    hi2c1.Init.OwnAddress1 = 0;
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2 = 0;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;

    if (HAL_I2C_Init(&hi2c1) != HAL_OK)
    {
        Error_Handler();
    }
}

static void MX_SPI1_Init(void)
{
    hspi1.Instance = SPI1;
    hspi1.Init.Mode = SPI_MODE_MASTER;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi1.Init.NSS = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial = 10;

    if (HAL_SPI_Init(&hspi1) != HAL_OK)
    {
        Error_Handler();
    }
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    HAL_GPIO_WritePin(RELAY_GPIO_Port, RELAY_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(LORA_NSS_GPIO_Port, LORA_NSS_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(LORA_RST_GPIO_Port, LORA_RST_Pin, GPIO_PIN_SET);

    GPIO_InitStruct.Pin = BUTTON_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(BUTTON_GPIO_Port, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = RELAY_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(RELAY_GPIO_Port, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = LORA_NSS_Pin | LORA_RST_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = LORA_DIO0_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(LORA_DIO0_GPIO_Port, &GPIO_InitStruct);
}

/* USER CODE BEGIN 4 */
/* USER CODE END 4 */

void Error_Handler(void)
{
    __disable_irq();

    while (1)
    {
    }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
}
#endif
