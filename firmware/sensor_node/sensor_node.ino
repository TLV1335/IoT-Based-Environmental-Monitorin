#include <Arduino.h>
#include <DHT.h>
#include <SoftwareSerial.h>
#include <LowPower.h>


// ============================================================
// 1. CHÂN KẾT NỐI
// ============================================================
#define DHTPIN 2
#define DHTTYPE DHT11
#define SHARP_LED 7
#define SHARP_OUT A0
#define LORA_RX 11
#define LORA_TX 10
#define LORA_M0 3
#define LORA_M1 4


DHT dht(DHTPIN, DHTTYPE);
SoftwareSerial lora(LORA_RX, LORA_TX);


// ============================================================
// 2. CẤU HÌNH (có thể thay đổi qua lệnh từ Gateway)
// ============================================================
unsigned long INTERVAL = 300;  // giây
int listenWindow = 3;          // giây
float deltaTemp = 0.5, deltaHumi = 2.0, deltaDust = 5.0;


// Ngưỡng nhiệt độ
float temp_safe_low = 18, temp_safe_high = 35;
float temp_warn_low = 10, temp_warn_high = 38;
float temp_danger_low = 0, temp_danger_high = 45;


// Ngưỡng độ ẩm
float humi_safe_low = 30, humi_safe_high = 65;
float humi_warn_low = 20, humi_warn_high = 80;
float humi_danger_low = 15, humi_danger_high = 90;


// Ngưỡng bụi
float dust_safe_high = 45, dust_warn_high = 80, dust_danger_high = 150;


// ============================================================
// 3. THỜI GIAN THỰC (đã sửa lỗi Deep Sleep)
// ============================================================
unsigned long timeElapsed = 0;   // giây tính từ epoch, được cộng dồn thủ công
unsigned long lastSentTime = 0;  // thời điểm gửi thành công cuối cùng


// ============================================================
// 4. DỮ LIỆU CẢM BIẾN
// ============================================================
float lastSentTemp = -99, lastSentHumi = -99, lastSentDust = -99;
enum AlertLevel { SAFE,
                  WARNING,
                  DANGER };
AlertLevel previousLevel = SAFE;


// ============================================================
// 5. MỐC GIỜ CỐ ĐỊNH (Ví dụ: "08:00,12:00,18:00")
// ============================================================
char fixedTimes[64] = "";
uint8_t fixedMinutes[10];
uint8_t fixedCount = 0;
int lastFixedMinute = -1;


// ============================================================
// 6. BUFFER LỆNH
// ============================================================
char cmdBuffer[32];
uint8_t cmdIdx = 0;


// ============================================================
// 7. HÀM TIỆN ÍCH
// ============================================================
void safeStrCpy(char *dest, const char *src, int maxLen) {
  strncpy(dest, src, maxLen - 1);
  dest[maxLen - 1] = '\0';
}


void trim(char *str) {
  char *p = str;
  while (*p == ' ' || *p == '\t') p++;
  memmove(str, p, strlen(p) + 1);
  for (int i = strlen(str) - 1; i >= 0; i--) {
    if (str[i] != ' ' && str[i] != '\t' && str[i] != '\r' && str[i] != '\n') break;
    str[i] = '\0';
  }
}


// ============================================================
// 8. ĐỌC CẢM BIẾN
// ============================================================
float readDust() {
    const int samples = 20;
    long sum = 0;
    for (int i=0; i<samples; i++) {
        digitalWrite(SHARP_LED, LOW);
        delayMicroseconds(280);
        sum += analogRead(SHARP_OUT);
        delayMicroseconds(40);
        digitalWrite(SHARP_LED, HIGH);
        delayMicroseconds(9680);
        delay(10);
    }
    int avgADC = sum / samples;
    float voltage = avgADC * (5.0 / 1024.0);


    static float lastDust = -1;
    float dust = (avgADC - 20) * 0.25;
    if (dust < 0) dust = 0;
   
    // Nếu có dữ liệu trước đó và sự thay đổi quá lớn (> 50% hoặc > 100), giới hạn
    if (lastDust > 0) {
        float change = abs(dust - lastDust);
        float limit = lastDust * 0.5; // thay đổi tối đa 50%
        if (change > limit) {
            // Nếu tăng quá nhiều, giữ lại giá trị cũ + một phần nhỏ
            dust = lastDust + (dust - lastDust) * 0.3; // chỉ cho phép thay đổi 30%
            if (dust > lastDust * 2) dust = lastDust * 2;
            if (dust < lastDust * 0.5) dust = lastDust * 0.5;
        }
    }
    lastDust = dust;
    return dust;
}


