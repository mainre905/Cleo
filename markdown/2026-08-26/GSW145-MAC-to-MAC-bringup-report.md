# STM32H750 ↔ GSW145 MAC-to-MAC 이더넷 브링업 수정 보고서

- **작성일**: 2026-08-26
- **대상 하드웨어**: Cleo SMB (STM32H750VBTX + MaxLinear GSW145)
- **브랜치**: `fix/gsw145-mac-to-mac-bringup`
- **기준 커밋**: `d81a5d2` (lwip added)
- **참조 문서**: `Document/620246_GSW145_DS_Rev1.4.pdf` (Rev 1.4, 2023-11-08)

---

## 1. 배경

STM32H750의 이더넷 MAC을 GSW145 스위치의 **포트 5에 PHY 없이 직결(MAC-to-MAC)** 하는 구성이다.
CubeMX 프로젝트는 PHY를 LAN8742로 잡고 코드를 생성하므로, 생성된 `ethernetif.c`의
PHY 폴링 로직은 이 보드에서 성립하지 않는다.

기존에 동작하던 코드가 있었으나 다음 조건에서만 검증된 상태였다.

- 디버거로 재플래시하는 환경 (MCU만 리셋, 스위치는 전원 유지)
- 특정 PC 한 대 (MAC 주소 하드코딩)
- 짧은 시간의 벤치 테스트

본 작업은 이 코드를 데이터시트 및 HAL 소스와 대조하여 검증하고, 재현 가능한 구조로 재작성한 결과다.

---

## 2. 요약

총 **8건**의 문제를 확인하고 수정했다. 이 중 2건은 코드 리뷰가 아닌 **빌드 검증 과정에서 발견**되었으며,
심각도가 가장 높다.

| # | 항목 | 심각도 | 발견 경로 |
|---|---|---|---|
| 1 | 링커 스크립트에 ETH DMA 섹션 미정의 | 🔴 치명 | 빌드 검증 |
| 2 | lwIP 힙과 RX 버퍼 풀 주소 중첩 | 🔴 치명 | 빌드 검증 |
| 3 | 초기화 순서 — 콜드 부팅 시 실패 | 🔴 높음 | 데이터시트 + HAL 소스 |
| 4 | 포트 5 autopolling이 강제 링크 설정 무효화 | 🔴 높음 | 데이터시트 |
| 5 | `HAL_ETH_ErrorCallback` 죽은 코드 | 🟡 중간 | 코드 리뷰 |
| 6 | RX 하드웨어 체크섬 드롭 | 🟡 중간 | HAL 소스 |
| 7 | IP 주소가 `0.0.0.0` | 🟡 중간 | 코드 리뷰 |
| 8 | `PHY_ADDR_5` 레지스터 값 부작용 | 🟡 중간 | 데이터시트 |

**정량적 성과**: 플래시 사용량 **19,011 바이트 절감** (128KB 중 14.5%)

---

## 3. 치명적 문제

### 3.1 링커 스크립트에 ETH DMA 섹션이 정의되지 않음 🔴

#### 증상

`STM32H750VBTX_FLASH.ld` 및 `STM32H750VBTX_RAM.ld` 어디에도 다음 섹션이 없었다.

- `.RxDecripSection` — RX DMA 디스크립터
- `.TxDecripSection` — TX DMA 디스크립터
- `.Rx_PoolSection` — zero-copy RX pbuf 풀

`ethernetif.c`는 `__attribute__((section(".RxDecripSection")))`으로 배치를 요청하지만,
링커 스크립트에 해당 출력 섹션이 없으면 **orphan section**이 되어 링커가 임의로 배치한다.

#### 검증

원본 링커 스크립트로 실제 링크하여 배치를 확인했다.

```
원본 (orphan 배치):
  24000080  0060  D  DMATxDscrTab
  240000E0  0060  D  DMARxDscrTab
  24000140  4983  D  memp_memory_RX_POOL_base   ← 18,819 바이트

수정 후 (D2 SRAM 명시 배치):
  30000000  0060  B  DMARxDscrTab
  30000100  0060  B  DMATxDscrTab
  30000200  4983  B  memp_memory_RX_POOL_base
```

