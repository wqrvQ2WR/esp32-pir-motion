/*
  ESP32 + PIR 인체감지센서 (HC-SR501) + 폰 알림

  - 센서 OUT -> GPIO 16
  - 감지하면 같은 와이파이에 UDP 브로드캐스트를 쏜다 -> 안드로이드 앱이 받아 알림
  - delay() 없이 millis() 기반 (루프가 막히지 않음)

  전선 위 형식: PIR|<기기이름>|<MOTION|CLEAR>|<uptimeMs>
  폰 IP를 적을 필요가 없다. 브로드캐스트라 같은 공유기면 알아서 닿는다.

  다만 폰이 절전(doze)에 들어가면 와이파이 칩이 브로드캐스트를 걸러버린다.
  그래서 앱이 60초마다 "PIRAPP|hello"를 브로드캐스트하고, 이 스케치는 그 보낸이 IP를
  기억해 두었다가 알림을 브로드캐스트 + 그 IP로 유니캐스트 양쪽으로 보낸다.

  와이파이 정보는 secrets.h 에 있다 (secrets.h.example 참고, 깃에는 안 올라감).

  집 밖(다른 와이파이, 데이터망)에서도 받으려면 인터넷 중계가 필요하다.
  그래서 감지 시 ntfy.sh 로도 HTTPS POST 한다. 폰의 ntfy 앱이 구글 푸시로 받으므로
  깊은 절전이든 LTE든 상관없이 도착한다. 토픽 이름은 secrets.h 에 있고 그 자체가 비밀번호다.

  보드: ESP32 Dev Module / 업로드 보드레이트 115200
  시리얼 모니터: 115200 baud
*/

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "secrets.h"

const int PIR_PIN   = 16;   // 인체감지센서 OUT
const int LED_PIN   = 2;    // 보드 내장 LED

const uint16_t NOTIFY_PORT = 8765;   // 안드로이드 앱이 듣는 포트

// 같은 와이파이 안에서 UDP로도 보낼지. ntfy 앱만 쓸 거면 false로 두면 알림이 겹치지 않는다.
const bool SEND_UDP = true;

// ntfy 푸시 사이 최소 간격. 센서는 몇 초마다 반응할 수 있어서 이게 없으면 알림 폭탄이 된다.
const unsigned long NTFY_COOLDOWN_MS = 60000;

// 감지 해제(CLEAR)까지 푸시할지. 보통은 감지만 받으면 충분하다.
const bool NTFY_ON_CLEAR = false;

// 감지 후 이 시간 동안은 "계속 감지중"으로 유지 (ms)
const unsigned long HOLD_MS = 3000;

// 전원 인가 후 센서 안정화 시간 (HC-SR501은 30~60초 권장, 테스트는 5초로)
const unsigned long WARMUP_MS = 5000;

// true면 2초마다 핀 상태를 찍어줌 (배선 확인용). 실사용 시 false로.
const bool VERBOSE = false;
const unsigned long VERBOSE_MS = 2000;

// 와이파이가 끊겼을 때 재접속 시도 간격
const unsigned long WIFI_RETRY_MS = 10000;

WiFiUDP udp;

// 앱이 알려온 폰 주소들. 유니캐스트로도 보내기 위해 기억해 둔다.
const int MAX_PHONES = 4;
const unsigned long PHONE_TTL_MS = 10UL * 60UL * 1000UL;   // 10분 동안 소식 없으면 잊는다
IPAddress phoneIp[MAX_PHONES];
unsigned long phoneSeen[MAX_PHONES] = {0};

bool motionState = false;
unsigned long lastMotionMs = 0;
unsigned long bootMs = 0;
unsigned long lastVerboseMs = 0;
unsigned long lastWifiTryMs = 0;
unsigned long lastNtfyMs = 0;
bool ntfyEverSent = false;
unsigned long skippedSinceNtfy = 0;
bool warmedUp = false;

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  bootMs = millis();

  Serial.println();
  Serial.println("=== ESP32 PIR Motion Sensor ===");
  Serial.printf("PIR pin: GPIO%d / LED pin: GPIO%d\n", PIR_PIN, LED_PIN);
  Serial.printf("device name: %s\n", DEVICE_NAME);

  connectWifi();
  udp.begin(NOTIFY_PORT);      // 앱의 hello 패킷을 받기 위해

  Serial.printf("warming up %lu ms ...\n", WARMUP_MS);
}

