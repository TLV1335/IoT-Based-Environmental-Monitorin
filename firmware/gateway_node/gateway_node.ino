#include <Arduino.h>
#include <WiFi.h>
#include <FirebaseESP32.h>
#include <UniversalTelegramBot.h>
#include <WiFiClientSecure.h>
#include <LiquidCrystal.h>
#include <HTTPClient.h>
#include <time.h>
#include <Preferences.h>

// ===== CẤU HÌNH CHUNG =====
#define WIFI_SSID "Bao Phuc"
#define WIFI_PASSWORD "baophuc142014"

#define FIREBASE_HOST "iot-based-environmental-78fcd-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "qVrScJGX6SSB4NXlwhgitGnfEPpyD9eKr0JvYz5t"

#define BOT_TOKEN "8844882764:AAHrUiGHfzrs0UzjnTQkJo1EPy_qyCnK9FU"
#define CHAT_ID "-1003767248274"

#define LORA_RX 16
#define LORA_TX 17

#define RED_LED 27
#define YELLOW_LED 13
#define GREEN_LED 18
#define BUZZER_PIN 26

// ===== ĐỐI TƯỢNG TOÀN CỤC =====
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

WiFiClientSecure clientSecure;
UniversalTelegramBot bot(BOT_TOKEN, clientSecure);

LiquidCrystal lcd(19, 21, 22, 23, 25, 33);
Preferences prefs;

// ===== BIẾN TRẠNG THÁI =====
unsigned long lastAlertTime = 0;
const unsigned long ALERT_WARN_MS = 300000;
const unsigned long ALERT_DANGER_MS = 120000;

int previousLevel = -1;
bool buzzerActive = false;

// Cache cấu hình (được cập nhật nền từ Firebase)
struct CachedConfig {
  int interval = 300;
  String fixedTimes = "";
  int listen = 3;
  float deltaTemp = 0.5, deltaHumi = 2.0, deltaDust = 5.0;
  float temp_safe_l = 18, temp_safe_h = 35;
  float temp_warn_l = 10, temp_warn_h = 38;
  float temp_dang_l = 0, temp_dang_h = 45;
  float humi_safe_l = 30, humi_safe_h = 65;
  float humi_warn_l = 20, humi_warn_h = 80;
  float humi_dang_l = 15, humi_dang_h = 90;
  float dust_safe = 45, dust_warn = 80, dust_dang = 150;
  String buzzerMode = "snooze";
  int snoozeDur = 600;
} cachedConfig;

// Cache trạng thái snooze (đọc từ Firebase định kỳ)
bool snoozeActive = false;

// Cờ bẩn cho việc ghi Flash (debounce)
bool configDirty = false;
unsigned long lastConfigChange = 0;
const unsigned long FLASH_DEBOUNCE_MS = 30000;  // Ghi Flash sau 30s không đổi

// Timer
unsigned long lastConfigFetch = 0;
const unsigned long CONFIG_FETCH_INTERVAL = 30000;
unsigned long lastPeriodicCheck = 0;
const unsigned long PERIODIC_CHECK_INTERVAL = 2000;  // 2 giây

// ===== PROTOTYPES =====
void connectWiFi();
void setupTime();
void fetchConfigFromFirebase();
void loadConfigFromFlash();
void saveConfigToFlash();
void syncConfigToNode();
void parseLoRaData(String data);
void handleAlert(int level, float t, float h, float d);
void pushToFirebase(float t, float h, float d, String levelStr);
void sendTimeToNode();
void checkAndSendCommand();
void checkSnooze();

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n=== GATEWAY START ==="));

  pinMode(RED_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(RED_LED, LOW);
  digitalWrite(YELLOW_LED, LOW);
  digitalWrite(GREEN_LED, LOW);
  noTone(BUZZER_PIN);  // 🔥 FIX 2: Tắt còi thụ động

  lcd.begin(16, 2);
  lcd.print(F("Gateway Start..."));

  Serial2.begin(9600, SERIAL_8N1, LORA_RX, LORA_TX);

  connectWiFi();
  setupTime();

  clientSecure.setInsecure();

  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Firebase.setReadTimeout(fbdo, 1000 * 60);

  loadConfigFromFlash();
  if (WiFi.status() == WL_CONNECTED) {
    fetchConfigFromFirebase();
  }

  Serial.println(F("=== GATEWAY READY ==="));
  lcd.clear();
  lcd.print(F("Gateway Ready"));
  delay(1500);
}

