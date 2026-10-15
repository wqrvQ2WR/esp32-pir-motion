/*
  ESP32 + PIR 인체감지센서 (HC-SR501 등)
  - 센서 OUT -> GPIO 16
  - 상태 변화만 시리얼로 출력, 내장 LED로 표시
  - delay() 없이 millis() 기반 (다른 작업 추가해도 안 막힘)

  보드: ESP32-D0WD-V3 (WROOM-32, 4MB) / 업로드 보드레이트 115200
  시리얼 모니터: 115200 baud
*/

const int PIR_PIN   = 16;   // 인체감지센서 OUT
const int LED_PIN   = 2;    // 보드 내장 LED (없으면 외부 LED + 220옴)

// 감지 후 이 시간 동안은 "계속 감지중"으로 유지 (ms)
const unsigned long HOLD_MS = 3000;

// 전원 인가 후 센서 안정화 시간 (HC-SR501은 30~60초 권장, 테스트는 5초로)
const unsigned long WARMUP_MS = 5000;

// true면 2초마다 핀 상태를 찍어줌 (배선 확인용). 실사용 시 false로.
const bool VERBOSE = false;
const unsigned long VERBOSE_MS = 2000;

bool motionState = false;          // 현재 감지 상태
unsigned long lastMotionMs = 0;    // 마지막으로 HIGH를 본 시각
unsigned long bootMs = 0;
unsigned long lastVerboseMs = 0;
bool warmedUp = false;

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIR_PIN, INPUT);   // HC-SR501은 자체 풀다운 출력이라 INPUT으로 충분
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  bootMs = millis();

  Serial.println();
  Serial.println("=== ESP32 PIR Motion Sensor ===");
  Serial.printf("PIR pin: GPIO%d / LED pin: GPIO%d\n", PIR_PIN, LED_PIN);
  Serial.printf("warming up %lu ms ...\n", WARMUP_MS);
}

void loop() {
  unsigned long now = millis();

  // 1) 워밍업 구간: 센서가 아직 안정화 전이라 오탐 많음
  if (!warmedUp) {
    if (now - bootMs >= WARMUP_MS) {
      warmedUp = true;
      Serial.println("ready. waiting for motion...");
    } else {
      // 워밍업 중에는 LED 깜빡임
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
    // HIGH가 끊긴 뒤 HOLD_MS 지나면 해제 (깜빡임 방지)
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

// 사람이 감지된 순간 - 여기에 원하는 동작 넣기
void onMotionStart() {
  Serial.printf("[%lu ms] MOTION DETECTED\n", millis());
}

// 감지가 끝난 순간
void onMotionEnd() {
  Serial.printf("[%lu ms] motion cleared\n", millis());
}
