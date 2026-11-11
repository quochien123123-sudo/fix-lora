#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "protocol.h"
#include "node_state.h"
#include "app_config.h"

static NodeControl control;
static uint8_t radioOK, replyPending, replySending, requestPending;
static uint32_t radioRetryTime, replyQueuedAt;
static ProtoFrame pendingReply, queuedRequest;
static uint8_t sensorMask, humidity, soilPercent, sampleSeen;
static int16_t temperature10;
static uint32_t sampleAt;
volatile uint32_t slaveRxCount, slaveBadPacketCount, slaveTxErrorCount;
volatile uint32_t slaveRequestDropCount, slaveTxDoneCount, slaveAckExpiredCount;
static uint8_t mockRx[PROTO_RF_SIZE], mockRxLength, mockBusy, mockReady;
static int8_t mockTxResult;
static uint8_t sendAllowed, injectOnSend;
static ProtoFrame injectedFrame, sentFrames[16];
static unsigned sentCount, relayWrites;
static void Relay_Write(void) { ++relayWrites; }
static uint8_t LoRa_Init(void) { return mockReady; }
static void LoRa_Process(void) { }
static uint8_t LoRa_IsReady(void) { return mockReady; }
static uint8_t LoRa_IsBusy(void) { return mockBusy; }
static int8_t LoRa_TxResult(void) { return mockTxResult; }
static void inject(const ProtoFrame *p) {
    assert(!mockRxLength);
    mockRxLength = Proto_Encode(mockRx, p);
    assert(mockRxLength == PROTO_RF_SIZE);
}
static uint8_t LoRa_ReadPacket(uint8_t *out, uint8_t cap) {
    uint8_t n = mockRxLength;
    mockRxLength = 0U;
    assert(cap >= n);
    memcpy(out, mockRx, n);
    return n;
}
static uint8_t LoRa_SendAsync(const uint8_t *raw, uint8_t length) {
    if (injectOnSend) {
        injectOnSend = 0U;
        inject(&injectedFrame); /* Driver captures RxDone in its pre-TX check. */
    }
    if (mockRxLength || mockBusy || !sendAllowed) return 0U;
    assert(sentCount < 16U);
    assert(Proto_Decode(&sentFrames[sentCount++], raw, length));
    mockBusy = 1U;
    mockTxResult = 0;
    return 1U;
}
#include "slave_radio.inc"

static ProtoFrame request(uint8_t type, uint32_t seq, uint8_t relay) {
    ProtoFrame q = {0};
    q.type=type; q.node=NODE_ID; q.masterSession=17U; q.seq=seq;
    q.nodeBoot=9U; q.revision=control.revision; q.relay=relay;
    return q;
}
static void reset(void) {
    NodeControl_Init(&control,9U);
    radioOK=mockReady=sendAllowed=1U;
    replyPending=replySending=requestPending=mockRxLength=mockBusy=0U;
    injectOnSend=0U; mockTxResult=0; sentCount=relayWrites=0U;
    slaveRxCount=slaveBadPacketCount=slaveTxErrorCount=0U;
    slaveRequestDropCount=slaveTxDoneCount=slaveAckExpiredCount=0U;
}
static void txDone(void) { mockBusy=0U; mockTxResult=1; }

static void test_rx_during_ack_does_not_block_tx(void) {
    reset();
    ProtoFrame q=request(PROTO_POLL,1U,0U);
    inject(&q);
    injectedFrame=request(PROTO_POLL,2U,0U);
    injectOnSend=1U;
    Radio_Process(10U);
    assert(replyPending && mockRxLength && sentCount==0U);
    Radio_Process(11U);
    assert(!mockRxLength && requestPending && replySending);
    assert(sentCount==1U && sentFrames[0].seq==1U);
    txDone();
    Radio_Process(90U);
    assert(sentCount==2U && sentFrames[1].seq==2U);
    assert(slaveAckExpiredCount==0U && slaveRxCount==2U);
}
static void test_duplicate_set_is_not_applied_twice(void) {
    reset();
    ProtoFrame q=request(PROTO_POLL,1U,0U);
    NodeControl_Handle(&control,&q);
    q=request(PROTO_SET,2U,1U);
    inject(&q);
    injectedFrame=q; injectOnSend=1U;
    Radio_Process(10U);
    assert(control.relay==1U && control.revision==1U && relayWrites==1U);
    Radio_Process(11U);
    assert(requestPending && sentFrames[0].result==PROTO_APPLIED);
    txDone();
    Radio_Process(90U);
    assert(sentCount==2U && sentFrames[1].result==PROTO_DUPLICATE);
    assert(sentFrames[1].seq==q.seq && relayWrites==1U && control.revision==1U);
}
static void test_overflow_never_applies_dropped_set(void) {
    reset(); sendAllowed=0U;
    ProtoFrame q=request(PROTO_POLL,1U,0U); inject(&q); Radio_Process(10U);
    q=request(PROTO_POLL,2U,0U); inject(&q); Radio_Process(11U);
    assert(replyPending && requestPending);
    q=request(PROTO_SET,3U,1U); inject(&q); Radio_Process(12U);
    assert(slaveRequestDropCount==1U && relayWrites==0U && control.relay==0U);
    sendAllowed=1U; Radio_Process(13U); txDone(); Radio_Process(90U);
    assert(sentCount==2U && sentFrames[0].seq==1U && sentFrames[1].seq==2U);
    assert(control.relay==0U && relayWrites==0U);
}
static void test_boot_failure_and_expiry(void) {
    reset(); control.nodeBoot=0U;
    ProtoFrame q=request(PROTO_POLL,1U,0U); inject(&q); Radio_Process(10U);
    assert(slaveRxCount==0U && sentCount==0U);
    reset(); sendAllowed=0U;
    q=request(PROTO_POLL,1U,0U); inject(&q); Radio_Process(10U);
    Radio_Process(211U);
    assert(!replyPending && slaveAckExpiredCount==1U);
}
static void test_stale_boot_set_cannot_change_relay(void) {
    reset(); ProtoFrame q=request(PROTO_POLL,1U,0U); NodeControl_Handle(&control,&q);
    q=request(PROTO_SET,2U,1U); q.nodeBoot=8U; inject(&q); Radio_Process(10U);
    assert(sentCount==1U && sentFrames[0].result==PROTO_BOOT_CHANGED);
    assert(control.relay==0U && relayWrites==0U);
}
int main(void) {
    test_rx_during_ack_does_not_block_tx();
    test_duplicate_set_is_not_applied_twice();
    test_overflow_never_applies_dropped_set();
    test_boot_failure_and_expiry();
    test_stale_boot_set_cannot_change_relay();
    puts("slave queue: 5 regression scenarios passed");
    return 0;
}
