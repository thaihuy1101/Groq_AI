#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <WiFi.h>
#include <driver/i2s.h>
#include <AudioGeneratorMP3.h>
#include <WiFiManager.h>
#include <AudioOutputI2S.h>
#include <DHT.h>

#define DHTPIN 7
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

float currentTemp = 0.0;
float currentHum = 0.0;
unsigned long lastDhtTime = 0;
bool forceUIUpdate = true;


#include <AudioFileSourcePROGMEM.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/base64.h>
#include <ArduinoJson.h>
#include "AudioFileSourceHTTPSStream.h"
#include <AudioFileSourceBuffer.h>
#include "huawei_logo.h"

// --- Thông tin WiFi & Server ---
// Tên WiFi bị xóa vì dùng WiFiManager

// Thay chữ X bằng IP thật của máy tính (Gõ ipconfig trong terminal để xem IPv4)
String SERVER_URL = "https://esp32-ai-server-3k8t.onrender.com/chat"; 
String lastAiTextGlobal = "";

// --- Định nghĩa chân I2S Micro (INMP441) ---
#define I2S_MIC_PORT I2S_NUM_0
#define I2S_MIC_SCK  4
#define I2S_MIC_WS   5
#define I2S_MIC_SD   6

// --- Định nghĩa chân I2S Loa (MAX98357) ---
#define I2S_SPK_BCLK 15
#define I2S_SPK_LRC  16
#define I2S_SPK_DIN  17

// --- Cấu hình I2S Global ---
i2s_config_t i2s_mic_config = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
  .sample_rate = 16000,
  .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
  .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
  .communication_format = I2S_COMM_FORMAT_STAND_I2S,
  .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
  .dma_buf_count = 8,
  .dma_buf_len = 512,
  .use_apll = false,
  .tx_desc_auto_clear = false,
  .fixed_mclk = 0
};

i2s_pin_config_t i2s_mic_pins = {
  .bck_io_num = I2S_MIC_SCK,
  .ws_io_num = I2S_MIC_WS,
  .data_out_num = I2S_PIN_NO_CHANGE,
  .data_in_num = I2S_MIC_SD
};

// --- Định nghĩa chân cắm màn hình ---
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   -1 
#define TFT_DC   9
#define TFT_RST  8
#define TOUCH_PIN 14

// --- Cấu hình Biến trở Âm lượng ---
#define USE_POTENTIOMETER true // Bật/tắt tính năng chỉnh âm lượng vật lý
#define POT_PIN 10              // Chân cắm biến trở (ADC)

SPIClass fspi(FSPI);
Adafruit_ST7789 tft = Adafruit_ST7789(&fspi, TFT_CS, TFT_DC, TFT_RST);

// Bảng màu RGB565 (Theo phong cách Gemini Live)
#define COLOR_BG       0x0000  // Đen tuyền
#define COLOR_EYE      0x07FF  // Xanh Cyan
#define COLOR_USER_BUB 0x03E0  // Xanh lá đậm (ít dùng)
#define COLOR_AI_BUB   0x18E3  // Xám đen (Dark Gray)
#define COLOR_WHITE    0xFFFF
#define COLOR_YELLOW   0xFFE0
#define GEMINI_BLUE    0x037F  // Xanh dương đậm
#define GEMINI_PINK    0xF81F  // Hồng tía
#define GEMINI_CYAN    0x07FF  // Xanh lơ

enum SystemState {
  STATE_IDLE,
  STATE_LISTENING
};
SystemState currentState = STATE_IDLE;

AudioGeneratorMP3 *mp3;
AudioOutputI2S *out;

int eyeHeight = 60;
bool isBlinking = false;
unsigned long lastBlinkTime = 0;

void printText(const char* text, int x, int y, uint16_t color, const GFXfont* font) {
  tft.setFont(font);
  tft.setTextColor(color);
  tft.setCursor(x, y);
  tft.setTextWrap(false);
  tft.print(text);
}

