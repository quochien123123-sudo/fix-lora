#ifndef LORA_TEST_HAL_MOCK_H
#define LORA_TEST_HAL_MOCK_H
/* Define the real main.h guard before including the unmodified radio driver.
 * Only HAL operations used by this driver are modeled. */
#define __MAIN_H
#include <stdint.h>
typedef struct { int unused; } SPI_HandleTypeDef;
typedef struct { int unused; } GPIO_TypeDef;
typedef enum { HAL_OK=0, HAL_ERROR=1, HAL_BUSY=2, HAL_TIMEOUT=3 } HAL_StatusTypeDef;
typedef enum { GPIO_PIN_RESET=0, GPIO_PIN_SET=1 } GPIO_PinState;
extern GPIO_TypeDef mock_nss_port, mock_rst_port;
#define LORA_NSS_GPIO_Port (&mock_nss_port)
#define LORA_RST_GPIO_Port (&mock_rst_port)
#define LORA_NSS_Pin 1U
#define LORA_RST_Pin 2U
uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t);
void HAL_GPIO_WritePin(GPIO_TypeDef *, uint16_t, GPIO_PinState);
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *, uint8_t *, uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *, uint8_t *, uint16_t, uint32_t);
#endif
