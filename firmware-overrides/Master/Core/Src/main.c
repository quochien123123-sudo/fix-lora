/* Three-node STM32F103C8 LoRa master. User pinout is preserved.
 * PB0 selects node 1..3; PB1 requests an absolute SET for selected node.
 * Link timeouts never send OFF. See firmware/CONTRACT.md and README. */
#include "main.h"
#include "lora.h"
#include "lcd2004.h"
#include "protocol.h"
#include "boot_counter.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#define NODE_COUNT 3U
#define UART_RING_SIZE 1024U
#define UART_LINE_SIZE 192U
#define RESULT_CACHE_SIZE 12U
#define NODE_OFFLINE_MS 8000U
#define SENSOR_STALE_MS 5000U
#define COMMAND_LIFETIME_MS 6000U
#define POLL_INTERVAL_MS 2000U
#define POLL_WAIT_MS 250U
#define COMMAND_WAIT_MS 350U
#define SET_ATTEMPTS 3U
#define RADIO_RECOVERY_MS 3000U

I2C_HandleTypeDef hi2c1;
SPI_HandleTypeDef hspi1;
UART_HandleTypeDef huart1;

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART1_UART_Init(void);

/* These counters may also be inspected with a debugger. */
volatile uint32_t masterBootFlashError;
volatile uint32_t masterUartErrors, masterProtocolErrors, masterRadioErrors;
volatile uint32_t masterCommandOk, masterCommandUnconfirmed, dio0InterruptCount;
static uint32_t masterSession, nextSequence;
static uint8_t radioReady, selectedNode;
static uint32_t nextRadioRecovery, nextTelemetry, nextLCD;
static uint8_t lcdRow;

typedef struct {
    uint8_t pending, desired;
    uint32_t gatewaySession, id, queuedAt;
} Command;
typedef struct {
    uint8_t seen, relay, valid, humidity, soil;
    int16_t temp10, rssi;
    int8_t snrQuarter;
    uint32_t nodeBoot, revision, sampleAgeMs, lastSeen, nextPoll;
    Command command;
} NodeState;
static NodeState nodes[NODE_COUNT];

typedef struct {
    uint8_t used, node, desired, relay, known;
    uint32_t gatewaySession, id;
    char status[16];
} CachedResult;
static CachedResult completed[RESULT_CACHE_SIZE];
static uint8_t cacheWrite;
static uint32_t acceptedGatewaySession, newestGatewayCommandId;

/* Only one half-duplex RF transaction owns the radio at a time. */
typedef enum { RF_NONE, RF_BACKGROUND_POLL, RF_PREFLIGHT, RF_SET } RadioJob;
typedef struct {
    RadioJob job;
    uint8_t nodeIndex, waiting, attempts, txQueued;
    uint32_t txStarted, waitStarted;
    ProtoFrame request;
} RadioOwner;
static RadioOwner owner;
static uint8_t roundRobin;
static uint8_t consecutiveRadioFailures;

static volatile uint8_t rxRing[UART_RING_SIZE];
static volatile uint16_t rxHead, rxTail;
static volatile uint8_t rxFault;
static char rxLine[UART_LINE_SIZE];
static uint16_t rxLineLength;
static uint8_t rxDropLine;

static uint8_t Due(uint32_t now, uint32_t deadline)
{ return (int32_t)(now - deadline) >= 0; }
static uint32_t Age(uint32_t now, uint32_t then)
{ return now - then; }
static uint8_t Online(const NodeState *n, uint32_t now)
{ return n->seen && Age(now, n->lastSeen) <= NODE_OFFLINE_MS; }
static uint32_t NextSequence(void)
{ if (++nextSequence == 0U) ++nextSequence; return nextSequence; }

static void UARTSend(const char *line)
{
    size_t length = strlen(line);
    if (length && length < UART_LINE_SIZE &&
        HAL_UART_Transmit(&huart1, (uint8_t *)line, (uint16_t)length, 25U) != HAL_OK)
        masterUartErrors++;
}

/* This is the sole receiver of USART1 DR; HAL receive-IT is not mixed with it. */
void ESP32_UART_IRQHandler(void)
{
    uint32_t sr = USART1->SR;
    if (sr & (USART_SR_RXNE | USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)) {
        uint8_t ch = (uint8_t)USART1->DR; /* SR followed by DR clears UART faults */
        if (sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)) {
            rxFault = 1U;
            return;
        }
        uint16_t next = (uint16_t)((rxHead + 1U) & (UART_RING_SIZE - 1U));
        if (next == rxTail) rxFault = 1U;
        else { rxRing[rxHead] = ch; rxHead = next; }
    }
}