bool readDHT(float &t, float &h) {
  for (int i = 0; i < 3; i++) {
    t = dht.readTemperature();
    h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) return true;
    delay(50);
  }
  return false;
}


// ============================================================
// 9. CẢNH BÁO VÀ CHUYỂN ĐỔI
// ============================================================
AlertLevel getAlertLevel(float t, float h, float d) {
  if (t > temp_danger_high || t < temp_danger_low || h > humi_danger_high || h < humi_danger_low || d > dust_danger_high) return DANGER;
  if (t > temp_warn_high || t < temp_warn_low || h > humi_warn_high || h < humi_warn_low || d > dust_warn_high) return WARNING;
  return SAFE;
}


const char *levelToString(AlertLevel level) {
  switch (level) {
    case DANGER: return "DANGER";
    case WARNING: return "WARNING";
    default: return "SAFE";
  }
}


// ============================================================
// 10. XỬ LÝ MỐC GIỜ CỐ ĐỊNH (GMT+7)
// ============================================================
void parseFixedTimes() {
  fixedCount = 0;
  char buf[64];
  safeStrCpy(buf, fixedTimes, 64);
  char *p = buf;
  while (*p) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') break;
    int h = 0, m = 0;
    if (sscanf(p, "%d:%d", &h, &m) == 2) {
      fixedMinutes[fixedCount++] = (uint8_t)(h * 60 + m);
      if (fixedCount >= 10) break;
    }
    while (*p && *p != ',') p++;
    if (*p == ',') p++;
  }
}


bool isFixedTime() {
  if (timeElapsed == 0 || fixedCount == 0) return false;
  unsigned long localEpoch = timeElapsed + 7UL * 3600UL;  // GMT+7
  uint16_t nowMinutes = (localEpoch % 86400UL) / 60;
  if (nowMinutes == lastFixedMinute) return false;
  for (uint8_t i = 0; i < fixedCount; i++) {
    if (fixedMinutes[i] == nowMinutes) {
      lastFixedMinute = nowMinutes;
      return true;
    }
  }
  return false;
}


// ============================================================
// 11. ĐỌC 1 DÒNG TỪ LoRa (có timeout)
// ============================================================
bool readLoRaLine(char *buffer, int maxLen, unsigned long timeoutMs) {
  unsigned long start = millis();
  int idx = 0;
  while (millis() - start < timeoutMs) {
    if (lora.available()) {
      char c = lora.read();
      if (c == '\n' || c == '\r') {
        if (idx > 0) {
          buffer[idx] = '\0';
          trim(buffer);
          return true;
        }
      } else if (idx < maxLen - 1) {
        buffer[idx++] = c;
      }
    }
  }
  return false;
}