int getMultilineTextHeight(const char* text, const GFXfont* font, int maxWidth) {
  tft.setFont(font);
  String str(text);
  String word = "";
  String line = "";
  int lines = 1;
  for (int i = 0; i <= str.length(); i++) {
    char c = (i < str.length()) ? str[i] : ' ';
    if (c == ' ' || c == '\n' || i == str.length()) {
      int16_t x1, y1; uint16_t w, h;
      tft.getTextBounds(line + word, 0, 0, &x1, &y1, &w, &h);
      if (w > maxWidth && line.length() > 0) {
        lines++;
        line = word + " ";
      } else {
        line += word + " ";
      }
      word = "";
      if (c == '\n') {
        lines++;
        line = "";
      }
    } else {
      word += c;
    }
  }
  return lines * 20;
}

void printMultilineText(const char* text, int x, int y, uint16_t color, const GFXfont* font, int maxWidth, int maxY = 240) {
  tft.setFont(font);
  tft.setTextColor(color);
  
  String str(text);
  String word = "";
  String line = "";
  int currentY = y;
  
  for (int i = 0; i <= str.length(); i++) {
    char c = (i < str.length()) ? str[i] : ' ';
    if (c == ' ' || c == '\n' || i == str.length()) {
      int16_t x1, y1; uint16_t w, h;
      tft.getTextBounds(line + word, x, currentY, &x1, &y1, &w, &h);
      if (w > maxWidth && line.length() > 0) {
        if (currentY > maxY) return; // Không tràn bóng
        tft.setCursor(x, currentY);
        tft.print(line);
        currentY += 20; // Xuống dòng
        line = word + " ";
      } else {
        line += word + " ";
      }
      word = "";
      if (c == '\n') {
        if (currentY > maxY) return;
        tft.setCursor(x, currentY);
        tft.print(line);
        currentY += 20;
        line = "";
      }
    } else {
      word += c;
    }
  }
  if (currentY <= maxY) {
    tft.setCursor(x, currentY);
    tft.print(line);
  }
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
    
    // Xóa nền khu vực đồng hồ cũ (Top center) phòng trường hợp vẫn còn lưu trên màn hình
    tft.fillRect(70, 0, 100, 30, COLOR_BG);
    
    // Nền panel (Xám đậm)
    tft.fillRoundRect(10, 145, 220, 85, 12, 0x2104);
    
    // --- DÒNG 1: ĐỒNG HỒ ---
    if (gotTime) {
      char timeStr[10];
      strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
      tft.setFont(&FreeSans12pt7b);
      tft.setTextColor(0xFFFF);
      tft.setCursor(85, 175); // Canh giữa hàng 1
      tft.print(timeStr);
    }
    
    // --- DÒNG 2: NHIỆT ĐỘ & ĐỘ ẨM ---
    tft.setFont(&FreeSans9pt7b);
    
    // Nhiệt độ bên trái
    tft.fillCircle(25, 210, 7, 0xF800); // Bầu nhiệt kế
    tft.fillRoundRect(22, 193, 7, 17, 3, 0xF800); // Ống nhiệt kế
    tft.setTextColor(0xFFE0); // Vàng
    tft.setCursor(40, 215);
    tft.printf("%.1f C", currentTemp);

    // Độ ẩm bên phải
    tft.fillCircle(135, 210, 7, 0x051D); // Giọt nước
    tft.fillTriangle(128, 210, 142, 210, 135, 196, 0x051D);
    tft.setTextColor(0x07E0); // Xanh lá
    tft.setCursor(150, 215);
    tft.printf("%.1f %%", currentHum);
  }
}

