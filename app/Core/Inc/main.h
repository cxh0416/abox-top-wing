/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define PVIN_OUT1_Pin GPIO_PIN_0
#define PVIN_OUT1_GPIO_Port GPIOA
#define PVIN_OUT2_Pin GPIO_PIN_1
#define PVIN_OUT2_GPIO_Port GPIOA
#define PVIN_OUT3_Pin GPIO_PIN_2
#define PVIN_OUT3_GPIO_Port GPIOA
#define IO_IN1_Pin GPIO_PIN_4
#define IO_IN1_GPIO_Port GPIOA
#define IO_IN2_Pin GPIO_PIN_5
#define IO_IN2_GPIO_Port GPIOA
#define IO_IN3_Pin GPIO_PIN_6
#define IO_IN3_GPIO_Port GPIOA
#define IO_IN4_Pin GPIO_PIN_7
#define IO_IN4_GPIO_Port GPIOA
#define RELAY_CTL_Pin GPIO_PIN_4
#define RELAY_CTL_GPIO_Port GPIOC
#define LED_1_Pin GPIO_PIN_0
#define LED_1_GPIO_Port GPIOB
#define LED_2_Pin GPIO_PIN_1
#define LED_2_GPIO_Port GPIOB
#define LED_3_Pin GPIO_PIN_2
#define LED_3_GPIO_Port GPIOB
#define CAN2_STB_Pin GPIO_PIN_14
#define CAN2_STB_GPIO_Port GPIOB
#define CAN1_STB_Pin GPIO_PIN_15
#define CAN1_STB_GPIO_Port GPIOB
#define EN_3_8V_Pin GPIO_PIN_7
#define EN_3_8V_GPIO_Port GPIOC
#define EC_PWR_Pin GPIO_PIN_8
#define EC_PWR_GPIO_Port GPIOC
#define EC_DTR_Pin GPIO_PIN_9
#define EC_DTR_GPIO_Port GPIOC
#define EC_MAIN_RI_Pin GPIO_PIN_8
#define EC_MAIN_RI_GPIO_Port GPIOA

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