// ===== LOOP =====
void loop() {
  // 1. Giữ WiFi
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(1000);
  }

  if (millis() - lastConfigFetch > CONFIG_FETCH_INTERVAL) {
    fetchConfigFromFirebase();
    lastConfigFetch = millis();
  }

  if (millis() - lastPeriodicCheck > PERIODIC_CHECK_INTERVAL) {
    checkSnooze();
    checkAndSendCommand();
    lastPeriodicCheck = millis();
  }
  // ============================================

  // 4. Ghi Flash nếu cấu hình đã ổn định
  if (configDirty && (millis() - lastConfigChange > FLASH_DEBOUNCE_MS)) {
    saveConfigToFlash();
    configDirty = false;
  }

  // 5. Nhận dữ liệu LoRa
  if (Serial2.available()) {
    String data = Serial2.readStringUntil('\n');
    data.trim();
    if (data.length() > 10) {
      parseLoRaData(data); // Đảm bảo bạn đã thêm 3 dòng log RAW mình gợi ý lúc nãy ở đầu hàm này nhé!
    }
  }

  delay(20);
}

// ============================================================
// PARSE DỮ LIỆU TỪ NODE 1
// ============================================================
void parseLoRaData(String data) {
  Serial.print(F("[LORA RAW] Gateway vừa nhận được: '"));
  Serial.print(data);
  Serial.println(F("'"));

  if (!data.startsWith("DATA:")) {
    if (data.startsWith("ACK:")) {
      Serial.println("[LORA] Node ACK: " + data);
    } else {
      Serial.println("[LORA-ERROR] Chuỗi bị loại vì không bắt đầu bằng DATA:");
    }
    return;
  }

  data = data.substring(5);
  Serial.print(F("[LORA] Nhận DATA: "));
  Serial.println(data);

  int c1 = data.indexOf(',');
  int c2 = data.indexOf(',', c1 + 1);
  int c3 = data.indexOf(',', c2 + 1);

  if (c1 == -1 || c2 == -1 || c3 == -1) {
    Serial.println(F("[LORA] Sai định dạng"));
    return;
  }

  float t = data.substring(0, c1).toFloat();
  float h = data.substring(c1 + 1, c2).toFloat();
  float d = data.substring(c2 + 1, c3).toFloat();
  String levelStr = data.substring(c3 + 1);
  levelStr.trim();

  int level = 0;
  if (levelStr == "DANGER") level = 2;
  else if (levelStr == "WARNING") level = 1;

  // 🔥 FIX QUAN TRỌNG: Gửi ACK và chờ LoRa truyền xong qua không khí
  Serial2.println("ACK:DATA");
  Serial2.flush();
  delay(150); // Chờ 150ms để đảm bảo module LoRa đã phát xong sóng gói ACK này
  Serial.println(F("[LORA] Đã gửi ACK:DATA và chờ LoRa truyền xong"));

  // Tiếp tục các tác vụ khác
  sendTimeToNode();
  syncConfigToNode();
  pushToFirebase(t, h, d, levelStr);
  handleAlert(level, t, h, d);
}

// ===== FIREBASE =====
void pushToFirebase(float t, float h, float d, String level) {
  if (!Firebase.ready()) return;

  // LATEST
  FirebaseJson latest;
  latest.set("nhiet_do", t);
  latest.set("do_am", h);
  latest.set("bui_min", d);
  latest.set("level", level);
  latest.set("timestamp", String(time(nullptr)));

  Firebase.setJSON(fbdo, "/IoT_Based_Environmental/sensor/latest", latest);

  // HISTORY
  FirebaseJson hist;
  hist.set("nhiet_do", t);
  hist.set("do_am", h);
  hist.set("bui_min", d);
  hist.set("level", level);
  hist.set("timestamp", String(time(nullptr)));

  Firebase.pushJSON(fbdo, "/IoT_Based_Environmental/history", hist);

  // TRẠNG THÁI THIẾT BỊ
  Firebase.setString(fbdo, "/devices/node/status", "online");
  Firebase.setInt(fbdo, "/devices/node/last_seen", time(nullptr));
}