심볼 타입에 주목: 원본은 **`D`(= `.data`)**, 수정 후는 **`B`(= `.bss`)** 이다.

#### 영향

**첫째, 플래시 낭비.** `.data`는 링커 스크립트에서 `>RAM_D1 AT> FLASH`로 정의되어 있다.
즉 초기값이 플래시에 저장되고 부팅 시 RAM으로 복사된다. 내용이 무의미한
**18,819바이트짜리 빈 버퍼 풀이 통째로 플래시에 들어가 있었다.**

| | text | data | 플래시 총 사용 |
|---|---:|---:|---:|
| 원본 | 65,764 | **19,127** | 84,891 B |
| 수정 | 65,756 | **116** | 65,880 B |
| 차이 | -8 | **-19,011** | **-19,011 B** |

STM32H750VBTX는 내장 플래시가 **128KB뿐**이다. 전체 용량의 **14.5%**를 회수했다.

**둘째, 배치 불안정성.** orphan 섹션 배치는 링커 버전, 오브젝트 파일 순서, 섹션 속성에
따라 달라진다. 현재는 AXI SRAM(D1, `0x24000000`)에 떨어져 우연히 동작하지만,
코드가 조금만 바뀌어도 배치가 달라질 수 있다. ST 레퍼런스 설계는 ETH DMA 구조체를
**D2 SRAM(`0x30000000`)** 에 두도록 하고 있다.

> **참고**: `STM32H750VBTX_RAM.ld` 빌드는 `.bss`가 **DTCM**에 배치된다.
> STM32H7의 ETH DMA는 **DTCM에 접근할 수 없다.** 이 구성에서는 명시적 배치가 선택이 아니라 필수다.

#### 수정

두 링커 스크립트 모두에 D2 SRAM 배치 블록을 추가했다.

```ld
.lwip_sec (NOLOAD) :
{
  . = ABSOLUTE(0x30000000);
  *(.RxDecripSection)

  . = ABSOLUTE(0x30000100);
  *(.TxDecripSection)

  . = ABSOLUTE(0x30000200);
  *(.Rx_PoolSection)
} >RAM_D2
```

`(NOLOAD)`로 선언하여 플래시에 저장되지 않도록 했다.

---

### 3.2 lwIP 힙과 RX 버퍼 풀 주소 중첩 🔴

#### 분석

`lwipopts.h`의 `LWIP_RAM_HEAP_POINTER`는 `0x30004000`이었다.
`mem.c:524`에서 확인한 대로 lwIP는 이 주소를 힙 시작점으로 그대로 사용한다.

```c
ram = (u8_t *)LWIP_MEM_ALIGN(LWIP_RAM_HEAP_POINTER);
```

RX 풀 크기를 실측하면 **18,819 바이트(`0x4983`)** 이다.

```
RX 풀:   0x30000200 ~ 0x30004B83
lwIP 힙: 0x30004000 ~
         ─────────────────────
         중첩 구간: 2,947 바이트
```

즉 §3.1의 섹션 배치를 올바르게 잡는 순간, **RX DMA가 쓰는 영역과 lwIP 힙이 2,947바이트 겹친다.**
DMA가 수신 데이터를 힙 메타데이터 위에 덮어쓰는 형태의, 추적이 매우 어려운 메모리 손상이 된다.

#### 수정

힙을 `0x30008000`으로 이동했다. RX 풀 끝(`0x30004B83`)과 약 13KB 여유가 생긴다.

```
0x30000000  RX DMA 디스크립터   (96 B)
0x30000100  TX DMA 디스크립터   (96 B)
0x30000200  RX pbuf 풀        (18,819 B) → 0x30004B83
0x30008000  lwIP 힙           (16 KB)
```

RAM_D2는 288KB이므로 충분한 여유가 있다.

> ⚠️ `LWIP_RAM_HEAP_POINTER`는 CubeMX 생성 값이다.
> **CubeMX GUI에서도 `0x30008000`으로 변경**해야 재생성 시 되돌아가지 않는다.

---

## 4. 초기화 순서 문제 (콜드 부팅) 🔴

### 4.1 근거