static void SendResult(uint32_t gateway, uint32_t id, uint8_t node,
                       const char *status, uint8_t relay, uint8_t known)
{
    char line[UART_LINE_SIZE];
    int count = snprintf(line, sizeof(line), "U1,A,%lu,%lu,%u,%s,%u,%u\n",
                         (unsigned long)gateway, (unsigned long)id,
                         (unsigned int)node, status, (unsigned int)relay,
                         (unsigned int)known);
    if (count > 0 && count < (int)sizeof(line)) UARTSend(line);
}
static CachedResult *FindResult(uint32_t gateway, uint32_t id)
{
    unsigned i;
    for (i = 0; i < RESULT_CACHE_SIZE; ++i)
        if (completed[i].used && completed[i].gatewaySession == gateway && completed[i].id == id)
            return &completed[i];
    return NULL;
}
static void StoreResult(uint32_t gateway, uint32_t id, uint8_t node, uint8_t desired,
                        const char *status, uint8_t relay, uint8_t known)
{
    CachedResult *cache = &completed[cacheWrite];
    cacheWrite = (uint8_t)((cacheWrite + 1U) % RESULT_CACHE_SIZE);
    cache->used = 1U;
    cache->node = node;
    cache->desired = desired;
    cache->relay = relay;
    cache->known = known;
    cache->gatewaySession = gateway;
    cache->id = id;
    snprintf(cache->status, sizeof(cache->status), "%s", status);
    if (gateway) SendResult(gateway, id, node, status, relay, known);
}
static void FinishCommand(uint8_t index, const char *status)
{
    NodeState *n = &nodes[index];
    Command *cmd = &n->command;
    if (!cmd->pending) return;
    cmd->pending = 0U;
    if (!strcmp(status, "OK")) masterCommandOk++;
    else masterCommandUnconfirmed++;
    StoreResult(cmd->gatewaySession, cmd->id, index + 1U, cmd->desired,
                status, n->relay, n->seen);
}

static void RequestCommand(uint32_t gateway, uint32_t id, uint8_t node, uint8_t desired)
{
    unsigned i;
    NodeState *n = &nodes[node - 1U];
    CachedResult *cache = FindResult(gateway, id);
    if (cache) {
        if (cache->node != node || cache->desired != desired)
            SendResult(gateway, id, node, "BAD_REQUEST", n->relay, n->seen);
        else if (gateway) {
            /* Keep the transaction result, but never masquerade an old cached
             * output as the current known relay state after a local override. */
            const char *status = cache->status;
            if (!strcmp(status, "OK") && n->seen && n->relay != cache->desired)
                status = "OVERRIDDEN";
            SendResult(gateway, id, node, status, n->relay, n->seen);
        }
        return;
    }
    for (i = 0; i < NODE_COUNT; ++i) {
        Command *other = &nodes[i].command;
        if (other->pending && other->gatewaySession == gateway && other->id == id) {
            if (i + 1U != node || other->desired != desired)
                SendResult(gateway, id, node, "BAD_REQUEST", n->relay, n->seen);
            return; /* same request is already running; never re-execute it */
        }
    }
    if (gateway) {
        if (gateway == acceptedGatewaySession &&
            (int32_t)(id - newestGatewayCommandId) <= 0) {
            SendResult(gateway, id, node, "BAD_REQUEST", n->relay, n->seen);
            return; /* an evicted old ID must not become a new pump command */
        }
        acceptedGatewaySession = gateway;
        newestGatewayCommandId = id;
    }
    if (n->command.pending) {
        StoreResult(gateway, id, node, desired, "BUSY", n->relay, n->seen);
        return;
    }
    if (!radioReady || !masterSession) {
        StoreResult(gateway, id, node, desired, "RADIO_ERROR", n->relay, n->seen);
        return;
    }
    if (!Online(n, HAL_GetTick())) {
        StoreResult(gateway, id, node, desired, "OFFLINE", n->relay, n->seen);
        return;
    }
    n->command.pending = 1U;
    n->command.desired = desired;
    n->command.gatewaySession = gateway;
    n->command.id = id;
    n->command.queuedAt = HAL_GetTick();
}