// ===== CẢNH BÁO =====
void handleAlert(int level, float t, float h, float d) {
  String msg = "";
  String status = "SAFE";

  digitalWrite(RED_LED, level == 2);
  digitalWrite(YELLOW_LED, level == 1);
  digitalWrite(GREEN_LED, level == 0);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.printf("T:%.1f H:%.0f", t, h);
  lcd.setCursor(0, 1);
  lcd.printf("D:%.1f ", d);

  if (level == 2) {
    status = "DANGER";
    msg = "🔴 NGUY HIEM!\n";
    msg += "Nhiet do: " + String(t, 1) + "°C\n";
    msg += "Do am: " + String(h, 0) + "%\n";
    msg += "Bui: " + String(d, 1) + " ug/m3";

    if (!snoozeActive) {
      tone(BUZZER_PIN, 1000);  // 🔥 FIX 2: Bật còi thụ động
      buzzerActive = true;
    }
  } else if (level == 1) {
    status = "WARNING";
    msg = "🟡 CANH BAO!\n";
    msg += "Nhiet do: " + String(t, 1) + "°C\n";
    msg += "Do am: " + String(h, 0) + "%\n";
    msg += "Bui: " + String(d, 1) + " ug/m3";
    noTone(BUZZER_PIN);  // 🔥 FIX 2
    buzzerActive = false;
  } else {
    status = "SAFE";
    noTone(BUZZER_PIN);  // 🔥 FIX 2
    buzzerActive = false;
  }
  lcd.print(status);

  unsigned long interval = (level == 2) ? ALERT_DANGER_MS : ALERT_WARN_MS;
  if (level > 0 && (level != previousLevel || millis() - lastAlertTime > interval)) {
    if (WiFi.status() == WL_CONNECTED) {
      if (bot.sendMessage(CHAT_ID, msg, "")) {
        lastAlertTime = millis();
        Serial.println(F("[TELEGRAM] OK"));
      }
    }
  }
  previousLevel = level;
}

// ===== GỬI TIMESTAMP =====
void sendTimeToNode() {
  String cmd = "CMD:TIME=" + String(time(nullptr));
  Serial2.println(cmd);
}