**데이터시트 p.146** — `MII_CFG_5`(offset `0xF100`) 리셋값은 `0x2044`이다.

| 필드 | 비트 | 리셋값 | 의미 |
|---|---|---|---|
| EN | 14 | 0 | 인터페이스 비활성 |
| ISOL | 13 | 1 | 출력 격리 |
| MIIRATE | 6:4 | 100 | Auto |
| MIIMODE | 3:0 | 0100 | **RGMII** |

**데이터시트 p.45–47** — self-start 모드에서도 부트로더는 `MII_CFG_5`의 **bit 14:13만** 설정한다
(Table 12/13). `MIIMODE`는 RGMII인 채로 남는다.

**결론: GSW145는 전원 인가 직후 RMII 50MHz 참조 클럭을 출력하지 않는다.**

한편 `stm32h7xx_hal_eth.c:355-372`의 `HAL_ETH_Init()`은 다음과 같다.

```c
SET_BIT(heth->Instance->DMAMR, ETH_DMAMR_SWR);
tickstart = HAL_GetTick();
while (READ_BIT(heth->Instance->DMAMR, ETH_DMAMR_SWR) > 0U)
{
  if (((HAL_GetTick() - tickstart) > ETH_SWRESET_TIMEOUT))   /* 500 ms */
  {
    heth->gState = HAL_ETH_STATE_ERROR;
    return HAL_ERROR;
  }
}
```

RM0433에 따르면 DMA 소프트웨어 리셋 비트는 **모든 활성 클럭 도메인의 리셋이 해제되어야** 클리어된다.
RMII 구성에서 그 클럭이 바로 `REF_CLK`다.

### 4.2 기존 코드의 문제

```c
MX_LWIP_Init();          /* → HAL_ETH_Init() : REF_CLK 필요 */
GSW145_Init_Sequence();  /* → 여기서야 50MHz 출력 시작 */
```

순서가 뒤집혀 있다. 게다가 실패를 이렇게 받았다.

```c
if (HAL_ETH_Init(&heth) != HAL_OK) return;   /* 조용히 리턴 */
```

이후 메인 루프는 `netif_set_link_up()`을 강제하고 `[UDP] sent` 로그를 계속 출력한다.
**죽은 MAC에 대고 성공 로그를 찍는 상태**가 된다.

### 4.3 왜 지금까지 동작했는가

디버거로 재플래시할 때는 **MCU만 리셋되고 GSW145는 전원이 유지**된다.
스위치는 이전 세션에서 쓴 `0x40B3` 설정을 그대로 들고 있으므로, 두 번째 실행부터는 항상 성공한다.
**전원을 완전히 차단했다 켜는 콜드 부팅에서만 드러나는 결함이다.**

### 4.4 수정

MDIO(MDC)는 HCLK에서 파생되므로 **REF_CLK 없이도 동작한다.** 이를 이용해 순서를 재구성했다.

```
1. ethernetif_PreInitMDIO()   ETH 클럭 + RMII 핀 + MDC 분주기만 설정
2. GSW145_Init()              MDIO로 스위치 설정 → 50MHz 클럭 공급 시작
3. HAL_ETH_Init()             이제 REF_CLK가 살아있으므로 성공
```

`HAL_ETH_SetMDIOClockRange()`, `HAL_ETH_ReadPHYRegister()`, `HAL_ETH_WritePHYRegister()`가
`heth->gState`를 검사하지 않고 레지스터만 조작함을 HAL 소스에서 확인하여, `HAL_ETH_Init()` 이전
호출이 안전함을 검증했다.

배치 위치는 `ethernetif.c`의 `USER CODE BEGIN MACADDRESS` 훅이다. `HAL_ETH_Init()` 직전이면서
**CubeMX 재생성에도 보존되는** 유일한 지점이다.

추가로 3회 재시도 로직과 명시적 오류 로그를 넣어, 조용한 실패를 없앴다.

---

## 5. 포트 5 Autopolling 문제 🔴

### 5.1 근거

**데이터시트 p.176** — `PHY_ADDR_5` 설명에 명시되어 있다.

> *"**When autopolling in MMDC_CFG_0 is disabled**, the modes defined here are used instead of the polling values."*