static void HandleUARTLine(char *line)
{
    char *field[8];
    uint32_t gateway, id, node, value, expectedMasterSession;
    size_t count = Proto_Split(line, field, 8U);
    if (count != 7U || strcmp(field[0], "U1") || strcmp(field[1], "C") ||
        !Proto_U32(field[2], &gateway) || !Proto_U32(field[3], &id) ||
        !Proto_U32(field[4], &node) || !Proto_U32(field[5], &value) ||
        !Proto_U32(field[6], &expectedMasterSession) ||
        !gateway || !id || node < 1U || node > NODE_COUNT || value > 1U) {
        masterProtocolErrors++;
        return;
    }
    /* A retry queued before this Master reboot must not become a fresh SET.
     * Check the receiver epoch before dedup lookup or command admission. */
    if (!masterSession || expectedMasterSession != masterSession) {
        NodeState *n = &nodes[node - 1U];
        SendResult(gateway, id, (uint8_t)node, "BOOT_CHANGED", n->relay, n->seen);
        return;
    }
    RequestCommand(gateway, id, (uint8_t)node, (uint8_t)value);
}
static void UARTProcess(void)
{
    uint16_t budget = UART_RING_SIZE;
    while (budget--) {
        uint8_t ch;
        uint32_t mask = __get_PRIMASK();
        __disable_irq();
        if (rxFault) {
            rxTail = rxHead;
            rxFault = 0U;
            rxDropLine = 1U;
            rxLineLength = 0U;
            masterUartErrors++;
        }
        if (rxTail == rxHead) { __set_PRIMASK(mask); break; }
        ch = rxRing[rxTail];
        rxTail = (uint16_t)((rxTail + 1U) & (UART_RING_SIZE - 1U));
        __set_PRIMASK(mask);
        if (ch == '\r') continue;
        if (ch == '\n') {
            if (!rxDropLine && rxLineLength) {
                rxLine[rxLineLength] = '\0';
                HandleUARTLine(rxLine);
            }
            rxDropLine = 0U;
            rxLineLength = 0U;
        } else if (!rxDropLine) {
            if (ch == 0U || rxLineLength >= sizeof(rxLine) - 1U) {
                rxDropLine = 1U;
                rxLineLength = 0U;
                masterProtocolErrors++;
            } else rxLine[rxLineLength++] = (char)ch;
        }
    }
}