// ===== ĐỒNG BỘ CẤU HÌNH (dùng cache) =====
void syncConfigToNode() {
  static CachedConfig lastSent;

  auto syncInt = [&](int &flashVal, int cacheVal, const char *prefix) {
    if (cacheVal != flashVal) {
      flashVal = cacheVal;
      Serial2.println(String(prefix) + String(cacheVal));
      delay(50); // 🔥 FIX: Giảm từ 500ms xuống 50ms để không làm tắc nghẽn LoRa
    }
  };

  auto syncFloat = [&](float &flashVal, float cacheVal, const char *prefix) {
    if (fabsf(cacheVal - flashVal) > 0.001f) {
      flashVal = cacheVal;
      Serial2.println(String(prefix) + String(cacheVal, 2));
      delay(50); // 🔥 FIX: Giảm từ 500ms xuống 50ms
    }
  };

  auto syncString = [&](String &flashVal, String cacheVal, const char *prefix) {
    if (cacheVal != flashVal) {
      flashVal = cacheVal;
      Serial2.println(String(prefix) + cacheVal);
      delay(50); // 🔥 FIX: Giảm từ 500ms xuống 50ms
    }
  };

  syncInt(lastSent.interval, cachedConfig.interval, "CMD:INTERVAL=");
  syncString(lastSent.fixedTimes, cachedConfig.fixedTimes, "CMD:FIXED_TIMES=");
  syncInt(lastSent.listen, cachedConfig.listen, "CMD:LISTEN=");
  syncFloat(lastSent.deltaTemp, cachedConfig.deltaTemp, "CMD:DELTA_T=");
  syncFloat(lastSent.deltaHumi, cachedConfig.deltaHumi, "CMD:DELTA_H=");
  syncFloat(lastSent.deltaDust, cachedConfig.deltaDust, "CMD:DELTA_D=");

  syncFloat(lastSent.temp_safe_l, cachedConfig.temp_safe_l, "CMD:TEMP_SAFE_L=");
  syncFloat(lastSent.temp_safe_h, cachedConfig.temp_safe_h, "CMD:TEMP_SAFE_H=");
  syncFloat(lastSent.temp_warn_l, cachedConfig.temp_warn_l, "CMD:TEMP_WARN_L=");
  syncFloat(lastSent.temp_warn_h, cachedConfig.temp_warn_h, "CMD:TEMP_WARN_H=");
  syncFloat(lastSent.temp_dang_l, cachedConfig.temp_dang_l, "CMD:TEMP_DANGER_L=");
  syncFloat(lastSent.temp_dang_h, cachedConfig.temp_dang_h, "CMD:TEMP_DANGER_H=");

  syncFloat(lastSent.humi_safe_l, cachedConfig.humi_safe_l, "CMD:HUMI_SAFE_L=");
  syncFloat(lastSent.humi_safe_h, cachedConfig.humi_safe_h, "CMD:HUMI_SAFE_H=");
  syncFloat(lastSent.humi_warn_l, cachedConfig.humi_warn_l, "CMD:HUMI_WARN_L=");
  syncFloat(lastSent.humi_warn_h, cachedConfig.humi_warn_h, "CMD:HUMI_WARN_H=");
  syncFloat(lastSent.humi_dang_l, cachedConfig.humi_dang_l, "CMD:HUMI_DANGER_L=");
  syncFloat(lastSent.humi_dang_h, cachedConfig.humi_dang_h, "CMD:HUMI_DANGER_H=");

  syncFloat(lastSent.dust_safe, cachedConfig.dust_safe, "CMD:DUST_SAFE_H=");
  syncFloat(lastSent.dust_warn, cachedConfig.dust_warn, "CMD:DUST_WARN_H=");
  syncFloat(lastSent.dust_dang, cachedConfig.dust_dang, "CMD:DUST_DANGER_H=");
}

// ===== ĐỌC CẤU HÌNH TỪ FIREBASE =====
void fetchConfigFromFirebase() {
  if (!Firebase.ready()) return;

  if (!Firebase.getJSON(fbdo, "/IoT_Based_Environmental/config")) {
    return;
  }

  FirebaseJson &json = fbdo.jsonObject();
  FirebaseJsonData result;
  bool changed = false;

  auto checkInt = [&](const char *path, int &dest) {
    if (json.get(result, path) && result.success) {
      int val = result.intValue;
      if (val != dest) {
        dest = val;
        changed = true;
      }
    }
  };
  auto checkFloat = [&](const char *path, float &dest) {
    if (json.get(result, path) && result.success) {
      float val = result.floatValue;
      if (fabsf(val - dest) > 0.001f) {
        dest = val;
        changed = true;
      }
    }
  };
  auto checkString = [&](const char *path, String &dest) {
    if (json.get(result, path) && result.success) {
      String val = result.stringValue;
      if (val != dest) {
        dest = val;
        changed = true;
      }
    }
  };

  checkInt("send_interval", cachedConfig.interval);
  checkString("fixed_times", cachedConfig.fixedTimes);
  checkInt("listen_window", cachedConfig.listen);
  checkFloat("delta/temp", cachedConfig.deltaTemp);
  checkFloat("delta/humi", cachedConfig.deltaHumi);
  checkFloat("delta/dust", cachedConfig.deltaDust);

  checkFloat("thresholds/temp_safe_low", cachedConfig.temp_safe_l);
  checkFloat("thresholds/temp_safe_high", cachedConfig.temp_safe_h);
  checkFloat("thresholds/temp_warn_low", cachedConfig.temp_warn_l);
  checkFloat("thresholds/temp_warn_high", cachedConfig.temp_warn_h);
  checkFloat("thresholds/temp_danger_low", cachedConfig.temp_dang_l);
  checkFloat("thresholds/temp_danger_high", cachedConfig.temp_dang_h);

  checkFloat("thresholds/humi_safe_low", cachedConfig.humi_safe_l);
  checkFloat("thresholds/humi_safe_high", cachedConfig.humi_safe_h);
  checkFloat("thresholds/humi_warn_low", cachedConfig.humi_warn_l);
  checkFloat("thresholds/humi_warn_high", cachedConfig.humi_warn_h);
  checkFloat("thresholds/humi_danger_low", cachedConfig.humi_dang_l);
  checkFloat("thresholds/humi_danger_high", cachedConfig.humi_dang_h);

  checkFloat("thresholds/dust_safe_high", cachedConfig.dust_safe);
  checkFloat("thresholds/dust_warn_high", cachedConfig.dust_warn);
  checkFloat("thresholds/dust_danger_high", cachedConfig.dust_dang);

  checkString("buzzer/mode", cachedConfig.buzzerMode);
  checkInt("buzzer/snooze_duration", cachedConfig.snoozeDur);

  if (changed) {
    configDirty = true;
    lastConfigChange = millis();
  }
}