**데이터시트 p.162** — `MMDC_CFG_0`(offset `0xF40B`) 리셋값은 `0x006F`다.

```
PEN[6:0] = 0x6F = 110 1111
                    │└─────── 포트 0,1,2,3 활성
                    └──────── 포트 5 활성 ← 문제
```

**포트 5의 autopolling이 기본 활성 상태다.**

**데이터시트 p.37** — 폴링 FSM은 REG0을 읽어 all-ones면
*"PHY Inactive, Link Status = 0"* 으로 판정한다.

포트 5에는 외부 PHY가 없다. 따라서 폴링은 항상 "링크 없음"으로 결론내고,
애써 강제한 `PHY_ADDR_5`의 링크업 설정을 덮을 수 있다. 부하·타이밍에 따라
**간헐적으로 나타나는 링크 다운**의 유력한 원인이다.

### 5.2 수정

`GSW145_Init()`에서 read-modify-write로 포트 5 폴링만 해제했다.

```c
GSW145_ReadReg(GSW145_REG_MMDC_CFG_0, &mmdc_cfg);
mmdc_cfg &= (uint16_t)~GSW145_MMDC_CFG0_PORT5;   /* bit 5 클리어 */
GSW145_WriteReg(GSW145_REG_MMDC_CFG_0, mmdc_cfg);
```

스트래핑에 따라 달라질 수 있는 다른 비트를 보존하기 위해 하드코딩(`0x004F`) 대신
read-modify-write를 사용했다.

> **참고**: STM32는 GSW145의 **SMDIO(슬레이브)** 핀에 연결되고, autopolling은 별도의
> **MMDIO(마스터)** 핀을 사용한다(p.40). 두 MDIO 마스터가 버스에서 충돌하는 문제는 아니다.

---

## 6. 레지스터 값 검증

### 6.1 `MII_CFG_5 = 0x40B3` — 검증 통과 ✅

데이터시트 p.146–147과 비트 단위로 대조한 결과 의도대로 정확하다.

| 필드 | 비트 | 값 | 의미 |
|---|---|---|---|
| RST | 15 | 0 | 리셋 해제 |
| EN | 14 | 1 | 인터페이스 활성 |
| ISOL | 13 | 0 | 격리 해제 |
| CLKDIS | 12 | 0 | 링크다운 시 클럭 자동차단 안 함 |
| RGMII_IBS | 8 | 0 | RGMII in-band status off |
| **RMII** | 7 | **1** | **참조 클럭 출력** — 스위치가 50MHz 공급 |
| MIIRATE | 6:4 | 011 | 50 MHz (RMII 필수값) |
| MIIMODE | 3:0 | 0011 | RMII |

### 6.2 `PHY_ADDR_5` — `0x2BF5` → `0x2AA5` 로 수정

기존 값 `0x2BF5`를 분해한 결과, 의도한 3개 필드는 맞았으나 2개 필드에 부작용이 있었다.

| 필드 | 비트 | `0x2BF5` | 판정 |
|---|---|---|---|
| AUTO_SEL | 15 | 0 (POLLING) | ✅ |
| LNKST | 14:13 | 01 = **UP** | ✅ 의도대로 |
| SPEED | 12:11 | 01 = **100M** | ✅ 의도대로 |
| FDUP | 10:9 | 01 = **Full** | ✅ 의도대로 |
| FCONTX | 8:7 | 11 = DIS | ⚠️ pause 비활성 |
| FCONRX | 6:5 | 11 = DIS | ⚠️ pause 비활성 |
| **ADDR** | 4:0 | **10101 = 21** | ❌ 기본값 5에서 변경 |

데이터시트 p.46–47에 나오는 **self-start 모드의 공식 값은 `0x32A5`** 이며,
그 의미는 *"force link on, speed is 1 Gbps, full duplex, pause enable"* + `ADDR = 5`다.

여기서 `SPEED`만 1Gbps → 100Mbps로 바꾸면 정확히 **`0x2AA5`** 가 된다.

```
0x32A5  = 0011 0010 1010 0101   (데이터시트 기준값, 1 Gbps)
0x2AA5  = 0010 1010 1010 0101   (본 수정값, 100 Mbps)
                ^^
          SPEED 필드만 차이
```