static void UpdateNode(uint8_t index, const ProtoFrame *reply)
{
    NodeState *n = &nodes[index];
    n->seen = 1U;
    n->lastSeen = HAL_GetTick();
    n->nextPoll = n->lastSeen + POLL_INTERVAL_MS;
    n->nodeBoot = reply->nodeBoot;
    n->revision = reply->revision;
    n->relay = reply->relay;
    n->valid = reply->valid;
    n->temp10 = reply->temp10;
    n->humidity = reply->humidity;
    n->soil = reply->soil;
    n->sampleAgeMs = reply->sampleAgeMs;
    n->rssi = LoRa_GetLastRSSI();
    n->snrQuarter = LoRa_GetLastSNR();
    consecutiveRadioFailures = 0U;
}
static uint8_t StartTransmission(void)
{
    uint8_t raw[34];
    if (!Proto_Encode(raw, &owner.request) || !radioReady || !LoRa_IsReady()) return 0U;
    owner.waiting = 0U;
    owner.txQueued = 1U;
    owner.txStarted = HAL_GetTick();
    return 1U;
}
static void ClearOwner(void)
{
    nodes[owner.nodeIndex].nextPoll = HAL_GetTick() + POLL_INTERVAL_MS;
    owner.job = RF_NONE;
}
static void RadioFailure(void)
{
    masterRadioErrors++;
    if (++consecutiveRadioFailures >= 3U) {
        radioReady = 0U;
        nextRadioRecovery = HAL_GetTick() + RADIO_RECOVERY_MS;
    }
}
static void StartJob(uint8_t index, RadioJob job)
{
    memset(&owner, 0, sizeof(owner));
    owner.nodeIndex = index;
    owner.job = job;
    owner.attempts = 1U;
    owner.request.type = PROTO_POLL;
    owner.request.node = index + 1U;
    owner.request.masterSession = masterSession;
    owner.request.seq = NextSequence();
    if (!StartTransmission()) {
        RadioFailure();
        if (job == RF_PREFLIGHT) FinishCommand(index, "RADIO_ERROR");
        ClearOwner();
    }
}
static void RetryOrFinish(const char *status)
{
    uint8_t index = owner.nodeIndex;
    if (owner.job == RF_SET && owner.attempts < SET_ATTEMPTS && radioReady &&
        Age(HAL_GetTick(), nodes[index].command.queuedAt) < COMMAND_LIFETIME_MS) {
        owner.attempts++;
        if (StartTransmission()) return;
        RadioFailure();
        status = "RADIO_ERROR";
    }
    if (owner.job == RF_SET || owner.job == RF_PREFLIGHT) FinishCommand(index, status);
    ClearOwner();
}
static void HandleReply(const ProtoFrame *reply)
{
    if (owner.job == RF_NONE || reply->type != PROTO_REPLY ||
        reply->node != owner.request.node || reply->masterSession != masterSession ||
        reply->seq != owner.request.seq || !reply->nodeBoot) return;
    uint8_t index = owner.nodeIndex;
    RadioJob job = owner.job;
    UpdateNode(index, reply);
    if (job == RF_BACKGROUND_POLL) { ClearOwner(); return; }
    if (!nodes[index].command.pending) { ClearOwner(); return; }
    if (job == RF_PREFLIGHT) {
        if (reply->result != PROTO_STATUS || !reply->nodeBoot) {
            FinishCommand(index, "RADIO_ERROR"); ClearOwner(); return;
        }
        if (Age(HAL_GetTick(), nodes[index].command.queuedAt) >= COMMAND_LIFETIME_MS) {
            FinishCommand(index, "ACK_TIMEOUT"); ClearOwner(); return;
        }
        owner.job = RF_SET;
        owner.attempts = 1U;
        owner.request.type = PROTO_SET;
        owner.request.seq = NextSequence();
        owner.request.nodeBoot = reply->nodeBoot;
        owner.request.revision = reply->revision;
        if (nodes[index].command.gatewaySession == 0U)
            nodes[index].command.desired = !reply->relay; /* toggle freshly polled state */
        owner.request.relay = nodes[index].command.desired;
        if (!StartTransmission()) {
            RadioFailure(); RetryOrFinish("RADIO_ERROR");
        }
        return;
    }
    const char *status;
    if (reply->nodeBoot != owner.request.nodeBoot || reply->result == PROTO_BOOT_CHANGED)
        status = "BOOT_CHANGED";
    else if (reply->result == PROTO_APPLIED || reply->result == PROTO_DUPLICATE)
        status = reply->relay == nodes[index].command.desired ? "OK" : "OVERRIDDEN";
    else if (reply->result == PROTO_CONFLICT) status = "CONFLICT";
    else status = "BAD_REQUEST";
    FinishCommand(index, status);
    ClearOwner();
}
static uint8_t HaveCommands(void)
{
    unsigned i;
    for (i = 0; i < NODE_COUNT; ++i) if (nodes[i].command.pending) return 1U;
    return 0U;
}
static void RadioProcess(void)
{
    uint32_t now = HAL_GetTick();
    unsigned i;
    if (radioReady) {
        uint8_t raw[128], length;
        LoRa_Process();
        while ((length = LoRa_ReadPacket(raw, sizeof(raw))) != 0U) {
            ProtoFrame reply;
            if (Proto_Decode(&reply, raw, length)) HandleReply(&reply);
            else masterProtocolErrors++;
        }
    }
    now = HAL_GetTick(); /* callbacks above can enqueue a newer transaction */
    if (owner.job != RF_NONE) {
        if (owner.txQueued) {
            uint8_t raw[34];
            if (owner.job == RF_SET &&
                Age(now, nodes[owner.nodeIndex].command.queuedAt) >= COMMAND_LIFETIME_MS) {
                FinishCommand(owner.nodeIndex, "ACK_TIMEOUT"); ClearOwner();
            } else if (!LoRa_IsReady() || Age(now, owner.txStarted) >= 250U) {
                RadioFailure(); RetryOrFinish("RADIO_ERROR");
            } else if (Proto_Encode(raw, &owner.request) && LoRa_SendAsync(raw, sizeof(raw))) {
                owner.txQueued = 0U;
                owner.txStarted = now;
            }
        } else if (!owner.waiting) {
            int8_t result = LoRa_TxResult();
            if (result > 0) { owner.waiting = 1U; owner.waitStarted = now; }
            else if (result < 0 || Age(now, owner.txStarted) > 600U) {
                RadioFailure(); RetryOrFinish("RADIO_ERROR");
            }
        } else if (Age(now, owner.waitStarted) >=
                   (owner.job == RF_SET ? COMMAND_WAIT_MS : POLL_WAIT_MS))
            RetryOrFinish("ACK_TIMEOUT");
    }
    /* Expire queued requests; timeout never changes a physical relay. */
    for (i = 0; i < NODE_COUNT; ++i) {
        if (nodes[i].command.pending && Age(now, nodes[i].command.queuedAt) >= COMMAND_LIFETIME_MS &&
            !(owner.job != RF_NONE && owner.nodeIndex == i))
            FinishCommand((uint8_t)i, "ACK_TIMEOUT");
    }
    if (owner.job != RF_NONE || LoRa_IsBusy()) return;
    if (!radioReady) {
        if (masterSession && Due(now, nextRadioRecovery)) {
            radioReady = LoRa_Init();
            consecutiveRadioFailures = 0U;
            nextRadioRecovery = HAL_GetTick() + RADIO_RECOVERY_MS;
        }
        if (!radioReady) return;
    }
    /* A continuously busy command source cannot starve another node's liveness poll. */
    for (i = 0; i < NODE_COUNT; ++i) {
        uint8_t idx = (uint8_t)((roundRobin + i) % NODE_COUNT);
        if (!nodes[idx].command.pending && Due(now, nodes[idx].nextPoll + 3000U)) {
            roundRobin = (uint8_t)((idx + 1U) % NODE_COUNT);
            StartJob(idx, RF_BACKGROUND_POLL); return;
        }
    }
    for (i = 0; i < NODE_COUNT; ++i) {
        uint8_t idx = (uint8_t)((roundRobin + i) % NODE_COUNT);
        if (nodes[idx].command.pending) {
            roundRobin = (uint8_t)((idx + 1U) % NODE_COUNT);
            StartJob(idx, RF_PREFLIGHT); return;
        }
    }
    for (i = 0; i < NODE_COUNT; ++i) {
        uint8_t idx = (uint8_t)((roundRobin + i) % NODE_COUNT);
        if (Due(now, nodes[idx].nextPoll)) {
            roundRobin = (uint8_t)((idx + 1U) % NODE_COUNT);
            StartJob(idx, RF_BACKGROUND_POLL); return;
        }
    }
}