// ===== LỆNH ĐẶC BIỆT =====
void checkAndSendCommand() {
  if (!Firebase.ready()) return;
  // 🔥 FIX 1: Sửa lại đường dẫn lệnh cho khớp với Web
  if (Firebase.getBool(fbdo, "/IoT_Based_Environmental/commands/pending") && fbdo.boolData()) {
    String cmd = Firebase.getString(fbdo, "/IoT_Based_Environmental/commands/command") ? fbdo.stringData() : "";
    if (cmd.length() > 0) {
      Serial2.println(cmd);
      Firebase.setBool(fbdo, "/IoT_Based_Environmental/commands/pending", false);
      Firebase.setString(fbdo, "/IoT_Based_Environmental/commands/command", "");
    }
  }
}

// ===== SNOOZE =====
void checkSnooze() {
  if (!Firebase.ready()) return;

  bool active = false;
  // 🔥 FIX 1: Sửa lại đường dẫn Snooze cho khớp với Web
  if (Firebase.getBool(fbdo, "/IoT_Based_Environmental/snooze/active") && fbdo.boolData()) {
    time_t start = Firebase.getInt(fbdo, "/IoT_Based_Environmental/snooze/start_time") ? fbdo.intData() : 0;
    time_t dur = Firebase.getInt(fbdo, "/IoT_Based_Environmental/snooze/duration") ? fbdo.intData() : 0;

    if (time(nullptr) - start <= dur) {
      active = true;
    } else {
      Firebase.setBool(fbdo, "/IoT_Based_Environmental/snooze/active", false);
    }
  }

  if (active != snoozeActive) {
    snoozeActive = active;
    if (!active && previousLevel == 2 && !buzzerActive) {
      tone(BUZZER_PIN, 1000);  // 🔥 FIX 2: Bật lại còi nếu hết Snooze mà vẫn nguy hiểm
      buzzerActive = true;
    }
  }

  if (snoozeActive && buzzerActive) {
    noTone(BUZZER_PIN);  // 🔥 FIX 2: Tắt còi khi Web ra lệnh Snooze
    buzzerActive = false;
  }
}

// ===== WIFI & NTP =====
void connectWiFi() {
  lcd.clear();
  lcd.print(F("WiFi Connect..."));
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lcd.setCursor(0, 1);
  lcd.print(F(WIFI_SSID));
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F(" OK"));
    lcd.clear();
    lcd.print(F("WiFi OK"));
  } else {
    Serial.println(F(" FAIL"));
    lcd.clear();
    lcd.print(F("WiFi FAIL"));
  }
}

void setupTime() {
  Serial.print(F("Đang đồng bộ thời gian (NTP)"));
  
  // 🔥 FIX: Chờ 500ms để DNS ổn định sau khi WiFi vừa kết nối
  delay(500); 
  
  configTime(7 * 3600, 0, "time.google.com", "pool.ntp.org", "time.windows.com");

  int timeout = 0;
  while (time(nullptr) < 100000) {
    Serial.print(".");
    delay(500);
    timeout++;
    if (timeout > 30) {
      Serial.println(F(" Time FAIL! (Chạy tạm thời gian ảo)"));
      break;
    }
  }

  if (time(nullptr) > 100000) {
    Serial.println(F(" OK"));
  }
}

