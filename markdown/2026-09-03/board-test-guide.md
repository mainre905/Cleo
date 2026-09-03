# 내일 보드에서 할 일 — GSW145 1Gbps 트래픽 시험

작성일: 2026-09-03

## 목표

STM32H750에서 **UART로 명령어를 입력하면**, GSW145 스위치가 자신의 내장 기가비트 PHY로
**1000BASE-T 라인 레이트 트래픽을 PC로 쏘고**, PC에서 실제로 1Gbps가 나오는지 확인한다.

```
  ┌──────────┐   RMII 100M   ┌─────────────┐   1000BASE-T   ┌─────┐
  │ STM32H750│◄─────────────►│   GSW145    │◄──────────────►│ PC  │
  │          │   + MDIO 제어  │  포트 0~3    │   RJ45         │     │
  └────┬─────┘               └─────────────┘                └─────┘
       │ UART 115200
   ┌───▼────┐
   │ PuTTY  │  ← 여기서 g / s / l 명령
   └────────┘
```

**핵심**: 트래픽은 MCU를 거치지 않는다. MCU는 MDIO로 "쏴라"라고 지시만 하고,
실제 패킷은 GSW145의 PHY 안에서 만들어져 바로 랜선으로 나간다.
그래서 MCU의 RMII가 100Mbps여도 1Gbps 측정이 가능하다.

---

## 준비물

- [ ] Cleo SMB 보드 + ST-Link
- [ ] USB-UART 어댑터 (보드 PB14/PB15 ↔ PC)
- [ ] 랜케이블 — **8가닥 전부 연결된 것** (4가닥짜리는 100Mbps까지만 됨)
- [ ] PC에 **PuTTY** (또는 아무 시리얼 터미널)
- [ ] PC에 **Wireshark** — 이게 없으면 측정을 못 한다 (이유는 5단계에)

---

# 1단계 · CubeMX 코드 생성 (제일 먼저, 필수)

지난번에 `.ioc`에 핀 9개를 추가했는데 **아직 코드 생성을 안 했다.**
지금 `Core/Inc/main.h`에 `ETH_RESET_N_Pin` 같은 매크로가 없는 상태다.

### 하는 법

1. STM32CubeIDE에서 `Cleo_SMB.ioc` 더블클릭
2. **Migrate 물어보면 "Use the version used to generate the initial project"** (원래 버전 유지)
3. 왼쪽 메뉴에서 아래 두 개를 확인/수정:

| 위치 | 항목 | 값 |
|---|---|---|
| Middleware → LwIP → Key Options | `CHECKSUM_BY_HARDWARE` | **Disabled** |
| Middleware → LwIP → Key Options | `LWIP_RAM_HEAP_POINTER` | **0x30008000** |

4. `Alt + K` (Generate Code)

### 생성 직후 반드시 검사

Git Bash에서:

```bash
cd "/c/Work/MCU(STM)/Project/Cleo"
sh tools/check_after_cubemx.sh
```

**전부 `OK`가 나와야 한다.** `BROKEN`이 있으면 화면에 복구 방법이 같이 나온다.
가장 흔한 건 `ethernetif.c`가 덮어써진 경우인데, 이때는:

```bash
git checkout -- LWIP/Target/ethernetif.c
```

> **왜 검사가 필요한가**: CubeMX의 `ethernetif.c` 템플릿에는 우리가 고친 부분을 보호할
> USER CODE 마커가 없다. 그냥 두면 PHY도 없는데 LAN8742를 찾다가 100ms마다 링크를
> 내려버린다. 혹시 검사를 깜빡해도 빌드가 `undefined reference to
> Cleo_EthernetifIsCustomized`로 멈추게 안전장치를 넣어놨다.

---

# 2단계 · 빌드 & 플래시

STM32CubeIDE에서 빌드 → 보드에 다운로드.

빌드가 성공하면 정상이다. 참고로 검증된 크기는 **flash 68.5KB / 128KB**.