static void TelemetryProcess(void)
{
    uint32_t now = HAL_GetTick();
    if (!Due(now, nextTelemetry)) return;
    nextTelemetry = now + 1000U;
    char line[UART_LINE_SIZE];
    snprintf(line, sizeof(line), "U1,H,%lu,%u\n", (unsigned long)masterSession, (unsigned)radioReady);
    UARTSend(line);
    unsigned i;
    for (i = 0; i < NODE_COUNT; ++i) {
        NodeState *n = &nodes[i];
        uint32_t seenAge = n->seen ? Age(now, n->lastSeen) : UINT32_MAX;
        uint32_t sampleAge = n->seen && n->sampleAgeMs <= UINT32_MAX - seenAge
                           ? n->sampleAgeMs + seenAge : UINT32_MAX;
        uint8_t online = Online(n, now);
        uint8_t valid = online && sampleAge <= SENSOR_STALE_MS ? n->valid : 0U;
        snprintf(line, sizeof(line), "U1,N,%u,%u,%u,%u,%u,%d,%u,%u,%lu,%lu,%d,%d\n",
                 i + 1U, (unsigned)online, (unsigned)n->seen, (unsigned)n->relay,
                 (unsigned)valid, (int)n->temp10, (unsigned)n->humidity, (unsigned)n->soil,
                 (unsigned long)sampleAge, (unsigned long)seenAge, (int)n->rssi, (int)n->snrQuarter);
        UARTSend(line);
    }
}