// ===== FLASH =====
void loadConfigFromFlash() {
  prefs.begin("config", true);
  cachedConfig.interval = prefs.getInt("interval", 300);
  cachedConfig.fixedTimes = prefs.getString("fixedTimes", "");
  cachedConfig.listen = prefs.getInt("listen", 3);
  cachedConfig.deltaTemp = prefs.getFloat("dTemp", 0.5);
  cachedConfig.deltaHumi = prefs.getFloat("dHumi", 2.0);
  cachedConfig.deltaDust = prefs.getFloat("dDust", 5.0);
  cachedConfig.temp_safe_l = prefs.getFloat("tSL", 18);
  cachedConfig.temp_safe_h = prefs.getFloat("tSH", 35);
  cachedConfig.temp_warn_l = prefs.getFloat("tWL", 10);
  cachedConfig.temp_warn_h = prefs.getFloat("tWH", 38);
  cachedConfig.temp_dang_l = prefs.getFloat("tDL", 0);
  cachedConfig.temp_dang_h = prefs.getFloat("tDH", 45);
  cachedConfig.humi_safe_l = prefs.getFloat("hSL", 30);
  cachedConfig.humi_safe_h = prefs.getFloat("hSH", 65);
  cachedConfig.humi_warn_l = prefs.getFloat("hWL", 20);
  cachedConfig.humi_warn_h = prefs.getFloat("hWH", 80);
  cachedConfig.humi_dang_l = prefs.getFloat("hDL", 15);
  cachedConfig.humi_dang_h = prefs.getFloat("hDH", 90);
  cachedConfig.dust_safe = prefs.getFloat("dSH", 45);
  cachedConfig.dust_warn = prefs.getFloat("dWH", 80);
  cachedConfig.dust_dang = prefs.getFloat("dDH", 150);
  cachedConfig.buzzerMode = prefs.getString("buzMode", "snooze");
  cachedConfig.snoozeDur = prefs.getInt("snoozeDur", 600);
  prefs.end();
}

void saveConfigToFlash() {
  prefs.begin("config", false);
  prefs.putInt("interval", cachedConfig.interval);
  prefs.putString("fixedTimes", cachedConfig.fixedTimes);
  prefs.putInt("listen", cachedConfig.listen);
  prefs.putFloat("dTemp", cachedConfig.deltaTemp);
  prefs.putFloat("dHumi", cachedConfig.deltaHumi);
  prefs.putFloat("dDust", cachedConfig.deltaDust);
  prefs.putFloat("tSL", cachedConfig.temp_safe_l);
  prefs.putFloat("tSH", cachedConfig.temp_safe_h);
  prefs.putFloat("tWL", cachedConfig.temp_warn_l);
  prefs.putFloat("tWH", cachedConfig.temp_warn_h);
  prefs.putFloat("tDL", cachedConfig.temp_dang_l);
  prefs.putFloat("tDH", cachedConfig.temp_dang_h);
  prefs.putFloat("hSL", cachedConfig.humi_safe_l);
  prefs.putFloat("hSH", cachedConfig.humi_safe_h);
  prefs.putFloat("hWL", cachedConfig.humi_warn_l);
  prefs.putFloat("hWH", cachedConfig.humi_warn_h);
  prefs.putFloat("hDL", cachedConfig.humi_dang_l);
  prefs.putFloat("hDH", cachedConfig.humi_dang_h);
  prefs.putFloat("dSH", cachedConfig.dust_safe);
  prefs.putFloat("dWH", cachedConfig.dust_warn);
  prefs.putFloat("dDH", cachedConfig.dust_dang);
  prefs.putString("buzMode", cachedConfig.buzzerMode);
  prefs.putInt("snoozeDur", cachedConfig.snoozeDur);
  prefs.end();
}