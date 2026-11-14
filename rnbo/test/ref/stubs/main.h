/* Host stand-in for Core/Inc/main.h: only the jack-detect GPIOs the mixer reads. */
#ifndef HOST_MAIN_H
#define HOST_MAIN_H
#include "stm32f7xx_hal.h"
extern GPIO_TypeDef host_gpio;
#define switch_l1_Pin 1
#define switch_l1_GPIO_Port (&host_gpio)
#define switch_r1_Pin 2
#define switch_r1_GPIO_Port (&host_gpio)
#define switch_l2_Pin 4
#define switch_l2_GPIO_Port (&host_gpio)
#define switch_r2_Pin 8
#define switch_r2_GPIO_Port (&host_gpio)
extern TIM_HandleTypeDef htim2;
#endif
