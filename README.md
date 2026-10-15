# ESP32 PIR Motion Sensor

ESP32에 PIR 인체감지센서(HC-SR501)를 붙여 사람 움직임을 감지하는 Arduino 스케치.
`delay()` 없이 `millis()` 기반으로 돌아가므로 다른 작업을 덧붙여도 루프가 막히지 않는다.

## 배선

| 센서 (HC-SR501) | ESP32 |
|-----------------|-------|
| VCC | 5V (VIN) |
| OUT | GPIO 16 |
| GND | GND |

HC-SR501의 OUT은 HIGH가 3.3V라 ESP32에 바로 연결해도 안전하다.
AM312(3.3V 동작)를 쓰면 VCC를 3V3에 연결한다.

표시용 LED는 GPIO 2(보드 내장)를 쓴다. 외부 LED를 달 경우 220옴 직렬.

### 주의: GPIO 16

ESP32-WROVER 계열(PSRAM 탑재)은 GPIO16/17이 내부 PSRAM에 배정되어 있어 쓸 수 없다.
그 경우 `PIR_PIN`을 27이나 32 등으로 바꾼다. PSRAM 없는 WROOM-32는 16번을 그대로 쓸 수 있다.

칩과 PSRAM 유무는 esptool로 확인할 수 있다.

```
esptool --port /dev/cu.usbserial-XXXX flash_id
```

## 동작

- 전원 인가 후 `WARMUP_MS` 동안은 센서 안정화 구간으로, 내장 LED가 깜빡이며 감지를 무시한다.
  HC-SR501은 전원 직후 30~60초간 오탐이 많으므로 실사용 시 이 값을 올린다.
- 센서 OUT이 HIGH가 되면 `MOTION DETECTED`를 출력하고 LED를 켠다.
- HIGH가 끊긴 뒤 `HOLD_MS`가 지나면 해제한다. 센서의 HIGH 펄스가 짧게 끊기는 경우에도
  감지 상태가 깜빡이지 않게 하기 위한 것이다.
- `VERBOSE`를 `true`로 두면 2초마다 핀 상태를 출력한다. 배선 확인용.

실제 동작을 넣을 자리는 `onMotionStart()` / `onMotionEnd()` 두 함수다.

### 센서 조정

HC-SR501 보드의 가변저항 2개와 점퍼:

- Sx (Sensitivity): 감지 거리 3~7m
- Tx (Time delay): OUT이 HIGH로 유지되는 시간
- 점퍼: H(repeat trigger) 권장. 사람이 계속 움직이는 동안 HIGH를 유지한다.
  L(non-repeat)이면 HIGH 펄스가 짧게 끝나고 `HOLD_MS`에 의존하게 된다.

## 빌드 / 업로드

Arduino IDE에서는 보드를 `ESP32 Dev Module`, Upload Speed를 115200으로 두고 업로드한다.
esp32 보드 패키지가 없으면 환경설정의 추가 보드 관리자 URL에 아래를 넣고 보드 매니저에서 설치한다.

```
https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

arduino-cli 사용 시:

```
arduino-cli core install esp32:esp32
arduino-cli compile --fqbn esp32:esp32:esp32 .
arduino-cli upload -p /dev/cu.usbserial-XXXX --fqbn esp32:esp32:esp32:UploadSpeed=115200 .
```

`arduino-cli upload`는 재컴파일하지 않고 캐시된 바이너리를 올리므로,
코드를 수정했으면 반드시 `compile`을 먼저 돌린다.

업로드 보드레이트 기본값(921600)은 CH9102 계열 USB-시리얼 칩에서
`Unable to verify flash chip connection`으로 실패하는 경우가 있다. 115200으로 낮추면 된다.

## 시리얼 모니터

115200 baud.

```
=== ESP32 PIR Motion Sensor ===
PIR pin: GPIO16 / LED pin: GPIO2
warming up 5000 ms ...
ready. waiting for motion...
[6059 ms] MOTION DETECTED
[10449 ms] motion cleared
```

macOS에서 CLI로 읽을 때는 포트를 여는 fd에서 보드레이트를 설정해야 한다.
`stty`를 따로 실행한 뒤 `cat`을 하면 포트 재오픈 시 설정이 초기화되어 깨진 바이트만 읽힌다.

```
( exec < /dev/cu.usbserial-XXXX; stty 115200 raw -echo crtscts -hupcl; cat )
```

## 검증 환경

ESP32-D0WD-V3 rev3.1 (WROOM-32, 4MB flash, PSRAM 없음), CH9102 USB-시리얼.
HC-SR501 센서로 실제 감지 동작까지 확인했다.