---

# 3단계 · 부팅 로그 확인 ⭐ 가장 중요

PuTTY 설정: **115200, 8-N-1**, 해당 COM 포트.

### ⚠️ 반드시 "콜드 부팅"으로

디버거로 재시작(리셋 버튼)하지 말고, **보드 전원을 완전히 뽑았다가 다시 꽂는다.**

이유: 전원이 유지되면 GSW145가 이전 설정을 그대로 기억한다. 그러면 초기화 순서가
틀려도 동작하는 것처럼 보인다. 진짜 확인은 콜드 부팅에서만 된다.

### 정상이면 이렇게 나온다

```
[GSW145] init
[GSW145] MII_CFG_5 at power-up: 0x2044      ← 이 값을 꼭 기록!
[GSW145] MII_CFG_5 = 0x40B3 (expect 0x40B3)
[GSW145] PHY_ADDR_5 = 0x2AA5 (expect 0x2AA5)
[GSW145] port 5 forced 100M/FDX, RMII clock out

=== STM32H750 + GSW145 MAC-to-MAC ===
[NET] ip=192.168.1.10 link=up

--- commands ---
  0-3  select RJ45 port (now: 0)
  g    start 1G traffic generator on the selected port
  s    stop it
  l    link status of all RJ45 ports
  h    this list
```

### `MII_CFG_5 at power-up` 값의 의미

| 값 | 뜻 |
|---|---|
| `0x2044` 또는 `0x6044` | 스위치가 RGMII 모드로 부팅함. **초기화 순서 수정이 실제로 필요했다는 증거** (예상되는 값) |
| `0x40B3` | EEPROM이 이미 설정해줌. 순서 수정은 보험 역할 |

어느 쪽이든 그 아래 줄들이 `expect`와 일치하면 정상이다.

### 이게 안 나오면

| 증상 | 원인 |
|---|---|
| 아무것도 안 나옴 | UART 배선/COM포트/보드레이트 확인 |
| `MDIO read failed - switch unreachable` | 스위치가 리셋에 걸려있거나 MDIO 배선 문제. **6단계 참고** |
| `HAL_ETH_Init failed - no RMII REF_CLK` | 스위치가 50MHz 클럭을 안 주고 있음 |
| `link=DOWN` | 위 GSW145 로그가 정상인지 먼저 확인 |

---

# 4단계 · PC 네트워크 설정 + 링크 확인

## PC IP 설정

랜선을 **GSW145의 RJ45 포트 0**에 꽂는다. PC 이더넷 어댑터 설정:

| 항목 | 값 |
|---|---|
| IP | `192.168.1.2` |
| 서브넷 마스크 | `255.255.255.0` |
| 게이트웨이 | 비워둠 |

> MCU는 `192.168.1.10`으로 잡혀 있다.

## 링크 속도 확인

PuTTY에서 **`l`** 입력:

```
--- RJ45 link status ---
  port 0 : 1000 Mbps full duplex      ← 이게 나와야 한다
  port 1 : no link
  port 2 : no link
  port 3 : no link
```

**`1000 Mbps full duplex`가 아니면 다음 단계로 가도 소용없다.**

| 나온 값 | 조치 |
|---|---|
| `no link` | 케이블 확인, PC 랜포트 활성화 확인 |
| `100 Mbps` | 케이블이 4가닥짜리다. **8가닥 케이블로 교체** |
| `HALF duplex` | 오토네고 문제. 케이블 교체 후 재시도 |
| `read failed` | MDIO 문제 (6단계) |

---

# 5단계 · 트래픽 발생 & 측정

## 먼저 Wireshark를 켠다 (순서 중요!)

1. Wireshark 실행
2. 해당 이더넷 인터페이스 선택
3. **캡처 시작** (기본이 promiscuous 모드라 그냥 시작하면 된다)

### 왜 Wireshark가 꼭 필요한가