// ============================================================
// 12. XỬ LÝ LỆNH TỪ GATEWAY
// ============================================================
void processCommand(char *cmd) {
  trim(cmd);
  if (strncmp(cmd, "ACK:", 4) == 0) return;


  char *eq = strchr(cmd, '=');
  if (!eq) return;


  char *key = cmd;
  char *val = eq + 1;
  *eq = '\0';
  trim(key);
  trim(val);


  if (strncmp(key, "CMD:", 4) == 0) {
    key += 4;
  }


  // --- So sánh lệnh (ĐÃ XÓA CÁC LỆNH lora.println) ---
  if (strcmp(key, "INTERVAL") == 0) {
    INTERVAL = strtoul(val, NULL, 10);
    INTERVAL = constrain(INTERVAL, 8, 3600);
    // lora.println("ACK:INTERVAL"); <--- ĐÃ XÓA
  }
  else if (strcmp(key, "DELTA_T") == 0) {
    deltaTemp = atof(val);
    // lora.println("ACK:DELTA_T");  <--- ĐÃ XÓA
  }
  else if (strcmp(key, "DELTA_H") == 0) {
    deltaHumi = atof(val);
  }
  else if (strcmp(key, "DELTA_D") == 0) {
    deltaDust = atof(val);
  }
  else if (strcmp(key, "LISTEN") == 0) {
    listenWindow = strtoul(val, NULL, 10);
    listenWindow = constrain(listenWindow, 1, 5);
  }
  else if (strcmp(key, "TIME") == 0) {
    timeElapsed = strtoul(val, NULL, 10);  
  }
  else if (strcmp(key, "FIXED_TIMES") == 0) {
    safeStrCpy(fixedTimes, val, 64);
    parseFixedTimes();
    lastFixedMinute = -1;
  }
  // --- Ngưỡng (cũng xóa hết lora.println ở các dòng này) ---
  else if (strcmp(key, "TEMP_SAFE_L") == 0) { temp_safe_low = atof(val); }
  else if (strcmp(key, "TEMP_SAFE_H") == 0) { temp_safe_high = atof(val); }
  else if (strcmp(key, "TEMP_WARN_L") == 0) { temp_warn_low = atof(val); }
  else if (strcmp(key, "TEMP_WARN_H") == 0) { temp_warn_high = atof(val); }
  else if (strcmp(key, "TEMP_DANGER_L") == 0) { temp_danger_low = atof(val); }
  else if (strcmp(key, "TEMP_DANGER_H") == 0) { temp_danger_high = atof(val); }
 
  else if (strcmp(key, "HUMI_SAFE_L") == 0) { humi_safe_low = atof(val); }
  else if (strcmp(key, "HUMI_SAFE_H") == 0) { humi_safe_high = atof(val); }
  else if (strcmp(key, "HUMI_WARN_L") == 0) { humi_warn_low = atof(val); }
  else if (strcmp(key, "HUMI_WARN_H") == 0) { humi_warn_high = atof(val); }
  else if (strcmp(key, "HUMI_DANGER_L") == 0) { humi_danger_low = atof(val); }
  else if (strcmp(key, "HUMI_DANGER_H") == 0) { humi_danger_high = atof(val); }
 
  else if (strcmp(key, "DUST_SAFE_H") == 0) { dust_safe_high = atof(val); }
  else if (strcmp(key, "DUST_WARN_H") == 0) { dust_warn_high = atof(val); }
  else if (strcmp(key, "DUST_DANGER_H") == 0) { dust_danger_high = atof(val); }
}


// ============================================================
// 13. GỬI DỮ LIỆU + ACK (đã sửa lỗi đọc ACK)
// ============================================================


bool sendDataWithAck(float t, float h, float d, AlertLevel level, int maxRetries = 2) {
  // Dùng String để nối số thập phân (vì Uno không hỗ trợ %f trong snprintf)
  String payload = "DATA:" + String(t, 1) + "," + String(h, 0) + "," + String(d, 2) + "," + levelToString(level);


  for (int attempt = 0; attempt < maxRetries; attempt++) {
    lora.println(payload);
    Serial.print("Sent: ");
    Serial.println(payload);


    unsigned long waitStart = millis();


    // Kiên nhẫn chờ đủ 3 giây (3000ms). Có rác thì bỏ qua, chờ tiếp!
    while (millis() - waitStart < 3000) {
      char ackBuffer[16];


      // Cứ 100ms đọc 1 lần để không bị treo vòng lặp
      if (readLoRaLine(ackBuffer, 16, 100)) {
        if (strcmp(ackBuffer, "ACK:DATA") == 0) {
          Serial.println("ACK received");
          return true;  // Nghe chuẩn ACK -> Thoát thành công!
        } else {
          // Bắt được rác thì in ra theo dõi, KHÔNG TỪ BỎ!
          Serial.print("Ignored trash: ");
          Serial.println(ackBuffer);
        }
      }
    }


    // Hết 3 giây mà vẫn chưa thấy ACK mới kết luận là thất bại
    Serial.println("No ACK, retry...");
  }
  return false;
}


