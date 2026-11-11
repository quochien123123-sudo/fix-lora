#include "hal_mock.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef DRIVER_SOURCE
#error "Compile with DRIVER_SOURCE set to the radio source path"
#endif
#include DRIVER_SOURCE

SPI_HandleTypeDef hspi1;
GPIO_TypeDef mock_nss_port, mock_rst_port;
static uint8_t registers[128], fifo[256], chip_version=0x12;
static uint32_t tick;
static unsigned selected, spi_failures_remaining;
static int corrupt_readback_address=-1;

uint32_t HAL_GetTick(void) { return tick; }
void HAL_Delay(uint32_t delay) { tick += delay; }
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState value)
{
    if (port==LORA_NSS_GPIO_Port && pin==LORA_NSS_Pin) selected=value==GPIO_PIN_RESET;
    else if (port==LORA_RST_GPIO_Port && pin==LORA_RST_Pin && value==GPIO_PIN_SET) {
        memset(registers,0,sizeof(registers));
        memset(fifo,0,sizeof(fifo));
        registers[REG_VERSION]=chip_version;
    }
}
static HAL_StatusTypeDef spi_status(void)
{
    assert(selected);
    if (spi_failures_remaining) { --spi_failures_remaining; return HAL_ERROR; }
    return HAL_OK;
}
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *handle, uint8_t *tx,
                                        uint8_t *rx, uint16_t count, uint32_t timeout)
{
    (void)handle; (void)timeout;
    HAL_StatusTypeDef status=spi_status();
    if (status!=HAL_OK) return status;
    assert(count>=2 && !(tx[0]&0x80U));
    memset(rx,0,count);
    uint8_t address=tx[0]&0x7f;
    if (address==REG_FIFO) {
        for (unsigned i=1; i<count; ++i) rx[i]=fifo[registers[REG_FIFO_PTR]++];
    } else {
        assert(count==2);
        rx[1]=registers[address];
        if ((int)address==corrupt_readback_address) rx[1]^=1U;
    }
    return HAL_OK;
}
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *handle, uint8_t *data,
                                 uint16_t count, uint32_t timeout)
{
    (void)handle; (void)timeout;
    HAL_StatusTypeDef status=spi_status();
    if (status!=HAL_OK) return status;
    assert(count>=2 && (data[0]&0x80U));
    uint8_t address=data[0]&0x7f;
    if (address==REG_FIFO) {
        for (unsigned i=1; i<count; ++i) fifo[registers[REG_FIFO_PTR]++]=data[i];
    } else {
        assert(count==2);
        if (address==REG_IRQ) registers[address]&=(uint8_t)~data[1]; /* W1C */
        else registers[address]=data[1];
    }
    return HAL_OK;
}
static void init_good(void)
{
    assert(LoRa_Init()==1);
    assert(LoRa_IsReady() && !LoRa_IsBusy());
    assert(registers[REG_MODE]==MODE_RX);
    assert(registers[REG_CONFIG1]==0x72 && registers[REG_CONFIG2]==0x74);
    assert(registers[REG_CONFIG3]==0x04 && registers[REG_SYNC]==0x12);
    assert(registers[REG_FRF_MSB]==0x6c && registers[REG_FRF_MID]==0x40 && registers[REG_FRF_LSB]==0);
#ifdef TEST_DIAGNOSTICS
    assert(loraDiagnostics.initAttempts==1 && loraDiagnostics.initSuccesses==1);
    assert(loraDiagnostics.initStage==LORA_INIT_READY && loraDiagnostics.lastError==LORA_ERROR_NONE);
    assert(loraDiagnostics.lastVersion==0x12);
#endif
}
static void inject_rx(uint8_t flags, uint8_t crc_present)
{
    const uint8_t payload[]={0x4c,0x50,1,0xaa,0x55};
    registers[REG_RX_CURRENT]=16;
    registers[REG_RX_BYTES]=sizeof(payload);
    memcpy(fifo+16,payload,sizeof(payload));
    registers[REG_HOP_CHANNEL]=crc_present?0x40:0;
    registers[REG_SNR]=(uint8_t)-8; /* -2 dB */
    registers[REG_RSSI]=100;
    registers[REG_IRQ]=flags;
    tick+=2;
    LoRa_Process();
}
static void test_init(void) { init_good(); }
static void test_bad_version(uint8_t version)
{
    chip_version=version;
    assert(!LoRa_Init());
    assert(!LoRa_IsReady());
#ifdef TEST_DIAGNOSTICS
    assert(loraDiagnostics.initAttempts==1 && loraDiagnostics.initSuccesses==0);
    assert(loraDiagnostics.lastError==LORA_ERROR_VERSION);
    assert(loraDiagnostics.failedRegister==REG_VERSION && loraDiagnostics.lastVersion==version);
#endif
}
static void test_rx(void)
{
    uint8_t out[16]={0};
    init_good(); inject_rx(IRQ_RX_DONE,1);
    assert(LoRa_ReadPacket(out,sizeof(out))==5);
    assert(memcmp(out,(uint8_t[]){0x4c,0x50,1,0xaa,0x55},5)==0);
    assert(LoRa_GetLastSNR()==-8 && LoRa_GetLastRSSI()==-66);
    assert(registers[REG_IRQ]==0);
    assert(LoRa_ReadPacket(out,sizeof(out))==0);
#ifdef TEST_DIAGNOSTICS
    assert(loraDiagnostics.rxAccepted==1);
#endif
}
static void test_bad_crc(void)
{
    uint8_t out[16];
    init_good(); inject_rx(IRQ_RX_DONE|IRQ_CRC_ERROR,1);
    assert(!LoRa_ReadPacket(out,sizeof(out)));
    assert(loraCrcErrorCount==1 && LoRa_IsReady());
    assert(registers[REG_IRQ]==0);
}
static void test_missing_crc(void)
{
    uint8_t out[16];
    init_good(); inject_rx(IRQ_RX_DONE,0);
    assert(!LoRa_ReadPacket(out,sizeof(out)));
    assert(loraCrcErrorCount==1 && LoRa_IsReady());
}
static void start_tx(void)
{
    const uint8_t payload[]={9,8,7};
    init_good();
    assert(LoRa_SendAsync(payload,sizeof(payload))==1);
    assert(LoRa_IsBusy() && LoRa_TxResult()==0);
    assert(registers[REG_MODE]==MODE_TX && registers[REG_PAYLOAD_LENGTH]==3);
    assert(memcmp(fifo,payload,3)==0);
    assert(!LoRa_Init()); /* live transaction cannot be silently cancelled */
}
static void test_tx_done(uint32_t elapsed)
{
    start_tx();
    registers[REG_IRQ]=IRQ_TX_DONE;
    registers[REG_MODE]=MODE_STANDBY; /* hardware finishes TX automatically */
    tick+=elapsed;
    LoRa_Process();
    if (!(LoRa_IsReady() && !LoRa_IsBusy() && LoRa_TxResult()==1 &&
          registers[REG_MODE]==MODE_RX && loraTxTimeoutCount==0)) {
        fprintf(stderr,"TxDone lost after %lu ms: ready=%u busy=%u result=%d timeouts=%lu\n",
                (unsigned long)elapsed,LoRa_IsReady(),LoRa_IsBusy(),LoRa_TxResult(),
                (unsigned long)loraTxTimeoutCount);
        exit(1);
    }
    assert(!(registers[REG_IRQ]&IRQ_TX_DONE));
#ifdef TEST_DIAGNOSTICS
    assert(loraDiagnostics.txAccepted==1 && loraDiagnostics.txDone==1);
    assert(loraDiagnostics.lastError==LORA_ERROR_NONE);
#endif
}
static void test_tx_timeout(void)
{
    start_tx(); tick+=501; LoRa_Process();
    assert(!LoRa_IsReady() && !LoRa_IsBusy() && LoRa_TxResult()==-1);
    assert(loraTxTimeoutCount==1 && registers[REG_MODE]==MODE_STANDBY);
#ifdef TEST_DIAGNOSTICS
    assert(loraDiagnostics.lastError==LORA_ERROR_TX_TIMEOUT);
    assert(loraDiagnostics.failedRegister==REG_IRQ && loraDiagnostics.actualValue==0);
#endif
}
static void test_spi_fault(void)
{
    init_good(); spi_failures_remaining=1; tick+=2; LoRa_Process();
    assert(!LoRa_IsReady() && LoRa_TxResult()==-1);
    assert(loraSpiErrorCount==1 && !selected);
#ifdef TEST_DIAGNOSTICS
    assert(loraDiagnostics.lastError==LORA_ERROR_SPI_READ && loraDiagnostics.failedRegister==REG_IRQ);
#endif
}
static void test_pending_rx_blocks_tx(void)
{
    uint8_t payload=7, out[16];
    init_good(); inject_rx(IRQ_RX_DONE,1);
    assert(!LoRa_SendAsync(&payload,1));
    assert(LoRa_ReadPacket(out,sizeof(out))==5);
    assert(LoRa_SendAsync(&payload,1));
}
static void test_receiving_blocks_tx(void)
{
    uint8_t payload=7;
    init_good(); registers[REG_MODEM_STATUS]=0x04;
    assert(!LoRa_SendAsync(&payload,1));
    assert(registers[REG_MODE]==MODE_RX && !LoRa_IsBusy());
}
static void test_brownout(void)
{
    init_good(); registers[REG_CONFIG1]=0; tick+=1001; LoRa_Process();
    assert(!LoRa_IsReady());
#ifdef TEST_DIAGNOSTICS
    assert(loraDiagnostics.lastError==LORA_ERROR_HEALTH && loraDiagnostics.failedRegister==REG_CONFIG1);
    assert(loraDiagnostics.expectedValue==0x72 && loraDiagnostics.actualValue==0);
#endif
}
static void test_readback_fault(void)
{
    corrupt_readback_address=REG_CONFIG1;
    assert(!LoRa_Init() && !LoRa_IsReady());
#ifdef TEST_DIAGNOSTICS
    assert(loraDiagnostics.lastError==LORA_ERROR_READBACK && loraDiagnostics.failedRegister==REG_CONFIG1);
    assert(loraDiagnostics.expectedValue==0x72 && loraDiagnostics.actualValue==0x73);
#endif
}
static void test_init_spi_fault(void)
{
    spi_failures_remaining=1;
    assert(!LoRa_Init() && !LoRa_IsReady());
#ifdef TEST_DIAGNOSTICS
    assert(loraDiagnostics.lastError==LORA_ERROR_SPI_READ && loraDiagnostics.failedRegister==REG_VERSION);
#endif
}
static void test_wrap_timeout(void)
{
    tick=UINT32_MAX-100U;
    test_tx_timeout(); /* elapsed subtraction must work across HAL tick rollover */
    assert(tick<1000U);
}
int main(int argc, char **argv)
{
    assert(argc==2);
    if (!strcmp(argv[1],"init")) test_init();
    else if (!strcmp(argv[1],"version_zero")) test_bad_version(0);
    else if (!strcmp(argv[1],"version_ff")) test_bad_version(0xff);
    else if (!strcmp(argv[1],"rx")) test_rx();
    else if (!strcmp(argv[1],"crc_error")) test_bad_crc();
    else if (!strcmp(argv[1],"crc_missing")) test_missing_crc();
    else if (!strcmp(argv[1],"tx_done")) test_tx_done(80);
    else if (!strcmp(argv[1],"delayed_tx_done")) test_tx_done(501);
    else if (!strcmp(argv[1],"tx_timeout")) test_tx_timeout();
    else if (!strcmp(argv[1],"spi_fault")) test_spi_fault();
    else if (!strcmp(argv[1],"pending_rx_blocks_tx")) test_pending_rx_blocks_tx();
    else if (!strcmp(argv[1],"receiving_blocks_tx")) test_receiving_blocks_tx();
    else if (!strcmp(argv[1],"brownout")) test_brownout();
    else if (!strcmp(argv[1],"readback_fault")) test_readback_fault();
    else if (!strcmp(argv[1],"init_spi_fault")) test_init_spi_fault();
    else if (!strcmp(argv[1],"wrap_timeout")) test_wrap_timeout();
    else { fprintf(stderr,"Unknown test %s\n",argv[1]); return 2; }
    printf("PASS %s\n",argv[1]);
    return 0;
}