`ADDR`을 21로 바꾼 것은 시행착오 중 섞인 값으로 판단되어 기본값 5로 복원했다.

---

## 7. 그 외 수정

### 7.1 `HAL_ETH_ErrorCallback` 죽은 코드 🟡

기존 코드는 `HAL_ETH_Start_IT()`로 시작하고 `HAL_ETH_ErrorCallback()`에 RBU
(Receive Buffer Unavailable) 복구 로직을 두었다. 그러나 확인 결과:

- `Core/Src/stm32h7xx_it.c`에 **`ETH_IRQHandler` 구현이 없다**
- `startup_stm32h750vbtx.s:513`에 weak 심볼로 `Default_Handler`에만 연결되어 있다
- `HAL_ETH_MspInit()`에 **NVIC 활성화 코드가 없다**

따라서 `HAL_ETH_IRQHandler()`가 호출될 일이 없고, **RBU 복구 로직은 절대 실행되지 않는다.**

**수정**: 폴링 구조(`MX_LWIP_Process()`가 메인 루프에서 호출)에 맞게 `HAL_ETH_Start()`로 변경했다.
RBU 복구는 HAL이 이미 처리한다 — `HAL_ETH_ReadData()`가 `ETH_UpdateDescriptor()`를 호출하고,
이것이 `DMACRDTPR`(RX 디스크립터 tail pointer)을 재기록하여 RX DMA를 재개한다
(`stm32h7xx_hal_eth.c:1122-1126`, `1218`). 래치된 RBU 상태 비트만 디버깅 편의를 위해 클리어한다.

### 7.2 RX 하드웨어 체크섬 드롭 🟡

`stm32h7xx_hal_eth.c:2919`에서 HAL 기본 MAC 설정이 `DropTCPIPChecksumErrorPacket = ENABLE`임을 확인했다.
이 경우 MAC이 체크섬 이상으로 판단한 프레임을 **lwIP가 보기 전에 조용히 폐기**한다.
에러 콜백도, 카운터도 없다.

기존 코드는 TX 방향만 소프트웨어 체크섬으로 전환하고 RX는 그대로였다.

**수정**: `MACConf.DropTCPIPChecksumErrorPacket = DISABLE;` 추가. 송수신 양방향 모두
소프트웨어 체크섬(`CHECKSUM_GEN_*` / `CHECKSUM_CHECK_*`)으로 일관되게 처리한다.

### 7.3 IP 주소 `0.0.0.0` 🟡

`Cleo_SMB.ioc:14`에 `LWIP.LWIP_DHCP=0`이고 정적 IP는 미설정 상태였다.
`LWIP/App/lwip.c`의 `IP_ADDRESS[]`가 전부 0이므로 인터페이스 주소가 `0.0.0.0`이었다.

이 상태에서 UDP를 보내면 **출발지 IP가 `0.0.0.0`** 인 패킷이 나간다.
Wireshark에는 보이지만 다수의 OS가 소켓 계층에서 폐기한다.
기존 코드가 `etharp_add_static_entry()`로 PC MAC(`00:E0:4C:5D:4F:88`)을 하드코딩해야 했던
근본 원인이 이것이다.

**수정**: `LWIP/App/lwip.c`의 `USER CODE BEGIN IP_ADDRESSES` 섹션에 정적 IP 설정.

```
IP      : 192.168.1.10
Netmask : 255.255.255.0
Gateway : 192.168.1.1
```

정상 IP가 생겼으므로 **static ARP 하드코딩을 제거**했다. 일반 ARP로 피어를 해석하며,
PC가 바뀌거나 NIC가 교체되어도 동작한다.

### 7.4 기타

