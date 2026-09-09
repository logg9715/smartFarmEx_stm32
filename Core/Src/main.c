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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef struct
{
    int16_t temp;
    uint16_t humi;
    uint16_t light;
} frame_uart_t;

typedef struct
{
	GPIO_TypeDef *const port;
	uint16_t pin;
} water_led_t;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define SENSOR_INTERVAL_MS 1000

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */



/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

I2C_HandleTypeDef hi2c1;

UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */
#define CMD_WATER  0x20

static volatile uint8_t rx_byte;
static volatile uint8_t g_cmd_water_sec = 0;   /* 0이면 명령 없음 */

static const water_led_t g_led_list[] =
{
    { .port = WATER_1_GPIO_Port, .pin = WATER_1_Pin },
    { .port = WATER_2_GPIO_Port, .pin = WATER_2_Pin },
    { .port = WATER_3_GPIO_Port, .pin = WATER_3_Pin },
    { .port = WATER_4_GPIO_Port, .pin = WATER_4_Pin },
};

#define LED_COUNT  (int)(sizeof(g_led_list) / sizeof(g_led_list[0]))
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_ADC1_Init(void);
/* USER CODE BEGIN PFP */
static uint16_t Read_LDR(void);
static uint16_t Read_LDR_Filtered(void);
//static float LDR_Resistance(uint16_t);
static void Task_Sensor(uint32_t);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

static uint16_t Read_LDR(void)
{
	HAL_ADC_Start(&hadc1);
	HAL_ADC_PollForConversion(&hadc1, HAL_MAX_DELAY);
	uint16_t raw = HAL_ADC_GetValue(&hadc1);
	HAL_ADC_Stop(&hadc1);
	return raw;
}

static uint16_t Read_LDR_Filtered(void)
{
	const int SAMPLE_COUNT = 8;
	uint32_t sum = 0;
	for(int i = 0; i < SAMPLE_COUNT; i++)
	{
		sum += Read_LDR();
		HAL_Delay(2);
	}
	return (uint16_t)(sum/ SAMPLE_COUNT);
}

//static float LDR_Resistance(uint16_t raw)
//{
//	if(raw == 0)
//		return 1e9f;
//
//	return 10000.0f * (4095.0f - raw) / raw;
//}

static void Task_Sensor(uint32_t now)
{
	static uint32_t last_ms = 0;
	static uint8_t cmd[2] = {0x2C, 0x06};

	if(now - last_ms < SENSOR_INTERVAL_MS)
		return;
	last_ms = now;

	uint8_t frame[100];
	uint8_t data[6];
	frame_uart_t frame_uart;

	// 1. 측정명령 전송
	HAL_I2C_Master_Transmit(&hi2c1, 0x44 << 1, cmd, 2, 100);
	HAL_Delay(20);

	// 2. 데이터 6바이트 받기
	HAL_I2C_Master_Receive(&hi2c1, 0x44 << 1, data, 6, 100);

	// 3. raw값 조합
	// 데이터시트 10페이지 table9참조
	// 8비트 8비트 나눠서 옴 >> 8비트 밀고 두 개 합쳐서 16비트로 조합
	uint16_t raw_temp = (data[0] << 8) | data[1];
	uint16_t raw_humi = (data[3] << 8) | data[4];

	// 4. 실제 값 연산
	float temp = -45 + 175 * ((float)raw_temp / 65535.0);
	float humi = 100 * ((float)raw_humi / 65535.0);

	// 4-1 . 조도센서 측정
	uint16_t light = Read_LDR_Filtered();

	// 5. 전송용 프레임 만들기 [!!!중요!!!] 100만큼 스케일링 함
	frame_uart.temp = (uint16_t)(temp * 100.0f);
	frame_uart.humi = (uint16_t)(humi * 100.0f);
	frame_uart.light = light;
	size_t frame_len = sizeof(frame_uart);

	frame[0] = 0x02;
	frame[1] = frame_len;
	frame[2] = 0x10;
	memcpy(&frame[3], &frame_uart, frame_len);
	frame[3 + frame_len] = 0x00;
	frame[4 + frame_len] = 0x03;

	// # 라즈베리파이 직결 포트
	if(HAL_UART_Transmit(&huart1, frame, frame_len + 5, 100) != HAL_OK)
	{
		// TODO err count
	}

	// 하트비트 둘지 없앨지?
	HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
}