TPG가 만드는 프레임의 목적지 MAC은 `00-03-19-FF-FF-Fx`로 **하드웨어에 고정**되어 있다.
PC 랜카드는 자기 MAC이 아닌 유니캐스트 프레임을 **랜카드 단계에서 그냥 버린다.**
Wireshark가 캡처를 시작하면 랜카드가 promiscuous 모드로 바뀌어 모든 프레임을 받게 된다.

즉 **Wireshark를 안 켜면 트래픽이 실제로 오고 있어도 PC에서는 아무것도 안 보인다.**

## 트래픽 시작

PuTTY에서 **`g`** 입력:

```
[TPG] port 0 (PHY 0) linked 1000BASE-T FDX
[TPG] TPGCTRL = 0x0453
[TPG] running: 1518-byte frames, ~987 Mbps of payload
[TPG] frames go to 00-03-19-FF-FF-Fx, so capture the PC NIC in
[TPG] promiscuous mode (Wireshark) or the NIC will discard them
```

다른 포트를 쓰려면 먼저 `0`~`3`으로 포트를 고르고 `g`를 누른다.

## 측정 방법 (셋 중 편한 것)

### 방법 A — Wireshark 통계 (가장 간단)

`Statistics` → `Capture File Properties` → **Average bits/s** 확인

**기대값: 약 950~990 Mbps**

> 주의: 1Gbps를 전부 캡처하면 Wireshark가 패킷을 놓칠 수 있다. 그래도 대략적인
> 확인은 된다. 정확한 수치는 방법 B가 낫다.

### 방법 B — Windows 어댑터 카운터 (가장 정확)

Wireshark 캡처는 **켜둔 채로**, PowerShell에서:

```powershell
$n = (Get-NetAdapter | Where-Object Status -eq 'Up').Name
$a = (Get-NetAdapterStatistics -Name $n).ReceivedBytes
Start-Sleep -Seconds 10
$b = (Get-NetAdapterStatistics -Name $n).ReceivedBytes
"{0:N1} Mbps" -f ((($b - $a) * 8) / 10 / 1MB)
```

**기대값: 940 Mbps 이상**

### 방법 C — 작업 관리자

`작업 관리자` → `성능` → `이더넷` 그래프

간편하지만 눈금이 거칠다. 참고용.

## 트래픽 정지

PuTTY에서 **`s`** 입력:

```
[TPG] port 0 stopped
```

---

# 6단계 · 문제 해결

## 숫자가 987Mbps보다 많이 낮다

| 원인 | 확인 |
|---|---|
| Wireshark가 못 따라감 | 방법 B로 다시 측정 |
| 링크가 1G가 아님 | `l` 명령으로 재확인 |
| 케이블 품질 | Cat5e 이상, 짧은 것으로 교체 |

## `g` 를 눌렀는데 거부당한다

```
[TPG] port 0 linked at 100 Mbps FDX, not 1000BASE-T
```

의도된 동작이다. **1Gbps 측정이 목적인데 1G 링크가 아니면 시작하지 않는다.**
케이블과 PC 랜카드를 먼저 해결한다.

## `MDIO read failed` / `read failed`

`ETH_RESET_N`(PD1)이 GSW145 리셋 라인인데, 지금까지 **아무도 제어하지 않고 있었다.**
1단계에서 CubeMX 코드 생성을 하면 이 핀이 출력 HIGH(리셋 해제)로 초기화된다.

그래도 안 되면 이 핀 문제일 가능성이 크다. 알려주면 명시적 리셋 시퀀스
(LOW 펄스 → HIGH → 부팅 대기)를 넣어주겠다.

## 콘솔이 UDP 로그로 도배된다

1초마다 `[UDP] Send Success!`가 찍혀서 TPG 로그가 묻힌다.
급하면 `Core/Src/main.c`의 `UDP_SEND_INTERVAL_MS`를 `10000`으로 바꾸면 10초에 한 번이 된다.
아예 끄고 싶으면 말해달라.

