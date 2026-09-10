# smartFarmEx_stm32
스마트팜 시스템의 STM32 펌웨어. 센서 측정+액추에이터 동작, UART 프레임으로 라즈베리파이(farmd)와 통신
 
| | |
|---|---|
| 보드 | NUCLEO-F411RE (STM32F411RE) |
| 개발 환경 | STM32CubeIDE / HAL |
| 리눅스 데몬 | [smartFarmEx](https://github.com/logg9715/smartFarmEx) (`farmd`) |
 
## 구현 특징
- `HAL_GetTick()` 기반 스케줄러로, 센서 송신과 액추에이터 동작을 동시 진행
- ISR은 플래그만 세우고 실제 동작은 메인 루프가 수행
- 액추에이터 자동 차단 타이머를 MCU에 배치 (AP 정지 시에도 정지 보장)
 
## 1. 핀 맵
 
| 기능 | 핀 | 비고 |
|---|---|---|
| SHT30 (I2C1 SCL) | PB8 | 슬레이브 주소 `0x44` |
| SHT30 (I2C1 SDA) | PB9 | |
| GL5528 조도 (ADC1_IN10) | PC0 | 12bit, 8회 평균 |
| UART TX (USART1) | PA9 | → 라즈베리파이 RX |
| UART RX (USART1) | PA10 | ← 라즈베리파이 TX |
| 액추에이터 1 | PB5 | |
| 액추에이터 2 | PB4 | |
| 액추에이터 3 | PB10 | |
| 액추에이터 4 | PA8 | |
| 하트비트 LED | PA5 | 보드 LD2 |
 
> 액추에이터는 워터펌프 대신 LED 4개로 대체 시연한다. 릴레이를 연결하면 워터펌프 등으로 확장 가능할 것.
 
보드 클럭 설정 : HSI + PLL, SYSCLK 100 MHz

## 2. 태스크 구조
 
```c
while (1)
{
    uint32_t now = HAL_GetTick();
 
    __disable_irq();
    int order = g_cmd_water_sec;    /* ISR이 세운 명령 플래그 소비 */
    g_cmd_water_sec = 0;
    __enable_irq();
 
    Task_Sensor(now);
    Task_WaterPump(now, order);
}
```
 
| 태스크 | 주기 | 동작 |
|---|---|---|
| `Task_Sensor` | 1000 ms | SHT30 측정 -> 조도 ADC -> 프레임 조립 -> UART 송신 |
| `Task_WaterPump` | 150 ms | 명령 수신 시 LED 순차 점등, 종료 시각 도달 시 소등 |
 
각 태스크는 자기 차례가 아니면 즉시 반환

## 3. UART 프로토콜
 
### 3.1 프레임 레이아웃
 
| 인덱스 | Size | Field | Value |
|---|---|---|---|
| 0 | 1 | STX | `0x02` |
| 1 | 1 | LEN | payload 바이트 수 |
| 2 | 1 | TYPE | `0x10` 센서 / `0x20` 급수 명령 |
| 3 | LEN | PAYLOAD | |
| 3+LEN | 1 | RESERVED | `0x00` |
| 4+LEN | 1 | ETX | `0x03` |
 
양방향 동일 구조. 총 길이 = LEN + 5.
회선 설정: 115200 8N1
 
### 3.2 페이로드
 
**CASE : TYPE `0x10` — 센서 (STM32 to Pi, LEN = 6, total 11 B)**
 
| 인덱스 | Type | Field | Value |
|---|---|---|---|
| 0 | `int16` | temp | ×100 (℃) |
| 2 | `uint16` | humi | ×100 (%) |
| 4 | `uint16` | light | ADC raw값 12bit |
 
부동소수점 대신, x100한 고정소수점 정수를 전송함. 

**CASE : TYPE `0x20` — 급수 명령 (Pi to STM32, LEN = 1, total 6 B)**
 
| 인덱스 | Type | Field | 값 범위 |
|---|---|---|---|
| 0 | `uint8` | sec | 1–30(farmd에서 설정값) |
 
명령을 "켜라"가 아니라, "N초간 켜라"로 정의 : 차단 타이머가 MCU에 있으므로 AP가 멈추거나 통신이 끊겨도 액추에이터가 정지한다.
 
### 3.3 수신 상태 기계
 
`HAL_UART_Receive_IT` 로 1바이트씩 수신하고, 콜백에서 바이트 단위로 프레임을 재조립한다.
 
```
STX -> LEN -> TYPE -> PAYLOAD -> RESERVED -> ETX
                                        // 검증 통과 시, 명령 플래그 설정
 // 어느 단계든 값이 어긋나면 state 0으로 복귀
```

ISR과 메인 루프의 공유 플래그는 `volatile` 로 선언하고, 읽기와 초기화를 `__disable_irq()` / `__enable_irq()`로 보장 (그 사이에 인터럽트가 들어오면 새로 도착한 명령이 유실됨)
 
## 4. 센서
 
**SHT30 (I2C)**
 
```
// 데이터시트 참조한 수식
0x2C 0x06 전송 (고반복 단발 측정)  →  20 ms 대기  →  6바이트 수신
temp = -45 + 175 × raw / 65535
humi = 100 × raw / 65535
```
 
**GL5528 조도 (ADC)**
 
ADC 원시값을 그대로 전송하고 저항/조도 환산은 수행하지 않음.

 