| 항목 | 내용 |
|---|---|
| `MII_CFG_5.RST` 펄스 | RGMII → RMII 모드 전환 시 xMII 모듈 리셋 추가 (p.146) |
| `MEM_SIZE` | opt.h 기본값 1,600 B → **16 KB**. 모든 `PBUF_RAM` 송신 할당의 원천 |
| `heth.Init.MACAddr` | 스택 지역변수를 가리키던 dangling pointer → `static` |
| MAC 주소 | `02:80:E1:00:00:01` (bit 1 = locally administered) |
| `udp_send()` 반환값 | 기존엔 무시하고 무조건 "sent" 출력 → 실제 결과 확인 |
| `CHECKSUM_GEN_ICMP` | 생성 코드에 ICMP6만 있고 ICMP 누락 → 명시 |
| D-Cache 주석 | 프로젝트 전체에 `SCB_EnableDCache()` 호출이 없음을 명시하고, 활성화 시 디스크립터 테이블도 처리해야 함을 경고 |

---

## 8. 변경 파일

```
 Core/Inc/gsw145.h        | 신규   GSW145 드라이버 헤더
 Core/Src/gsw145.c        | 신규   GSW145 드라이버 구현
 Core/Src/main.c          | +115   UDP 테스트, printf 리타겟, 메인 루프
 LWIP/App/lwip.c          |  +18   정적 IP 설정
 LWIP/Target/ethernetif.c | 재작성  MAC-to-MAC 드라이버
 LWIP/Target/ethernetif.h |  +10   ethernetif_PreInitMDIO() 선언
 LWIP/Target/lwipopts.h   |  +45   힙 주소, MEM_SIZE, 체크섬
 STM32H750VBTX_FLASH.ld   |  +24   ETH DMA 섹션 배치
 STM32H750VBTX_RAM.ld     |  +15   ETH DMA 섹션 배치
```

### 설계 원칙

1. **CubeMX 재생성 안전성** — 모든 수정을 `USER CODE` 섹션 안에 배치
2. **관심사 분리** — GSW145 드라이버를 독립 파일로 분리 (`Core`가 소스 경로에 등록되어 있어 자동 빌드)
3. **근거 명시** — 모든 레지스터 값에 비트 필드 분해와 데이터시트 페이지 번호를 주석으로 기록
4. **조용한 실패 제거** — 초기화 실패 시 명시적 로그 출력

---

## 9. 빌드 검증

STM32CubeCLT 1.20.0의 `arm-none-eabi-gcc`로 실제 빌드하여 검증했다.

```
컴파일: 88개 소스, -Wall -Wextra, 경고 0건, 실패 0건
링크  : 성공
```

### 메모리 배치 검증 (`arm-none-eabi-nm`)

```
30000000  00000060  B  DMARxDscrTab
30000100  00000060  B  DMATxDscrTab
30000200  00004983  B  memp_memory_RX_POOL_base
```

의도한 D2 SRAM 주소에 정확히 배치되었고, 심볼 타입이 `B`(.bss)로 바뀌어
플래시를 소비하지 않음을 확인했다.

### 크기 비교 (`arm-none-eabi-size`)

```
원본:  text 65,764   data 19,127   bss 16,640
수정:  text 65,756   data    116   bss 36,003
                     ─────────────
                     -19,011 바이트 (플래시)
```

---

## 10. 후속 확인 사항

### 10.1 콜드 부팅 테스트 (필수)

보드 전원을 **완전히 차단했다가** 인가한다. 부팅 로그 첫 줄이 진단 결과다.

```
[GSW145] MII_CFG_5 at power-up: 0x2044
  → 스위치가 RGMII 모드로 부팅. §4의 순서 수정이 필수였음을 의미 (예상되는 결과)

[GSW145] MII_CFG_5 at power-up: 0x40B3
  → EEPROM이 부팅 시 이미 설정. 순서 수정은 안전장치 역할
```

정상 부팅 시 기대 로그:

```
[GSW145] init
[GSW145] MII_CFG_5 at power-up: 0x____
[GSW145] MII_CFG_5 = 0x40B3 (expect 0x40B3)
[GSW145] PHY_ADDR_5 = 0x2AA5 (expect 0x2AA5)
[GSW145] port 5 forced 100M/FDX, RMII clock out

=== STM32H750 + GSW145 MAC-to-MAC ===
[NET] ip=192.168.1.10 link=up
```

### 10.2 CubeMX GUI 동기화 (필수)

`.ioc` 재생성 시 되돌아가는 항목이다. GUI에서도 반드시 변경한다.

