/* Host stand-in for Core/Inc/delay.h (DWT cycle-counter delays). */
#ifndef HOST_DELAY_H
#define HOST_DELAY_H
#include "stm32f7xx_hal.h"
static inline void DWT_Delay_us(uint32_t us) { (void)us; }
#endif
