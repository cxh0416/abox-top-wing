/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "can.h"
#include "dma.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "boot_cfg.h"
#include "app_version.h"
#include "app_log.h"
#include "abox_product_port.h"
#include "board_config.h"
#include "boot_state.h"
#include "ec800_at.h"
#include "ec_power.h"
#include "mqtt_app.h"
#include "mqtt_core.h"
#include "ota_service.h"
#include "time_base.h"
#include "wing_controller.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static MQTT_CoreConfig_t g_mqtt_cfg = {0};
static Ec800AtCore_t g_ec800_at;

#define UART1_RX_BUF_SZ 512
uint8_t uart1_rx_buf[UART1_RX_BUF_SZ];

static uint8_t g_mqtt_start_enable = 0;
static uint32_t g_ec_restart_tick = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
int fputc(int ch, FILE* f)
{
    (void)ch;
    (void)f;
    return ch;
}

static char g_client_id[32];

const char g_top_flying_wing_fw_version_marker[] = TOP_FLYING_WING_APP_FW_VERSION_MARKER;

const char* TopFlyingWing_AppFwVersion(void)
{
    return TOP_FLYING_WING_APP_FW_VERSION;
}

static void GenClientId(void)
{
    uint32_t uid0 = *(uint32_t*)0x1FFFF7E8;
    uint32_t uid1 = *(uint32_t*)0x1FFFF7EC;
    uint32_t uid2 = *(uint32_t*)0x1FFFF7F0;

    snprintf(g_client_id, sizeof(g_client_id), "dev_%08" PRIX32 "%08" PRIX32 "%08" PRIX32, uid0, uid1, uid2);
}

static void MQTT_ConfigInit(void)
{
    const BoardConfigStore_t* cfg = BoardConfig_Get();

    memset(&g_mqtt_cfg, 0, sizeof(g_mqtt_cfg));

    g_mqtt_cfg.client_idx = 0;

    strncpy(g_mqtt_cfg.server_host, cfg->mqtt_host, sizeof(g_mqtt_cfg.server_host) - 1);
    g_mqtt_cfg.server_port = cfg->mqtt_port;

    strncpy(g_mqtt_cfg.client_id, g_client_id, sizeof(g_mqtt_cfg.client_id) - 1);
    strncpy(g_mqtt_cfg.username, cfg->mqtt_username, sizeof(g_mqtt_cfg.username) - 1);
    strncpy(g_mqtt_cfg.password, cfg->mqtt_password, sizeof(g_mqtt_cfg.password) - 1);

    memset(g_mqtt_cfg.sub_topics, 0, sizeof(g_mqtt_cfg.sub_topics));
    g_mqtt_cfg.sub_topic_count = 0U;
    g_mqtt_cfg.pub_topic[0] = '\0';
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
    SCB->VTOR = APP_START_ADDR;
    __DSB();
    __ISB();
    // __enable_irq();
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
    __enable_irq();
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_CAN1_Init();
  MX_CAN2_Init();
  MX_USART1_UART_Init();
  MX_UART5_Init();
  /* USER CODE BEGIN 2 */

    ABox_ProductPlatform_Init();

    AppLog_Init();
    TimeBase_Init();
    APP_LOGI("APP", "boot");
    APP_LOGI("APP", "fw=%s", TopFlyingWing_AppFwVersion());
    APP_LOGI("APP", "%s", g_top_flying_wing_fw_version_marker);

    EC_Power_Init();
    EC_Power_StartRestartSequence();
    g_ec_restart_tick = HAL_GetTick();
    g_mqtt_start_enable = 0;

    APP_LOGI("EC", "restart sequence");

    HAL_UARTEx_ReceiveToIdle_DMA(&huart1, uart1_rx_buf, UART1_RX_BUF_SZ);
    __HAL_DMA_DISABLE_IT(huart1.hdmarx, DMA_IT_HT);

    BoardConfig_Load();
    AppLog_SetLevel((AppLogLevel_t)BoardConfig_Get()->diag_log_level);
    WingController_Init();

    GenClientId();
    MQTT_ConfigInit();

    EC800_AT_Init(&g_ec800_at, &huart1);
    MQTT_Core_SetAtCore(&g_ec800_at);
    MQTT_Core_Init(&g_mqtt_cfg, MQTT_App_OnMessage);
    OtaService_Init(&g_ec800_at);

    MQTT_App_Init(BoardConfig_Get()->vid);
    MQTT_Core_SetTopics(MQTT_App_GetSubTopics(), MQTT_App_GetSubTopicCount(), MQTT_App_GetHeartbeatTopic());

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
    while (1)
    {
        uint32_t now = HAL_GetTick();
        EC_Power_Task();

        if ((!g_mqtt_start_enable) && ((HAL_GetTick() - g_ec_restart_tick) >= 8000U))
        {
            g_mqtt_start_enable = 1;
            (void)OtaService_BeginProvisioning();
            // HAL_UART_Transmit(&huart5, (uint8_t*)"[APP] mqtt start enable\r\n", 25, 1000);
        }

        if (g_mqtt_start_enable)
        {
            EC800_AT_Task(&g_ec800_at);
            OtaService_Task();
            MQTT_Core_Task();
            MQTT_App_Task();
        }

        (void)now;
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.Prediv1Source = RCC_PREDIV1_SOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  RCC_OscInitStruct.PLL2.PLL2State = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the Systick interrupt time
  */
  __HAL_RCC_PLLI2S_ENABLE();
}

/* USER CODE BEGIN 4 */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef* hcan)
{
    WingController_OnCanRx(hcan);
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* huart, uint16_t Size)
{
    if (huart->Instance == USART1)
    {
        EC800_AT_FeedRx(&g_ec800_at, uart1_rx_buf, Size);

        HAL_UARTEx_ReceiveToIdle_DMA(&huart1, uart1_rx_buf, UART1_RX_BUF_SZ);
        __HAL_DMA_DISABLE_IT(huart1.hdmarx, DMA_IT_HT);
    }
    else { (void)Size; }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef* huart)
{
    if (huart->Instance == USART1)
    {
        APP_LOGE("AT", "USART1 error=0x%08lX state=%u mqtt_st=%u rx_age=%lu pub_age=%lu",
                 (unsigned long)huart->ErrorCode, (unsigned)HAL_UART_GetState(huart),
                 (unsigned)MQTT_Core_GetState(),
                 (unsigned long)MQTT_Core_GetLastRxAgeMs(),
                 (unsigned long)MQTT_Core_GetLastPubOkAgeMs());
    }
}
/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM1 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM1)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
    /* User can add his own implementation to report the HAL error return state */
    __disable_irq();
    while (1) {}
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
    /* User can add his own implementation to report the file name and line number,
       ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