// ============================================================
// 14. SETUP
// ============================================================
void setup() {
  Serial.begin(9600);


  // 1. DHT11 TRƯỚC
  dht.begin();
  delay(100);


  // 2. Sharp
  pinMode(SHARP_LED, OUTPUT);
  digitalWrite(SHARP_LED, HIGH);


  // 3. LoRa SAU CÙNG
  pinMode(LORA_M0, OUTPUT);
  digitalWrite(LORA_M0, LOW);
  pinMode(LORA_M1, OUTPUT);
  digitalWrite(LORA_M1, LOW);
  lora.begin(9600);


  INTERVAL = constrain(INTERVAL, 30, 7200);
  listenWindow = 5;
}


// ============================================================
// 15. VÒNG LẶP CHÍNH (đã sửa lỗi thời gian)
// ============================================================


void loop() {
  unsigned long loopStartMillis = millis();


  digitalWrite(LORA_M0, LOW);
  digitalWrite(LORA_M1, LOW);
  delay(50);


  // ----- Đọc cảm biến -----
  float t = -99, h = -99;
  bool dhtOK = readDHT(t, h);
  float d = readDust();


  Serial.print("t=");
  Serial.print(t);
  Serial.print(" h=");
  Serial.println(h);


  if (dhtOK) {


    AlertLevel currentLevel = getAlertLevel(t, h, d);


    bool deltaChange =
      (fabs(t - lastSentTemp) >= deltaTemp) ||
      (fabs(h - lastSentHumi) >= deltaHumi) ||
      (fabs(d - lastSentDust) >= deltaDust);


    bool intervalDue = (timeElapsed - lastSentTime >= INTERVAL);
    bool fixedTimeDue = isFixedTime();


    bool shouldSend = false;


    if (currentLevel != previousLevel)
      shouldSend = true;
    else if (fixedTimeDue)
      shouldSend = true;
    else if (currentLevel >= WARNING && (deltaChange || intervalDue))
      shouldSend = true;
    else if (currentLevel == SAFE && (deltaChange || intervalDue))
      shouldSend = true;


    if (shouldSend) {


      // ----- Gửi dữ liệu -----
      String payload =
        "DATA:" +
        String(t, 1) + "," +
        String(h, 0) + "," +
        String(d, 2) + "," +
        levelToString(currentLevel);


      lora.println(payload);


      Serial.print("Sent: ");
      Serial.println(payload);


      bool ackReceived = false;


      unsigned long listenStart = millis();


      while (millis() - listenStart < (unsigned long)(listenWindow * 1000)) {


        if (lora.available()) {


          String line = lora.readStringUntil('\n');
          line.trim();


          if (line == "ACK:DATA") {


            ackReceived = true;
            Serial.println("ACK received");


          }
          else if (line.startsWith("CMD:")) {


            Serial.print("Processing: ");
            Serial.println(line);


            processCommand((char *)line.c_str());


          }
        }
      }


      if (ackReceived) {


        lastSentTemp = t;
        lastSentHumi = h;
        lastSentDust = d;
        lastSentTime = timeElapsed;
        previousLevel = currentLevel;


      } else {


        Serial.println("No ACK, will retry later");


      }


    }


  } else {


    Serial.println("DHT error, skip send");


  }


  // ----- Đưa LoRa về Sleep -----
  digitalWrite(LORA_M0, HIGH);
  digitalWrite(LORA_M1, HIGH);


  Serial.flush();


  // ----- Cập nhật thời gian -----
  unsigned long awakeTime = (millis() - loopStartMillis) / 1000;


  if (timeElapsed > 0) {
    timeElapsed += awakeTime;
    timeElapsed += 8;
  }


  // ----- Deep Sleep -----
  LowPower.powerDown(SLEEP_8S, ADC_OFF, BOD_OFF);
}