void drawChatUI(const char* userText, const char* aiText) {
  tft.fillScreen(COLOR_BG);
  int currentY = 10;
  
  if (strlen(userText) > 0) {
    int textH = getMultilineTextHeight(userText, &FreeSans9pt7b, 180);
    int bubH = textH + 30;
    if (bubH > 100) bubH = 100; // Giới hạn
    tft.fillRoundRect(30, currentY, 200, bubH, 10, COLOR_USER_BUB);
    tft.fillTriangle(220, currentY + 15, 220, currentY + 30, 235, currentY + 22, COLOR_USER_BUB);
    printText("Tui: ", 40, currentY + 18, COLOR_WHITE, &FreeSans9pt7b);
    printMultilineText(userText, 40, currentY + 38, COLOR_WHITE, &FreeSans9pt7b, 180, currentY + bubH - 5);
    currentY += bubH + 10;
  }
  
  if (strlen(aiText) > 0) {
    int textH = getMultilineTextHeight(aiText, &FreeSans9pt7b, 200);
    int bubH = textH + 30; 
    if (currentY + bubH > 235) bubH = 235 - currentY; // Giới hạn không văng khỏi màn
    
    tft.fillRoundRect(10, currentY, 220, bubH, 10, COLOR_AI_BUB);
    tft.fillTriangle(20, currentY + 15, 20, currentY + 30, 5, currentY + 22, COLOR_AI_BUB);
    printText("AI: ", 20, currentY + 18, COLOR_EYE, &FreeSans9pt7b);
    printMultilineText(aiText, 20, currentY + 38, COLOR_WHITE, &FreeSans9pt7b, 200, currentY + bubH - 5);
  } else if (strlen(userText) == 0) {
    tft.fillRoundRect(20, 10, 200, 35, 10, COLOR_EYE);
    printText("AI Assistant", 42, 34, COLOR_BG, &FreeSans12pt7b);
  }
}

