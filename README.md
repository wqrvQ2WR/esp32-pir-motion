# ESP32 PIR Motion Sensor

ESP32에 PIR 인체감지센서(HC-SR501)를 붙여 사람 움직임을 감지하고, 같은 와이파이에 있는
안드로이드 폰으로 알림을 보내는 Arduino 스케치.
`delay()` 없이 `millis()` 기반으로 돌아가므로 다른 작업을 덧붙여도 루프가 막히지 않는다.

## 폰 알림

감지하면 UDP로 아래 한 줄을 뿌린다. 폰 IP를 적을 필요가 없다.

```
PIR|<기기이름>|<MOTION|CLEAR>|<uptimeMs>
```

받는 쪽은 UDP 8765를 듣는 안드로이드 앱이다(별도 프로젝트). 앱은 포그라운드 서비스로
소켓을 열어 두고, MOTION을 받으면 높은 중요도 알림을 띄운다.

## 집 밖에서 받기 (ntfy)

> 현재 구성: 라즈베리파이에 ntfy를 직접 올려 쓰고 있다.
> 서버 설정은 `capp/ntfy-pi`, 공개 주소는 Tailscale Funnel의 8443 포트.
> 공개 ntfy.sh를 쓰려면 `secrets.h`의 `NTFY_BASE_URL`만 `https://ntfy.sh`로 되돌리고
> `NTFY_TOKEN`을 비우면 된다.


UDP 브로드캐스트는 같은 공유기 안에서만 돈다. 다른 와이파이나 데이터망에서도 받으려면
인터넷 중계가 필요하므로, 감지 시 [ntfy.sh](https://ntfy.sh)로도 HTTPS POST 한다.
폰의 ntfy 앱이 구글 푸시로 받기 때문에 깊은 절전이든 LTE든 상관없이 도착한다.

폰 설정:

1. Play 스토어에서 ntfy 설치
2. 앱에서 Subscribe to topic
3. `secrets.h`의 `NTFY_TOPIC`과 같은 이름을 입력

토픽 이름을 아는 사람은 누구나 구독할 수 있다. 즉 토픽 이름이 곧 비밀번호이므로
추측 불가능한 긴 이름을 쓰고, 공개 저장소에 올리지 않는다(`secrets.h`는 gitignore 대상).

### 알림 폭탄 방지

HC-SR501은 사람이 근처에 있으면 몇 초 간격으로 계속 반응한다. 그대로 푸시하면
알림이 쏟아지고 ntfy 무료 한도도 넘기므로, `NTFY_COOLDOWN_MS`(기본 60초) 안에 들어온
감지는 보내지 않고 건수만 세었다가 다음 푸시 본문에 "쿨다운 동안 N건 더 있었음"으로 붙인다.

`SEND_UDP`를 `false`로 두면 UDP를 끄고 ntfy만 쓴다. 집 안에서 두 경로 모두 알림이 와서
겹치는 게 싫을 때 쓴다. `NTFY_ON_CLEAR`를 켜면 감지 해제도 푸시한다.

### 절전(doze) 한계 (UDP 경로)

폰이 깊은 절전에 들어가면 와이파이 칩이 브로드캐스트 패킷을 걸러버려 알림이 오지 않는다.
실측으로 확인한 동작이다(`dumpsys deviceidle force-idle` 상태에서 수신 실패).

완화책으로 앱이 60초마다 `PIRAPP|hello`를 브로드캐스트하고, 이 스케치는 그 보낸이 IP를
기억해 두었다가 알림을 **브로드캐스트와 유니캐스트 양쪽으로** 보낸다. 유니캐스트는 절전
필터를 통과할 확률이 높다. 여기에 앱에서 배터리 최적화 제외를 허용하면 더 안정적이다.

절전 상태에서도 100% 보장이 필요하면 FCM 같은 클라우드 푸시를 써야 한다.

## 와이파이 설정

`secrets.h.example`을 `secrets.h`로 복사하고 본인 값을 채운다.
`secrets.h`는 `.gitignore`에 있어 올라가지 않는다.

```c
#define WIFI_SSID "와이파이_이름"
#define WIFI_PASS "와이파이_비밀번호"
#define DEVICE_NAME "esp32-pir"
```

와이파이가 끊겨도 감지와 시리얼 출력은 계속 동작하고, 10초마다 재접속을 시도한다.

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

와이파이 스택을 넣으면 플래시 사용량이 20%에서 68%로, ntfy용 TLS까지 넣으면 78%로 올라간다.

실측 확인:

- 와이파이 접속, 감지 시 UDP 송출, ntfy HTTPS POST 성공(HTTP 200)
- 구독 측에서 메시지 수신 확인(title/priority/tags 포함)
- 쿨다운 동작 확인(연속 감지 6건 억제)
- 자체 호스팅 서버(파이) 전환 후 전구간 확인: 보드 시리얼 `ntfy: 전송 성공 (HTTP 200)` →
  서버에 메시지 저장 확인. 토큰 없는 요청은 403