| 위치 | 항목 | 값 |
|---|---|---|
| LwIP → Key Options | `CHECKSUM_BY_HARDWARE` | **Disabled** |
| LwIP → Key Options | `LWIP_RAM_HEAP_POINTER` | **0x30008000** |

### 10.3 통신 확인

- PC IP를 `192.168.1.2`로 설정
- UDP 8000 포트 수신 대기
- **Wireshark로 실제 수신 확인** — `udp_send()`가 `ERR_OK`를 반환해도 이는
  프레임이 MAC에 도달했다는 의미일 뿐, 피어가 수신했다는 보장이 아니다

---

## 10.4 CubeMX 재생성 대응 ⚠️

### 문제

CubeMX가 생성하는 `ethernetif.c` 템플릿에는 USER CODE 마커가 **19개**뿐이며,
다음 위치에는 **훅이 존재하지 않는다.**

- `low_level_init()`의 PHY 초기화 이후 구간 (MAC 속도/듀플렉스/체크섬 설정)
- `low_level_output()` / `low_level_input()` / `ethernetif_input()`
- `ethernet_link_check_state()` 본문
- 파일 상단의 `#include "lan8742.h"` 및 LAN8742 객체 선언

따라서 **`ethernetif.c`는 USER CODE 섹션만으로는 재생성으로부터 보호할 수 없다.**
재생성하면 다음이 복구되어 이더넷이 동작하지 않는다.

| 되돌아가는 것 | 결과 |
|---|---|
| `ethernet_link_check_state()` LAN8742 폴링 | PHY가 없어 0xFFFF 읽음 → **100ms마다 링크 다운** |
| `LAN8742_Init()` 실패 시 early return | `HAL_ETH_Start()` 미실행 → **인터페이스 사망** |
| MAC 속도/듀플렉스 강제 설정 | 링크 파라미터 불일치 |
| `DropTCPIPChecksumErrorPacket = DISABLE` | RX 프레임 조용히 폐기 |
| `TxConfig.ChecksumCtrl = ETH_CHECKSUM_DISABLE` | 소프트웨어 체크섬 위에 덮어씀 |
| MAC 주소 `02:...` → `00:80:E1:00:00:00` | - |

`USER CODE BEGIN LOW_LEVEL_INIT`은 함수 끝에 있지만, 그 앞의 `LAN8742_Init()` 실패
경로가 `return`으로 빠져나가므로 **도달하지 못한다.**

### 대응 1 — 링크 타임 canary (빌드 실패로 알림)

`ethernetif.c`의 **USER CODE 밖**에 마커 심볼을 정의하고, CubeMX가 건드리지 않는
`gsw145.c`에서 이를 참조한다.

```c
/* LWIP/Target/ethernetif.c - USER CODE 밖 */
const uint32_t Cleo_EthernetifIsCustomized = 1U;

/* Core/Src/gsw145.c - GSW145_Init() 내부 */
if (Cleo_EthernetifIsCustomized != 1U) { return HAL_ERROR; }
```

재생성되면 정의가 사라지고 링크가 실패한다.

**실제 검증** — CubeMX 동작(생성 영역은 템플릿으로 복원, USER CODE는 이월)을
스크립트로 재현하여 확인했다.

```
USER CODE blocks carried over : Header, 0, 1, 2, 4, MACADDRESS,
                                PHY_PRE_CONFIG, 6, HAL ETH Rx/Tx callbacks
blocks LOST (no such marker)  : ETHERNETIF_INPUT, ETHERNET_LINK_CHECK_STATE

$ arm-none-eabi-gcc ... -o regen.elf
gsw145.c:(.text.GSW145_Init+0x134): undefined reference to `Cleo_EthernetifIsCustomized'
collect2.exe: error: ld returned 1 exit status
```

조용히 잘못 동작하는 대신 **빌드가 멈춘다.** 복구는 `git checkout -- LWIP/Target/ethernetif.c`.

> 주의: `MACADDRESS` 훅(= `GSW145_Init()` 호출부)은 USER CODE라 재생성 후에도 남는다.
> 이것이 canary 참조를 살려두므로 `--gc-sections`에도 제거되지 않는다.

### 대응 2 — 링크 폴링을 `lwip.c`에서 영구 차단

가장 위험한 되돌림(100ms마다 링크 다운)은 **실제로 존재하는 마커**로 막았다.

```c
/* LWIP/App/lwip.c - Ethernet_Link_Periodic_Handle() */
/* USER CODE BEGIN 4_4_1 */
  LWIP_UNUSED_ARG(netif);
  return;                       /* ethernet_link_check_state()를 아예 호출하지 않음 */