static void Task_WaterPump(uint32_t now, int order)
{
	const uint32_t LED_MS = 150;

	static int working_step = 0;
	static uint32_t end_tm = 0;
	static uint32_t last_ms = 0;
	uint32_t left_ms = now - last_ms;

	// # 명령 최초하달 init
	if(order > 0)
	{
		for(int i = 0; i < LED_COUNT; i++)
			HAL_GPIO_WritePin(g_led_list[i].port, g_led_list[i].pin, GPIO_PIN_RESET);
		working_step = 0;
		HAL_GPIO_WritePin(g_led_list[working_step].port, g_led_list[working_step].pin, GPIO_PIN_SET);
		last_ms = now;
		end_tm = now + (uint32_t)order * 1000;
	}

	// # 지속시간 이외
	if(end_tm < now)
	{
		if (working_step >=0)
		{
			for(int i = 0; i < LED_COUNT; i++)
				HAL_GPIO_WritePin(g_led_list[i].port, g_led_list[i].pin, GPIO_PIN_RESET);
			working_step = -1;
		}
		return;
	}

	if(left_ms < LED_MS)
		return;

	last_ms = now;
	HAL_GPIO_WritePin(g_led_list[working_step].port, g_led_list[working_step].pin, GPIO_PIN_RESET);
	working_step = (working_step + 1) % LED_COUNT;
	HAL_GPIO_WritePin(g_led_list[working_step].port, g_led_list[working_step].pin, GPIO_PIN_SET);
}

/* 프레임구조: 02 | len | type | payload | 00 | 03 */
static void cmd_feed(uint8_t b)
{
	static uint8_t st = 0;
	static uint8_t len, type, payload;

	switch (st)
	{
	case 0: st = (b == 0x02) ? 1 : 0; break;	/* STX */
	case 1: len  = b; st = (len == 1) ? 2 : 0;	break;	/* 길이 */
	case 2: type = b; st = 3;	break;	/* 타입 */
	case 3: payload = b; st = 4;	break;	/* 값 */
	case 4: st = (b == 0x00) ? 5 : 0;	break;	/* 예약 */
	case 5:	/* ETX */
		if (b == 0x03 && type == CMD_WATER && payload > 0 && payload <= 30)
			g_cmd_water_sec = payload;
		st = 0;
		break;
	default:
		st = 0;
		break;
	}
}
// 오버라이드
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
	if (huart->Instance == USART1)
	{
		cmd_feed(rx_byte);
		HAL_UART_Receive_IT(&huart1, (uint8_t *)&rx_byte, 1);   /* 재장전 */
	}
}
// 오버라이드
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	if (huart->Instance == USART1)
		HAL_UART_Receive_IT(&huart1, (uint8_t *)&rx_byte, 1);   /* 복구 */
}


/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_USART1_UART_Init();
  MX_ADC1_Init();
  /* USER CODE BEGIN 2 */
  HAL_UART_Receive_IT(&huart1, (uint8_t *)&rx_byte, 1);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	uint32_t now = HAL_GetTick();

	__disable_irq();
	int order = g_cmd_water_sec;
	g_cmd_water_sec = 0;
	__enable_irq();

	Task_Sensor(now);
	Task_WaterPump(now, order);

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

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 100;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
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

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_10;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_3CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
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
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, LED_Pin|WATER_4_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, WATER_3_Pin|WATER_2_Pin|WATER_1_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : LED_Pin WATER_4_Pin */
  GPIO_InitStruct.Pin = LED_Pin|WATER_4_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : WATER_3_Pin WATER_2_Pin WATER_1_Pin */
  GPIO_InitStruct.Pin = WATER_3_Pin|WATER_2_Pin|WATER_1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
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