// --- Hiệu ứng Music Visualizer (Fake FFT EQ Bars) ---
void drawEQBars(int energy) {
    static const uint16_t EQ_COLORS[24] = {
      0xF800, 0xF800, 0xF804, 0xF808, // Red -> 
      0xF80C, 0xF810, 0xF814, 0xF818, // -> Pink
      0xF81C, 0xF81F, 0xD81F, 0xB81F, // -> Magenta
      0x981F, 0x781F, 0x581F, 0x381F, // -> Purple
      0x181F, 0x001F, 0x01FF, 0x03FF, // -> Blue
      0x05FF, 0x07FF, 0x07FF, 0x07FF  // -> Cyan
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
        
        // Hiệu ứng vật lý
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

void drawListeningUI(const char* lastAiText, const char* topStatus) {
  tft.fillScreen(COLOR_BG);
  
  if (strlen(lastAiText) > 0) {
    int textH = getMultilineTextHeight(lastAiText, &FreeSans9pt7b, 200);
    int bubH = textH + 30; 
    if (bubH > 90) bubH = 90; // Giới hạn chiều cao bong bóng để nhường chỗ cho cực quang
    
    tft.fillRoundRect(10, 40, 220, bubH, 20, COLOR_AI_BUB);
    printText("Qwen:", 20, 58, COLOR_WHITE, &FreeSans9pt7b);
    printMultilineText(lastAiText, 20, 78, COLOR_WHITE, &FreeSans9pt7b, 200, 40 + bubH - 5);
  } else {
    // Giao diện tĩnh lặng giống Gemini Live
    printText(topStatus, 80, 30, COLOR_WHITE, &FreeSans9pt7b);
  }
}

// Hàm giải mã Base64 (Nhận Text từ Server)
String decodeBase64(String input) {
    size_t outputLength;
    unsigned char *decodedData = (unsigned char *)malloc(input.length());
    if (decodedData == NULL) return "";
    mbedtls_base64_decode(decodedData, input.length(), &outputLength, (const unsigned char *)input.c_str(), input.length());
    String result = String((char*)decodedData, outputLength);
    free(decodedData);
    return result;
}

void createWavHeader(byte* header, int waveDataSize){
  header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
  unsigned int fileSize = waveDataSize + 36;
  header[4] = (byte)(fileSize & 0xFF);
  header[5] = (byte)((fileSize >> 8) & 0xFF);
  header[6] = (byte)((fileSize >> 16) & 0xFF);
  header[7] = (byte)((fileSize >> 24) & 0xFF);
  header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
  header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
  header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
  header[20] = 1; header[21] = 0;
  header[22] = 1; header[23] = 0;
  header[24] = 0x80; header[25] = 0x3E; header[26] = 0x00; header[27] = 0x00; // 16000
  header[28] = 0x00; header[29] = 0x7D; header[30] = 0x00; header[31] = 0x00; // 32000
  header[32] = 2; header[33] = 0;
  header[34] = 16; header[35] = 0;
  header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
  header[40] = (byte)(waveDataSize & 0xFF);
  header[41] = (byte)((waveDataSize >> 8) & 0xFF);
  header[42] = (byte)((waveDataSize >> 16) & 0xFF);
  header[43] = (byte)((waveDataSize >> 24) & 0xFF);
}

void setup() {
  Serial.begin(115200); dht.begin();
  pinMode(TOUCH_PIN, INPUT);
  if (USE_POTENTIOMETER) pinMode(POT_PIN, INPUT);

  fspi.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS); 
  tft.init(240, 240, SPI_MODE3);
  tft.setRotation(2);
  tft.invertDisplay(true);
  tft.fillScreen(COLOR_BG);

  printText("AI Assistant", 42, 100, COLOR_EYE, &FreeSans12pt7b);
  printText("Dang ket noi WiFi...", 50, 130, COLOR_YELLOW, &FreeSans9pt7b);

  WiFiManager wm;
  // wm.resetSettings(); // Bỏ comment dòng này 1 lần nếu muốn xoá WiFi cũ để test
  
  // Tạo mảng char cho tên WiFi phát ra
  const char* apName = "Cai_Dat_AI_Bot";
  
  bool res = wm.autoConnect(apName); // Sẽ chặn (block) cho tới khi có mạng
  
  if (!res) {
    Serial.println("Failed to connect");
    // ESP.restart();
  }
  Serial.println("\nWiFi Connected!");
  
  tft.fillScreen(COLOR_BG);
  printText("AI Assistant", 42, 100, COLOR_EYE, &FreeSans12pt7b);
  printText("Da ket noi mang!", 60, 130, COLOR_WHITE, &FreeSans9pt7b);
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  
  // 1. Cài đặt I2S Loa (Dùng I2S_NUM_1)
  out = new AudioOutputI2S(1);
  out->SetPinout(I2S_SPK_BCLK, I2S_SPK_LRC, I2S_SPK_DIN);
  
  // ---> CHỈNH ÂM LƯỢNG LOA Ở ĐÂY (0.0 ĐẾN 4.0 LÀ LỚN NHẤT) <---
  out->SetGain(0.5); 
  
  mp3 = new AudioGeneratorMP3();

  // 2. Khởi tạo I2S Micro (Dùng I2S_NUM_0)
  i2s_driver_install(I2S_MIC_PORT, &i2s_mic_config, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &i2s_mic_pins);
  i2s_start(I2S_MIC_PORT);

  delay(1000);
  Serial.println("Hệ thống SS! Chạm cảm biến để nói.");
  tft.fillScreen(COLOR_BG); // Xóa màn hình trước khi vào chế độ ngủ (mắt)
}

void loop() {
  bool isTouched = digitalRead(TOUCH_PIN);

  if (isTouched && currentState == STATE_IDLE) {
    currentState = STATE_LISTENING;
    delay(200); 
  }
  
  switch (currentState) {
    case STATE_IDLE:
      drawEyes();
      delay(20);
      break;
      
    case STATE_LISTENING: {
      drawListeningUI(lastAiTextGlobal.c_str(), "+ Live"); 
      
      // Reset lại I2S Micro để tránh lỗi xung đột Clock với Loa
      i2s_driver_uninstall(I2S_MIC_PORT);
      i2s_driver_install(I2S_MIC_PORT, &i2s_mic_config, 0, NULL);
      i2s_set_pin(I2S_MIC_PORT, &i2s_mic_pins);
      i2s_start(I2S_MIC_PORT);
      delay(100); // Ổn định phần cứng
      
      uint32_t MAX_WAV_SIZE = 16000 * 2 * 30; // Tăng lên 30 giây (PSRAM 8MB dư sức chứa)
      uint8_t* wav_buffer = (uint8_t*)ps_malloc(MAX_WAV_SIZE + 44);
      if (!wav_buffer) {
        Serial.println("Khong co PSRAM, chuyen sang RAM thuong...");
        wav_buffer = (uint8_t*)malloc(MAX_WAV_SIZE + 44);
      }
      
      if(!wav_buffer) {
        Serial.println("Loi RAM!");
        tft.fillScreen(COLOR_BG);
        drawChatUI("Loi", "Khong du RAM!");
        delay(2000);
        tft.fillScreen(COLOR_BG);
        currentState = STATE_IDLE; forceUIUpdate = true;
        eyeHeight = 60;
        
        while(digitalRead(TOUCH_PIN) == HIGH) { delay(10); } // Đợi người dùng thả tay ra
        delay(300); // Chống dội
        break;
      }
      
      uint32_t wav_index = 44;
      int silence_count = 0;
      int noise_frames = 0;
      bool started_talking = false;
      int max_vol_bar = 0;
      unsigned long listenStartTime = millis();
      bool timeout_occurred = false;
      
      // Xóa rác mic và dư âm của loa (Clear toàn bộ DMA buffer)
      size_t bytes_read;
      for (int i = 0; i < 8; i++) {
        i2s_read(I2S_MIC_PORT, wav_buffer, 1024, &bytes_read, pdMS_TO_TICKS(100));
      }
      
      while (wav_index < MAX_WAV_SIZE + 44) {

        // Kiểm tra quá 15 giây không nói gì thì hủy (về ngủ)
        if (!started_talking && (millis() - listenStartTime > 15000)) {
          timeout_occurred = true;
          break;
        }

        // Ngăn chặn tràn bộ đệm (Buffer Overflow) gây sụp nguồn
        size_t bytes_to_read = 1024;
        if (wav_index + bytes_to_read > MAX_WAV_SIZE + 44) {
          bytes_to_read = (MAX_WAV_SIZE + 44) - wav_index;
        }

        i2s_read(I2S_MIC_PORT, wav_buffer + wav_index, bytes_to_read, &bytes_read, pdMS_TO_TICKS(100));
        if (bytes_read == 0) break;
        
        long sum = 0;
        int16_t* samples = (int16_t*)(wav_buffer + wav_index);
        for(int i = 0; i < bytes_read/2; i++) {
          sum += samples[i];
        }
        int16_t mean = sum / (bytes_read/2);

        long energy = 0;
        for(int i = 0; i < bytes_read/2; i++) {
          samples[i] = (samples[i] - mean) * 8; 
          energy += abs(samples[i]);
        }
        energy /= (bytes_read/2);
        
        // --- Hiệu ứng Music Visualizer (Fake FFT EQ Bars) ---
        drawEQBars(energy);
        


        if (energy > 2500) { // Giảm ngưỡng xuống 2500 để dễ nhận diện tiếng người hơn
          noise_frames++;
          if (noise_frames > 2) { // Chỉ cần âm thanh liên tục ~200ms là kích hoạt
            silence_count = 0;
            started_talking = true;
          }
        } else {
          noise_frames = 0;
          if (started_talking) {
            silence_count++; 
          }
        }
        
        wav_index += bytes_read;
        if (started_talking && silence_count > 100) { // Đợi ~3-4 giây im lặng thì tự ngắt
          break; 
        }
      }
      
      if (timeout_occurred || !started_talking || wav_index < 16000) {
        // Nếu quá thời gian, hoặc người dùng chưa nói gì cả thì HỦY
        free(wav_buffer);
        tft.fillScreen(COLOR_BG);
        currentState = STATE_IDLE; forceUIUpdate = true;
        while(digitalRead(TOUCH_PIN) == HIGH) { delay(10); } // Wait for release
        delay(300);
        break;
      }

      createWavHeader(wav_buffer, wav_index - 44);
      
      // 3. Gửi lên Server
      tft.fillScreen(COLOR_BG);
      tft.fillRect(0, 0, 240, 40, COLOR_BG);
      printText("Thinking...", 75, 30, COLOR_WHITE, &FreeSans9pt7b);
      for(int i=0; i<15; i++) { drawEQBars(0); delay(20); }
      
      WiFiClientSecure client;
      client.setInsecure(); client.setTimeout(60);
      
      HTTPClient http;
      http.begin(client, SERVER_URL);
      http.setTimeout(60000); // Tăng thời gian chờ lên 60 giây (Mặc định 5s là quá ngắn)
      http.addHeader("Content-Type", "application/octet-stream");
      http.addHeader("X-Temperature", String(currentTemp));
      http.addHeader("X-Humidity", String(currentHum));
      
      const char * headerKeys[] = {"X-User-Text", "X-Screen-Text"};
      http.collectHeaders(headerKeys, 2);
      
      int httpCode = http.POST(wav_buffer, wav_index);
      free(wav_buffer); // Giải phóng RAM ngay lập tức
      
      if (httpCode == HTTP_CODE_OK || httpCode == 200) {
          String payload = http.getString();
          http.end(); // Kết thúc kết nối POST
          
          // Parse JSON
          DynamicJsonDocument doc(1024);
          DeserializationError error = deserializeJson(doc, payload);
          if (error) {
              Serial.print(F("deserializeJson() failed: "));
              Serial.println(error.f_str());
          }
          
          String userText = doc["user_text"].as<String>();
          String sessionId = doc["session_id"].as<String>();
          
          if (userText.length() == 0) userText = "Khong nghe ro";
          
          // Vẽ giao diện
          tft.fillScreen(COLOR_BG);
          drawListeningUI("Dang suy nghi...", "+ Live");
          
          String uLow = String(userText); uLow.toLowerCase();
          bool cancelled_speech = false;
          
          // Nếu người dùng chủ động chào tạm biệt
          if (uLow.indexOf("tam biet") >= 0 || uLow.indexOf("tạm biệt") >= 0 || uLow.indexOf("cam on") >= 0 || uLow.indexOf("cảm ơn") >= 0) {
              tft.fillScreen(COLOR_BG);
              currentState = STATE_IDLE; forceUIUpdate = true;
              break;
          }
          
          // Stream thẳng Audio từ Server (Vừa nghĩ vừa nói)
          String streamUrl = SERVER_URL.substring(0, SERVER_URL.lastIndexOf("/")) + "/stream/" + sessionId;
          Serial.println("Streaming URL: " + streamUrl);
          
          AudioFileSourceHTTPSStream *file = new AudioFileSourceHTTPSStream(streamUrl.c_str());
          AudioFileSourceBuffer *buff = new AudioFileSourceBuffer(file, 4096);
          if (mp3->begin(buff, out)) {
              long last_eq = 0;
              long last_vol_check = 0;
              while (mp3->isRunning()) {
                  if (digitalRead(TOUCH_PIN) == HIGH) {
                      mp3->stop();
                      while(digitalRead(TOUCH_PIN) == HIGH) { delay(10); } // Đợi thả tay
                      delay(300);
                      cancelled_speech = true;
                      break;
                  }
                  
                  if (!mp3->loop()) {
                      mp3->stop();
                      break; 
                  }
                  
                  if (USE_POTENTIOMETER && millis() - last_vol_check > 100) {
                      last_vol_check = millis();
                      int potValue = analogRead(POT_PIN);
                      float gain = (float)potValue / 4095.0 * 1.5;
                      out->SetGain(gain);
                  }
                  
                  if (millis() - last_eq > 50) {
                      last_eq = millis();
                      drawEQBars(random(1000, 4500));
                  }
                  delay(1);
              }
              out->stop();
          }
          delete buff;
          delete file;
          
          if (cancelled_speech) {
              tft.fillScreen(COLOR_BG);
              currentState = STATE_IDLE; forceUIUpdate = true;
          } else {
              delay(200);
              currentState = STATE_LISTENING; // Lắng nghe tiếp
          }
          
      } else if (httpCode == 204) {
          // Im lặng
          Serial.println("Silence detected, server returned 204.");
          currentState = STATE_IDLE; forceUIUpdate = true;
          eyeHeight = 60;
          http.end();
      } else {
          tft.fillScreen(COLOR_BG);
          drawChatUI("Loi ket noi Server", String(httpCode).c_str());
          delay(3000);
          currentState = STATE_IDLE; forceUIUpdate = true;
          eyeHeight = 60;
          http.end();
      }
      break;
    }
  }
}