---

# 명령어 요약

| 키 | 동작 |
|---|---|
| `0` `1` `2` `3` | 대상 RJ45 포트 선택 |
| `g` | 선택한 포트에서 1G 트래픽 시작 |
| `s` | 정지 |
| `l` | 전체 포트 링크 상태 |
| `h` | 도움말 |

---

# ⚠️ 시작 전 확인해야 할 안전 사항

**`MOT_DRVOFF` (PD13)를 HIGH로 초기화하도록 설정해 두었다.**

이 보드의 명명 규칙상 액티브 로우 신호는 `MOT_NFAULT`, `ETH_RESET_N`처럼 표시되는데
`MOT_DRVOFF`에는 표시가 없어서 액티브 하이로 판단했다. 즉 **HIGH = 모터 드라이버 정지**로
보고 부팅 시 안전한 쪽으로 잡았다.

**모터 드라이버 데이터시트로 꼭 확인해달라. 반대라면 CubeMX 코드 생성 후 첫 부팅에서
모터가 돌 수 있다.** 확인 전이라면 모터 전원을 분리한 상태로 시험하는 것을 권한다.

같이 추가된 핀들:

| 핀 | 이름 | 방향 | 초기값 |
|---|---|---|---|
| PD1 | `ETH_RESET_N` | 출력 | HIGH (리셋 해제) |
| PD10 | `MOT_DIR` | 출력 | LOW |
| PD13 | `MOT_DRVOFF` | 출력 | **HIGH** ← 확인 필요 |
| PA6 | `MOT_FG` | 입력 | - |
| PA8 | `IMU_ALARMB` | 입력 | - |
| PC6 | `MOT_NFAULT` | 입력 | - |
| PB0 | `OPTIC_SD` | 입력 | - |
| PB1 | `PG_1V1_ETH` | 입력 | - |
| PD12 | `ASIL_B_HALL_OUT` | 입력 | - |

읽기만 하는 신호는 전부 입력으로 두어 외부와 충돌하지 않게 했다.

---

# 아직 검증 안 된 것 (정직하게)

내가 확인한 것과 못 한 것을 구분해 둔다.

### ✅ 확인함

- 모든 레지스터 값을 데이터시트와 비트 단위로 대조
  - `TPGCTRL = 0x0453` (p.313-315)
  - MDIO 프록시 `MMDIO_CTRL/READ/WRITE` (p.160-161)
  - `MIISTAT` 속도 판정 (p.307)
- 전체 빌드: 컴파일 경고 0건, 링크 성공, flash 68.5KB/128KB

### ❌ 확인 못 함

- **실제 보드에서 동작시켜 보지 못했다.** 내일 이 문서대로 해보는 게 첫 검증이다.
- TPG가 정말 987Mbps를 내는지
- 콜드 부팅 시 `MII_CFG_5` 실제 값
- `ETH_RESET_N` 배선이 정말 GSW145 리셋인지 (회로도 네트명으로 추정)

### 참고: 이전 코드에 있던 문제

`GSW145_Start1G_TrafficGenerator()`가 쓰던 `0xF200`~`0xF207` 주소는
**데이터시트에 존재하지 않는다.** 스위치 내부 레지스터 맵은 `0xF16B`에서 끝나고
`0xF400`으로 건너뛴다. 또 PHY 레지스터는 직접 주소로 접근할 수 없고 MDIO 프록시를
거쳐야 한다. 그래서 데이터시트 기준으로 새로 작성했다.

---

# 결과 알려줄 때

이 세 가지만 알려주면 다음 단계를 잡을 수 있다.

1. **콜드 부팅 로그 전체** (특히 `MII_CFG_5 at power-up` 값)
2. **`l` 명령 출력**
3. **측정된 Mbps 값**

문제가 생기면 그 시점의 PuTTY 출력을 그대로 복사해서 주면 된다.
