#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <WiFi.h>
#include <driver/i2s.h>
#include <WiFiManager.h>
#include <DHT.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>

// --- Định nghĩa chân I2S Micro (INMP441) ---
#define I2S_MIC_PORT I2S_NUM_0
#define I2S_MIC_SCK  4
#define I2S_MIC_WS   5
#define I2S_MIC_SD   6

// --- Định nghĩa chân I2S Loa (MAX98357) ---
#define I2S_SPK_BCLK 15
#define I2S_SPK_LRC  16
#define I2S_SPK_DIN  17

// --- Định nghĩa chân cảm biến ---
#define DHTPIN 7
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);
#define TOUCH_PIN 14
#define POT_PIN 10 // Biến trở 20k

// --- Định nghĩa chân cắm màn hình ---
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   -1 
#define TFT_DC   9
#define TFT_RST  8

SPIClass fspi(FSPI);
Adafruit_ST7789 tft = Adafruit_ST7789(&fspi, TFT_CS, TFT_DC, TFT_RST);

// --- Màu sắc Giao diện ---
#define COLOR_BG       0x0000  // Đen tuyền
#define COLOR_EYE      0x07FF  // Xanh Cyan
#define COLOR_USER_BUB 0x03E0  // Xanh lá đậm (ít dùng)
#define COLOR_AI_BUB   0x18E3  // Xám đen (Dark Gray)
#define COLOR_WHITE    0xFFFF
#define COLOR_YELLOW   0xFFE0

enum SystemState {
  STATE_IDLE,
  STATE_LISTENING,
  STATE_THINKING,
  STATE_SPEAKING
};
SystemState currentState = STATE_IDLE;

float currentTemp = 0.0;
float currentHum = 0.0;
unsigned long lastDhtTime = 0;
bool forceUIUpdate = true;

// Các hàm vẽ giao diện 
void printText(const char* text, int x, int y, uint16_t color, const GFXfont* font) {
  tft.setFont(font);
  tft.setTextColor(color);
  tft.setCursor(x, y);
  tft.setTextWrap(false);
  tft.print(text);
}

void drawEyes() {
  static unsigned long lastActionTime = 0;
  static int currentX = 50;
  static int currentH = 60;
  static bool needsRedraw = true;
  static bool isBlinking = false;
  static unsigned long blinkTime = 0;
  static int lastPrintedMinute = -1;

  static float lastPrintedTemp = -999.0;
  static float lastPrintedHum = -999.0;
  bool isPanelRedrawNeeded = false;

  if (forceUIUpdate) {
    needsRedraw = true;
    isPanelRedrawNeeded = true;
    lastPrintedMinute = -1;
    tft.fillScreen(COLOR_BG);
    forceUIUpdate = false;
  }

  // Xử lý nháy mắt (Mắt nhắm lại rồi mở ra sau 150ms)
  if (isBlinking && millis() - blinkTime > 150) {
    isBlinking = false;
    currentH = 60;
    needsRedraw = true;
  }

  // Cứ mỗi 1.5 - 3 giây sẽ random làm 1 hành động
  if (!isBlinking && millis() - lastActionTime > random(1500, 3000)) {
    lastActionTime = millis();
    int action = random(100);
    
    if (action < 40) {
      isBlinking = true;
      blinkTime = millis();
      currentH = 10;
    } else if (action < 60) {
      currentX = 30; // Liếc trái
    } else if (action < 80) {
      currentX = 70; // Liếc phải
    } else {
      currentX = 50; // Nhìn thẳng
    }
    needsRedraw = true;
  }

  // Cập nhật DHT
  if (millis() - lastDhtTime > 5000) {
    lastDhtTime = millis();
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      if (t != currentTemp || h != currentHum) {
         currentTemp = t;
         currentHum = h;
      }
    }
  }
  
  if (currentTemp != lastPrintedTemp || currentHum != lastPrintedHum) {
      isPanelRedrawNeeded = true;
  }

  // Vẽ mắt (chỉ xóa cục bộ khu vực mắt để không giật panel)
  if (needsRedraw) {
    tft.fillRect(0, 0, 240, 140, COLOR_BG); // Xóa nửa trên
    tft.fillRoundRect(currentX, 70 - (currentH/2), 50, currentH, 15, COLOR_EYE);
    tft.fillRoundRect(currentX + 90, 70 - (currentH/2), 50, currentH, 15, COLOR_EYE);
    needsRedraw = false;
  }

  // Đồng hồ & Panel
  struct tm timeinfo;
  bool gotTime = getLocalTime(&timeinfo, 0);
  if (gotTime && (timeinfo.tm_min != lastPrintedMinute || forceUIUpdate)) {
      isPanelRedrawNeeded = true;
  }

  // Vẽ Panel thông tin kết hợp
  if (isPanelRedrawNeeded && currentTemp > 0.0) {
    lastPrintedTemp = currentTemp;
    lastPrintedHum = currentHum;
    if (gotTime) lastPrintedMinute = timeinfo.tm_min;
    
    tft.fillRect(70, 0, 100, 30, COLOR_BG);
    
    // Nền panel (Xám đậm)
    tft.fillRoundRect(10, 145, 220, 85, 12, 0x2104);
    
    if (gotTime) {
      char timeStr[10];
      strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
      tft.setFont(&FreeSans12pt7b);
      tft.setTextColor(0xFFFF);
      tft.setCursor(85, 175); 
      tft.print(timeStr);
    }
    
    tft.setFont(&FreeSans9pt7b);
    
    // Nhiệt độ bên trái
    tft.fillCircle(25, 210, 7, 0xF800); 
    tft.fillRoundRect(22, 193, 7, 17, 3, 0xF800); 
    tft.setTextColor(0xFFE0); 
    tft.setCursor(40, 215);
    tft.printf("%.1f C", currentTemp);

    // Độ ẩm bên phải
    tft.fillCircle(135, 210, 7, 0x051D); 
    tft.fillTriangle(128, 210, 142, 210, 135, 196, 0x051D);
    tft.setTextColor(0x07E0); 
    tft.setCursor(150, 215);
    tft.printf("%.1f %%", currentHum);
  }
}