/* USER CODE END 4_4_1 */
```

`ethernetif.c`가 어떤 상태든 LAN8742 폴링은 실행되지 않는다.

### 대응 3 — 재생성 후 검사 스크립트

```sh
sh tools/check_after_cubemx.sh
```

CubeMX가 덮어쓸 수 있는 모든 항목을 검사한다 — `lwipopts.h`의 체크섬/힙 주소,
두 링커 스크립트의 `.lwip_sec` 블록, `ethernetif.c`의 커스터마이징 흔적,
`lwip.c`의 정적 IP와 링크 폴링 차단. 실패 시 각 항목의 복구 방법을 출력한다.

### 권장 워크플로

```
1. CubeMX에서 Generate Code
2. sh tools/check_after_cubemx.sh
3. BROKEN 항목이 있으면 안내대로 복구
     ethernetif.c → git checkout -- LWIP/Target/ethernetif.c
     lwipopts.h   → CubeMX GUI 값 확인 후 재생성
4. 빌드 (canary가 남은 누락을 링크 에러로 잡아냄)
```

---

## 11. 알려진 한계

| 항목 | 내용 |
|---|---|
| **링크 상태 감시 없음** | `ethernet_link_check_state()`가 의도적으로 빈 함수다. 배선 단선, 스위치 리셋, GSW145 재설정이 스택에 보이지 않는다. 필요하다면 이 함수에서 `PHY_ADDR_5` 또는 포트 5 링크 상태를 MDIO로 폴링하도록 구현해야 한다. |
| **D-Cache 비활성 전제** | 프로젝트에 `SCB_EnableDCache()` 호출이 없어 캐시 유지보수 코드가 불필요하다. 향후 D-Cache를 활성화하면 RX/TX 버퍼뿐 아니라 `DMARxDscrTab` / `DMATxDscrTab` **디스크립터 테이블**의 clean/invalidate도 함께 처리해야 한다. |
| **GSW145 부트 모드 미확인** | 회로도가 자동압축해제 실행파일(`.exe`) 형태여서 스트래핑 핀 구성을 확인하지 못했다. §10.1의 콜드 부팅 로그로 실제 부팅 상태를 판별할 수 있다. |
| **스위치 코어 초기화 미수행** | 보드가 "wait for external master" 모드로 스트래핑된 경우 `GSWIP_CFG`와 `RST_REQ` 설정이 추가로 필요하다(p.45). 현재 코드가 동작한다는 것은 self-start 또는 EEPROM 모드임을 시사하나, 명시적으로 확인되지는 않았다. |

---

## 12. 참고 자료

| 출처 | 위치 |
|---|---|
| GSW145 데이터시트 | `Document/620246_GSW145_DS_Rev1.4.pdf` |
| `MII_CFG_5` 레지스터 | 데이터시트 p.146–147 |
| `PHY_ADDR_5` 레지스터 | 데이터시트 p.176–177 |
| `MMDC_CFG_0` 레지스터 | 데이터시트 p.162 |
| Autopolling FSM | 데이터시트 p.37–38 |
| Boot Loader / Pin Strapping | 데이터시트 p.45–47 |
| MDIO Slave Module (SMDIO) | 데이터시트 p.40–41 |
| `HAL_ETH_Init()` DMA 리셋 | `Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_eth.c:355-372` |
| `DropTCPIPChecksumErrorPacket` 기본값 | `stm32h7xx_hal_eth.c:2919` |
| `ETH_UpdateDescriptor()` RX 재개 | `stm32h7xx_hal_eth.c:1152-1223` |
| lwIP 힙 배치 | `Middlewares/Third_Party/LwIP/src/core/mem.c:524` |