void loop() {
  unsigned long now = millis();

  keepWifiAlive(now);
  readHello(now);

  // 1) 워밍업 구간: 센서가 아직 안정화 전이라 오탐 많음
  if (!warmedUp) {
    if (now - bootMs >= WARMUP_MS) {
      warmedUp = true;
      Serial.println("ready. waiting for motion...");
    } else {
      digitalWrite(LED_PIN, (now / 300) % 2);
      return;
    }
  }

  // 2) 센서 읽기
  int raw = digitalRead(PIR_PIN);

  if (raw == HIGH) {
    lastMotionMs = now;

    if (!motionState) {
      motionState = true;
      digitalWrite(LED_PIN, HIGH);
      onMotionStart();
    }
  } else {
    if (motionState && (now - lastMotionMs >= HOLD_MS)) {
      motionState = false;
      digitalWrite(LED_PIN, LOW);
      onMotionEnd();
    }
  }

  // 3) 배선 확인용 주기 출력
  if (VERBOSE && (now - lastVerboseMs >= VERBOSE_MS)) {
    lastVerboseMs = now;
    Serial.printf("[%lu ms] pin=%d  motion=%s\n", now, raw, motionState ? "YES" : "no");
  }
}

// 사람이 감지된 순간
void onMotionStart() {
  Serial.printf("[%lu ms] MOTION DETECTED\n", millis());
  notifyPhone("MOTION");
}

// 감지가 끝난 순간
void onMotionEnd() {
  Serial.printf("[%lu ms] motion cleared\n", millis());
  notifyPhone("CLEAR");
}

// --- 와이파이 / 알림 전송 ---

void connectWifi() {
  Serial.printf("wifi: connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // 절전 모드면 전송이 늦어진다
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("wifi: connected, ip=%s  broadcast=%s:%u\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.broadcastIP().toString().c_str(),
                  NOTIFY_PORT);
  } else {
    Serial.println("wifi: FAILED (감지는 계속 동작, 전송만 안 됨)");
  }
  lastWifiTryMs = millis();
}

void keepWifiAlive(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED) return;
  if (now - lastWifiTryMs < WIFI_RETRY_MS) return;

  lastWifiTryMs = now;
  Serial.println("wifi: reconnecting...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void notifyPhone(const char* kind) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("  -> 전송 건너뜀 (와이파이 끊김)");
    return;
  }

  char msg[96];
  snprintf(msg, sizeof(msg), "PIR|%s|%s|%lu", DEVICE_NAME, kind, millis());

  if (SEND_UDP) {
    // 같은 공유기 안 모든 기기에 뿌린다. UDP라 유실될 수 있어 두 번 보낸다.
    for (int i = 0; i < 2; i++) {
      sendTo(WiFi.broadcastIP(), msg);
      delay(20);
    }

    // 앱이 알려온 폰에는 유니캐스트로도 보낸다 (절전 중엔 브로드캐스트가 걸러지므로)
    int sent = 0;
    for (int i = 0; i < MAX_PHONES; i++) {
      if (phoneSeen[i] == 0) continue;
      sendTo(phoneIp[i], msg);
      delay(20);
      sent++;
    }
    Serial.printf("  -> udp: %s (broadcast + 유니캐스트 %d대)\n", msg, sent);
  }

  bool isMotion = (strcmp(kind, "MOTION") == 0);
  if (isMotion || NTFY_ON_CLEAR) sendNtfy(isMotion);
}