void drawEQBars(int energy) {
    static const uint16_t EQ_COLORS[24] = {
      0xF800, 0xF800, 0xF804, 0xF808, 
      0xF80C, 0xF810, 0xF814, 0xF818, 
      0xF81C, 0xF81F, 0xD81F, 0xB81F, 
      0x981F, 0x781F, 0x581F, 0x381F, 
      0x181F, 0x001F, 0x01FF, 0x03FF, 
      0x05FF, 0x07FF, 0x07FF, 0x07FF  
    };
    
    static float phase = 0;
    phase += 0.4;
    
    static int old_bar_height[24] = {0};
    
    int max_amp = map(energy, 0, 5000, 5, 80);
    if (max_amp > 80) max_amp = 80;
    if (max_amp < 5) max_amp = 5;

    for (int i = 0; i < 24; i++) {
        float wave = abs(sin(i * 0.5 + phase) * cos(i * 0.3 - phase * 0.7));
        float noise = (float)random(60, 100) / 100.0;
        float center_boost = 1.0 - abs(i - 11.5) / 12.0; 
        
        int target_h = 5 + max_amp * wave * noise * (0.5 + 0.8 * center_boost);
        if (target_h > 80) target_h = 80;
        
        int h = old_bar_height[i];
        
        if (target_h > h) h = target_h;
        else { h -= 4; if (h < 5) h = 5; }
        
        int x = i * 10 + 1; 
        int old_h = old_bar_height[i];
        
        if (h < old_h) tft.fillRect(x, 230 - old_h, 8, old_h - h, COLOR_BG);
        else if (h > old_h) tft.fillRect(x, 230 - h, 8, h - old_h, EQ_COLORS[i]);
        
        if (old_h == 0) tft.fillRect(x, 230 - 5, 8, 5, EQ_COLORS[i]); 
        
        old_bar_height[i] = h;
    }
}

void setup() {
  Serial.begin(115200);
  dht.begin();
  pinMode(TOUCH_PIN, INPUT);
  pinMode(POT_PIN, INPUT);

  // KHỞI TẠO SPI TRƯỚC THEO QUY TẮC CỦA LƯU Ý
  fspi.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS); 
  tft.init(240, 240, SPI_MODE3);
  tft.setRotation(2);
  tft.invertDisplay(true); // Cần thiết cho vài loại màn ST7789 để màu ko bị đảo (đen thành trắng)
  tft.fillScreen(COLOR_BG);

  printText("AI Assistant", 42, 100, COLOR_EYE, &FreeSans12pt7b);
  printText("San sang! Cham de thu am", 10, 130, COLOR_YELLOW, &FreeSans9pt7b);
  delay(2000);
  tft.fillScreen(COLOR_BG);
}

void loop() {
  bool isTouched = digitalRead(TOUCH_PIN);

  if (isTouched && currentState == STATE_IDLE) {
    currentState = STATE_LISTENING;
    tft.fillScreen(COLOR_BG);
    printText("Dang nghe...", 60, 120, COLOR_EYE, &FreeSans12pt7b);
    delay(200); // Debounce
  } else if (!isTouched && currentState == STATE_LISTENING) {
    currentState = STATE_IDLE;
    forceUIUpdate = true;
  }
  
  switch (currentState) {
    case STATE_IDLE:
      drawEyes(); // Vẽ con mắt liếc qua liếc lại + panel nhiệt độ
      delay(20);
      break;

    case STATE_LISTENING:
      // Vẽ EQ Bars chạy giả lập (chưa có âm thanh thật thì cho chạy random)
      drawEQBars(random(1000, 4000));
      delay(50);
      break;

    case STATE_THINKING:
      // (Để dành cho tương lai)
      break;
      
    case STATE_SPEAKING:
      // (Để dành cho tương lai)
      break;
  }
}