typedef struct { uint8_t raw, stable; uint32_t changedAt; } Button;
static Button selectButton = {1U,1U,0U}, controlButton = {1U,1U,0U};
static uint8_t Pressed(Button *b, uint16_t pin)
{
    uint32_t now = HAL_GetTick();
    uint8_t value = HAL_GPIO_ReadPin(GPIOB, pin) == GPIO_PIN_SET;
    if (value != b->raw) { b->raw = value; b->changedAt = now; }
    if (value != b->stable && Age(now, b->changedAt) >= 40U) {
        b->stable = value;
        return !value;
    }
    return 0U;
}
static void ButtonsProcess(void)
{
    static uint32_t localId;
    if (Pressed(&selectButton, BTN_NODE1_Pin)) {
        selectedNode = (uint8_t)((selectedNode + 1U) % NODE_COUNT);
        nextLCD = HAL_GetTick();
    }
    if (Pressed(&controlButton, BTN_NODE2_Pin)) {
        NodeState *n = &nodes[selectedNode];
        if (Online(n, HAL_GetTick()) && n->seen && !n->command.pending) {
            if (++localId == 0U) ++localId;
            RequestCommand(0U, localId, selectedNode + 1U, !n->relay);
        }
    }
}
static void LCDProcess(void)
{
    uint32_t now = HAL_GetTick();
    if (!Due(now, nextLCD) || !LCD2004_IsReady() || owner.job != RF_NONE || HaveCommands()) return;
    /* Only reserve a poll window when the radio can actually use it.
     * Failed radio/boot initialization must still allow the LCD to show ERR. */
    if (radioReady && LoRa_IsReady()) {
        unsigned i;
        for (i = 0; i < NODE_COUNT; ++i)
            if (Due(now + 60U, nodes[i].nextPoll)) return;
    }
    nextLCD = now + 150U;
    char line[21];
    if (lcdRow < NODE_COUNT) {
        NodeState *n = &nodes[lcdRow];
        snprintf(line, sizeof(line), "%cN%u %-3s R:%s%s",
                 selectedNode == lcdRow ? '>' : ' ', (unsigned)lcdRow + 1U,
                 Online(n, now) ? "ONL" : "OFF", n->seen ? (n->relay ? "ON" : "OFF") : "?",
                 n->command.pending ? " WAIT" : "");
    } else {
        if (!masterSession)
            snprintf(line, sizeof(line), "BOOT:ERR FLASH:%lx", (unsigned long)masterBootFlashError);
        else if (!LoRa_IsReady())
            snprintf(line, sizeof(line), "E%u V%02X R%02X %02X/%02X",
                     (unsigned)loraDiagnostics.lastError, (unsigned)loraDiagnostics.lastVersion,
                     (unsigned)loraDiagnostics.failedRegister, (unsigned)loraDiagnostics.expectedValue,
                     (unsigned)loraDiagnostics.actualValue);
        else
            snprintf(line, sizeof(line), "SEL:N%u CHIP:OK", (unsigned)selectedNode + 1U);
    }
    LCD2004_PrintLine(lcdRow, line);
    lcdRow = (uint8_t)((lcdRow + 1U) % 4U);
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init(); MX_I2C1_Init(); MX_SPI1_Init(); MX_USART1_UART_Init();
    masterSession = BootCounter_Next();
    if (!masterSession) masterBootFlashError = HAL_FLASH_GetError();
    __HAL_UART_CLEAR_OREFLAG(&huart1);
    HAL_NVIC_SetPriority(USART1_IRQn, 1U, 0U);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
    __HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);
    LCD2004_Init(&hi2c1);
    if (masterSession) radioReady = LoRa_Init();
    uint32_t now = HAL_GetTick();
    nextRadioRecovery = now + RADIO_RECOVERY_MS;
    unsigned i;
    for (i = 0; i < NODE_COUNT; ++i) nodes[i].nextPoll = now + i * 400U;
    nextTelemetry = now;
    nextLCD = now;
    while (1) {
        RadioProcess();
        UARTProcess();
        ButtonsProcess();
        TelemetryProcess();
        LCDProcess();
        HAL_Delay(1U);
    }
}
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    RCC_OscInitStruct.OscillatorType =
        RCC_OSCILLATORTYPE_HSE;

    RCC_OscInitStruct.HSEState =
        RCC_HSE_ON;

    RCC_OscInitStruct.HSEPredivValue =
        RCC_HSE_PREDIV_DIV1;

    RCC_OscInitStruct.HSIState =
        RCC_HSI_ON;

    RCC_OscInitStruct.PLL.PLLState =
        RCC_PLL_ON;

    RCC_OscInitStruct.PLL.PLLSource =
        RCC_PLLSOURCE_HSE;

    RCC_OscInitStruct.PLL.PLLMUL =
        RCC_PLL_MUL9;

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType =
        RCC_CLOCKTYPE_HCLK |
        RCC_CLOCKTYPE_SYSCLK |
        RCC_CLOCKTYPE_PCLK1 |
        RCC_CLOCKTYPE_PCLK2;

    RCC_ClkInitStruct.SYSCLKSource =
        RCC_SYSCLKSOURCE_PLLCLK;

    RCC_ClkInitStruct.AHBCLKDivider =
        RCC_SYSCLK_DIV1;

    RCC_ClkInitStruct.APB1CLKDivider =
        RCC_HCLK_DIV2;

    RCC_ClkInitStruct.APB2CLKDivider =
        RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct,
                            FLASH_LATENCY_2) != HAL_OK)
    {
        Error_Handler();
    }
}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{
    hi2c1.Instance =
        I2C1;

    hi2c1.Init.ClockSpeed =
        100000;

    hi2c1.Init.DutyCycle =
        I2C_DUTYCYCLE_2;

    hi2c1.Init.OwnAddress1 =
        0;

    hi2c1.Init.AddressingMode =
        I2C_ADDRESSINGMODE_7BIT;

    hi2c1.Init.DualAddressMode =
        I2C_DUALADDRESS_DISABLE;

    hi2c1.Init.OwnAddress2 =
        0;

    hi2c1.Init.GeneralCallMode =
        I2C_GENERALCALL_DISABLE;

    hi2c1.Init.NoStretchMode =
        I2C_NOSTRETCH_DISABLE;

    if (HAL_I2C_Init(&hi2c1) != HAL_OK)
    {
        Error_Handler();
    }
}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{
    hspi1.Instance =
        SPI1;

    hspi1.Init.Mode =
        SPI_MODE_MASTER;

    hspi1.Init.Direction =
        SPI_DIRECTION_2LINES;

    hspi1.Init.DataSize =
        SPI_DATASIZE_8BIT;

    hspi1.Init.CLKPolarity =
        SPI_POLARITY_LOW;

    hspi1.Init.CLKPhase =
        SPI_PHASE_1EDGE;

    hspi1.Init.NSS =
        SPI_NSS_SOFT;

    hspi1.Init.BaudRatePrescaler =
        SPI_BAUDRATEPRESCALER_16;

    hspi1.Init.FirstBit =
        SPI_FIRSTBIT_MSB;

    hspi1.Init.TIMode =
        SPI_TIMODE_DISABLE;

    hspi1.Init.CRCCalculation =
        SPI_CRCCALCULATION_DISABLE;

    hspi1.Init.CRCPolynomial =
        10;

    if (HAL_SPI_Init(&hspi1) != HAL_OK)
    {
        Error_Handler();
    }
}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{
    huart1.Instance =
        USART1;

    huart1.Init.BaudRate =
        115200;

    huart1.Init.WordLength =
        UART_WORDLENGTH_8B;

    huart1.Init.StopBits =
        UART_STOPBITS_1;

    huart1.Init.Parity =
        UART_PARITY_NONE;

    huart1.Init.Mode =
        UART_MODE_TX_RX;

    huart1.Init.HwFlowCtl =
        UART_HWCONTROL_NONE;

    huart1.Init.OverSampling =
        UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart1) != HAL_OK)
    {
        Error_Handler();
    }
}


static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef cfg = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOB_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOB, LORA_NSS_Pin | LORA_RST_Pin, GPIO_PIN_SET);
    cfg.Pin = BTN_NODE1_Pin | BTN_NODE2_Pin;
    cfg.Mode = GPIO_MODE_INPUT; cfg.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOB, &cfg);
    cfg.Pin = LORA_NSS_Pin | LORA_RST_Pin;
    cfg.Mode = GPIO_MODE_OUTPUT_PP; cfg.Pull = GPIO_NOPULL;
    cfg.Speed = GPIO_SPEED_FREQ_HIGH; HAL_GPIO_Init(GPIOB, &cfg);
    cfg.Pin = LORA_DIO0_Pin; cfg.Mode = GPIO_MODE_IT_RISING; cfg.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &cfg);
    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 2U, 0U);
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    if (pin == LORA_DIO0_Pin) { dio0InterruptCount++; LoRa_OnDio0Interrupt(); }
}
void Error_Handler(void)
{
    __disable_irq();
    while (1) { /* no automatic relay command on an infrastructure fault */ }
}
#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) { (void)file; (void)line; }
#endif