/**
 * ntfy.sh 로 푸시를 보낸다. 집 밖에서도 받기 위한 경로.
 * 쿨다운 안에 다시 감지되면 보내지 않고 횟수만 세었다가 다음 푸시에 같이 알려 준다.
 */
void sendNtfy(bool isMotion) {
  unsigned long now = millis();

  if (ntfyEverSent && (now - lastNtfyMs < NTFY_COOLDOWN_MS)) {
    skippedSinceNtfy++;
    Serial.printf("  -> ntfy 생략 (쿨다운, 누적 %lu건)\n", skippedSinceNtfy);
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();        // 인증서 번들을 싣지 않는다. 토픽 이름이 접근 열쇠다.
  client.setTimeout(8000);

  HTTPClient http;
  String url = String("https://ntfy.sh/") + NTFY_TOPIC;
  if (!http.begin(client, url)) {
    Serial.println("  -> ntfy: 연결 준비 실패");
    return;
  }

  char body[160];
  if (skippedSinceNtfy > 0) {
    snprintf(body, sizeof(body), "%s 에서 움직임 감지 (쿨다운 동안 %lu건 더 있었음)",
             DEVICE_NAME, skippedSinceNtfy);
  } else {
    snprintf(body, sizeof(body), "%s 에서 움직임 감지", DEVICE_NAME);
  }

  http.addHeader("Content-Type", "text/plain");
  http.addHeader("Title", "PIR 움직임 감지");
  http.addHeader("Priority", isMotion ? "high" : "default");
  http.addHeader("Tags", "eyes");

  int code = http.POST((uint8_t*)body, strlen(body));
  if (code > 0 && code < 300) {
    Serial.printf("  -> ntfy: 전송 성공 (HTTP %d)\n", code);
    lastNtfyMs = now;
    ntfyEverSent = true;
    skippedSinceNtfy = 0;
  } else {
    Serial.printf("  -> ntfy: 실패 (HTTP %d) - 다음 감지 때 다시 시도\n", code);
  }
  http.end();
}

void sendTo(IPAddress ip, const char* msg) {
  udp.beginPacket(ip, NOTIFY_PORT);
  udp.print(msg);
  udp.endPacket();
}

// 앱이 보내는 "PIRAPP|hello"를 받아 그 폰 주소를 기억한다.
void readHello(unsigned long now) {
  int size = udp.parsePacket();
  while (size > 0) {
    char buf[64];
    int n = udp.read(buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = 0;
      if (strncmp(buf, "PIRAPP|", 7) == 0) {
        rememberPhone(udp.remoteIP(), now);
      }
    }
    size = udp.parsePacket();
  }
  forgetStalePhones(now);
}

void rememberPhone(IPAddress ip, unsigned long now) {
  // 이미 아는 폰이면 시각만 갱신
  for (int i = 0; i < MAX_PHONES; i++) {
    if (phoneSeen[i] != 0 && phoneIp[i] == ip) {
      phoneSeen[i] = now;
      return;
    }
  }
  // 빈 자리에 등록
  for (int i = 0; i < MAX_PHONES; i++) {
    if (phoneSeen[i] == 0) {
      phoneIp[i] = ip;
      phoneSeen[i] = now;
      Serial.printf("app: 폰 등록 %s\n", ip.toString().c_str());
      return;
    }
  }
  // 자리가 없으면 가장 오래된 것을 밀어낸다
  int oldest = 0;
  for (int i = 1; i < MAX_PHONES; i++) {
    if (phoneSeen[i] < phoneSeen[oldest]) oldest = i;
  }
  phoneIp[oldest] = ip;
  phoneSeen[oldest] = now;
  Serial.printf("app: 폰 교체 %s\n", ip.toString().c_str());
}

void forgetStalePhones(unsigned long now) {
  for (int i = 0; i < MAX_PHONES; i++) {
    if (phoneSeen[i] != 0 && now - phoneSeen[i] > PHONE_TTL_MS) {
      Serial.printf("app: 폰 만료 %s\n", phoneIp[i].toString().c_str());
      phoneSeen[i] = 0;
    }
  }
}
