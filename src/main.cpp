#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <driver/i2s.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <AudioFileSourceHTTPSStream.h>

// --- THAY BẰNG LINK CLOUDFLARE CỦA BẠN ---
const char* SERVER_URL = "https://groq-ai.thaihuy1112006.workers.dev";

// --- Chân I2S Micro (INMP441) ---
#define I2S_MIC_PORT I2S_NUM_0
#define I2S_MIC_SCK  4
#define I2S_MIC_WS   5
#define I2S_MIC_SD   6

// --- Chân I2S Loa (MAX98357) ---
#define I2S_SPK_PORT I2S_NUM_1
#define I2S_SPK_BCLK 15
#define I2S_SPK_LRC  16
#define I2S_SPK_DIN  17

// --- Chân cảm biến & Nút bấm ---
#define DHTPIN 7
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);
#define TOUCH_PIN 14
#define POT_PIN 10 // Biến trở 20k
#define BUTTON_PIN 19 // Nút bấm cứng (Ấn nhả để huỷ lệnh)

// --- Chân cắm màn hình ---
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   -1 
#define TFT_DC   9
#define TFT_RST  8

SPIClass fspi(FSPI);
Adafruit_ST7789 tft = Adafruit_ST7789(&fspi, TFT_CS, TFT_DC, TFT_RST);

#define COLOR_BG       0x0000 
#define COLOR_EYE      0x07FF 
#define COLOR_WHITE    0xFFFF
#define COLOR_YELLOW   0xFFE0
#define COLOR_BUBBLE   0x18E3 // Xám mờ cho bóng chat

enum SystemState {
  STATE_IDLE,
  STATE_LISTENING,
  STATE_THINKING,
  STATE_SPEAKING
};
SystemState currentState = STATE_IDLE;
bool forceUIUpdate = true;

// VAD (Voice Activity Detection - Lọc ồn & Dò thời gian im lặng)
const long SILENCE_TIMEOUT = 1500; // Dừng nói 1.5 giây là gửi
const int NOISE_THRESHOLD = 500;   // Ngưỡng lọc ồn (Tăng lên nếu mic nhạy quá)
unsigned long lastSpeechTime = 0;
bool isSpeaking = false;

// Bộ nhớ Audio PSRAM
const int MAX_RECORD_TIME_SEC = 8;
const int SAMPLE_RATE = 16000;
const int MAX_AUDIO_BYTES = SAMPLE_RATE * 2 * MAX_RECORD_TIME_SEC + 44; 
uint8_t* audioBuffer;
int audioLength = 0;

AudioGeneratorMP3 *mp3;
AudioOutputI2S *out;
AudioFileSourceHTTPSStream *fileStream;

// ================= CÁC HÀM GIAO DIỆN =================

void printText(const char* text, int x, int y, uint16_t color, const GFXfont* font) {
  tft.setFont(font); tft.setTextColor(color);
  tft.setCursor(x, y); tft.print(text);
}

void printMultilineText(const char* text, int x, int y, uint16_t color, const GFXfont* font, int maxWidth, int maxY = 240) {
  tft.setFont(font); tft.setTextColor(color);
  String str(text); String word = ""; String line = ""; int currentY = y;
  for (int i = 0; i <= (int)str.length(); i++) {
    char c = (i < (int)str.length()) ? str[i] : ' ';
    if (c == ' ' || c == '\n' || i == str.length()) {
      int16_t x1, y1; uint16_t w, h;
      tft.getTextBounds(line + word, x, currentY, &x1, &y1, &w, &h);
      if (w > maxWidth && line.length() > 0) {
        if (currentY > maxY) return; 
        tft.setCursor(x, currentY); tft.print(line);
        currentY += 20; line = word + " ";
      } else { line += word + " "; }
      word = "";
      if (c == '\n') {
        if (currentY > maxY) return;
        tft.setCursor(x, currentY); tft.print(line);
        currentY += 20; line = "";
      }
    } else { word += c; }
  }
  if (currentY <= maxY) { tft.setCursor(x, currentY); tft.print(line); }
}

void drawGeminiLiveUI(const char* aiText) {
  tft.fillScreen(COLOR_BG);
  // Bóng chat mờ nằm giữa
  tft.fillRoundRect(10, 10, 220, 150, 15, COLOR_BUBBLE);
  printText("Groq:", 20, 35, COLOR_EYE, &FreeSans12pt7b);
  
  if (strlen(aiText) > 0) {
    printMultilineText(aiText, 20, 60, COLOR_WHITE, &FreeSans9pt7b, 200, 150);
  } else {
    printText("Dang suy nghi...", 20, 80, COLOR_YELLOW, &FreeSans9pt7b);
  }
}

void drawEyes() {
  static unsigned long lastActionTime = 0;
  static int currentX = 50;
  static int currentH = 60;
  static bool needsRedraw = true;
  static bool isBlinking = false;
  static unsigned long blinkTime = 0;

  if (forceUIUpdate) {
    needsRedraw = true; tft.fillScreen(COLOR_BG); forceUIUpdate = false;
  }

  if (isBlinking && millis() - blinkTime > 150) {
    isBlinking = false; currentH = 60; needsRedraw = true;
  }
  if (!isBlinking && millis() - lastActionTime > random(1500, 3000)) {
    lastActionTime = millis();
    int action = random(100);
    if (action < 40) { isBlinking = true; blinkTime = millis(); currentH = 10; } 
    else if (action < 60) { currentX = 30; } 
    else if (action < 80) { currentX = 70; } 
    else { currentX = 50; }
    needsRedraw = true;
  }

  if (needsRedraw) {
    tft.fillRect(0, 0, 240, 140, COLOR_BG); 
    tft.fillRoundRect(currentX, 70 - (currentH/2), 50, currentH, 15, COLOR_EYE);
    tft.fillRoundRect(currentX + 90, 70 - (currentH/2), 50, currentH, 15, COLOR_EYE);
    needsRedraw = false;
    printText("Cham de noi", 65, 160, COLOR_WHITE, &FreeSans9pt7b);
  }
}

void drawEQBars(int energy) {
    static const uint16_t EQ_COLORS[24] = {
      0xF800, 0xF800, 0xF804, 0xF808, 0xF80C, 0xF810, 0xF814, 0xF818, 
      0xF81C, 0xF81F, 0xD81F, 0xB81F, 0x981F, 0x781F, 0x581F, 0x381F, 
      0x181F, 0x001F, 0x01FF, 0x03FF, 0x05FF, 0x07FF, 0x07FF, 0x07FF  
    };
    static float phase = 0; phase += 0.4;
    static int old_bar_height[24] = {0};
    int max_amp = map(energy, 0, 5000, 5, 60); // Max 60px cao
    if (max_amp > 60) max_amp = 60; if (max_amp < 5) max_amp = 5;

    for (int i = 0; i < 24; i++) {
        float wave = abs(sin(i * 0.5 + phase) * cos(i * 0.3 - phase * 0.7));
        float noise = (float)random(60, 100) / 100.0;
        float center_boost = 1.0 - abs(i - 11.5) / 12.0; 
        
        int target_h = 5 + max_amp * wave * noise * (0.5 + 0.8 * center_boost);
        if (target_h > 60) target_h = 60;
        
        int h = old_bar_height[i];
        if (target_h > h) h = target_h; else { h -= 4; if (h < 5) h = 5; }
        
        int x = i * 10 + 1; int old_h = old_bar_height[i];
        if (h < old_h) tft.fillRect(x, 240 - old_h, 8, old_h - h, COLOR_BG);
        else if (h > old_h) tft.fillRect(x, 240 - h, 8, h - old_h, EQ_COLORS[i]);
        if (old_h == 0) tft.fillRect(x, 240 - 5, 8, 5, EQ_COLORS[i]); 
        
        old_bar_height[i] = h;
    }
}

// ================= CÁC HÀM XỬ LÝ ÂM THANH =================

void createWavHeader(byte* header, int waveDataSize){
  header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
  unsigned int fileSize = waveDataSize + 36;
  header[4] = (byte)(fileSize & 0xFF); header[5] = (byte)((fileSize >> 8) & 0xFF); header[6] = (byte)((fileSize >> 16) & 0xFF); header[7] = (byte)((fileSize >> 24) & 0xFF);
  header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
  header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
  header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
  header[20] = 1; header[21] = 0; header[22] = 1; header[23] = 0;
  header[24] = 0x80; header[25] = 0x3E; header[26] = 0x00; header[27] = 0x00; 
  header[28] = 0x00; header[29] = 0x7D; header[30] = 0x00; header[31] = 0x00; 
  header[32] = 2; header[33] = 0; header[34] = 16; header[35] = 0;
  header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
  header[40] = (byte)(waveDataSize & 0xFF); header[41] = (byte)((waveDataSize >> 8) & 0xFF); header[42] = (byte)((waveDataSize >> 16) & 0xFF); header[43] = (byte)((waveDataSize >> 24) & 0xFF);
}

void abortAndReset() {
  if (mp3 && mp3->isRunning()) mp3->stop();
  currentState = STATE_IDLE;
  forceUIUpdate = true;
}

void setup() {
  Serial.begin(115200);
  pinMode(TOUCH_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(POT_PIN, INPUT);

  audioBuffer = (uint8_t*) ps_malloc(MAX_AUDIO_BYTES);

  fspi.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS); 
  tft.init(240, 240, SPI_MODE3);
  tft.setRotation(2); tft.invertDisplay(true); tft.fillScreen(COLOR_BG);

  printText("AI Assistant", 42, 100, COLOR_EYE, &FreeSans12pt7b);
  printText("Dang ket noi WiFi...", 40, 130, COLOR_YELLOW, &FreeSans9pt7b);

  WiFiManager wm;
  if (!wm.autoConnect("Groq_AI_Setup")) ESP.restart();
  
  tft.fillScreen(COLOR_BG);
  printText("WiFi Connected!", 50, 130, COLOR_WHITE, &FreeSans9pt7b);
  delay(1000);

  // Cấu hình I2S Micro
  i2s_config_t i2s_mic_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8, .dma_buf_len = 512,
    .use_apll = false, .tx_desc_auto_clear = false, .fixed_mclk = 0
  };
  i2s_pin_config_t i2s_mic_pins = {
    .bck_io_num = I2S_MIC_SCK, .ws_io_num = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = I2S_MIC_SD
  };
  i2s_driver_install(I2S_MIC_PORT, &i2s_mic_config, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &i2s_mic_pins);
  
  // Cấu hình I2S Loa
  out = new AudioOutputI2S(I2S_SPK_PORT, 1);
  out->SetPinout(I2S_SPK_BCLK, I2S_SPK_LRC, I2S_SPK_DIN);
  mp3 = new AudioGeneratorMP3();

  tft.fillScreen(COLOR_BG);
}

void loop() {
  // Đọc Nút bấm huỷ khẩn cấp
  if (digitalRead(BUTTON_PIN) == LOW) {
    if (currentState != STATE_IDLE) abortAndReset();
    delay(200);
  }

  // Đọc Cảm biến chạm
  bool isTouched = digitalRead(TOUCH_PIN);
  static bool lastTouchState = false;

  // Lógica chạm 
  if (isTouched && !lastTouchState) {
    if (currentState == STATE_IDLE) {
      // 1. Chạm để bắt đầu nghe
      currentState = STATE_LISTENING;
      audioLength = 44; 
      isSpeaking = true;
      lastSpeechTime = millis();
      i2s_zero_dma_buffer(I2S_MIC_PORT);
      tft.fillScreen(COLOR_BG);
      printText("Dang nghe...", 60, 50, COLOR_EYE, &FreeSans12pt7b);
    } else if (currentState == STATE_LISTENING) {
      // 2. Chạm lần nữa để chốt câu khẩn cấp
      currentState = STATE_THINKING;
      drawGeminiLiveUI(""); // Vẽ UI Đang suy nghĩ
    }
  }
  lastTouchState = isTouched;
  
  // --- VÒNG LẶP STATE MACHINE ---
  switch (currentState) {
    case STATE_IDLE:
      drawEyes();
      delay(20);
      break;

    case STATE_LISTENING: {
      if (audioLength < MAX_AUDIO_BYTES) {
        size_t bytesRead = 0;
        int16_t sampleBuffer[256]; 
        i2s_read(I2S_MIC_PORT, &sampleBuffer, sizeof(sampleBuffer), &bytesRead, portMAX_DELAY);
        
        long energy = 0;
        for(int i=0; i < bytesRead/2; i++) {
          energy += abs(sampleBuffer[i]);
          if (audioLength < MAX_AUDIO_BYTES - 1) {
             uint8_t* bytePtr = (uint8_t*)&sampleBuffer[i];
             audioBuffer[audioLength++] = bytePtr[0];
             audioBuffer[audioLength++] = bytePtr[1];
          }
        }
        
        long avgEnergy = energy / (bytesRead/2);
        drawEQBars(avgEnergy); // Sóng nhạc nhảy theo lời nói thật

        // Logic VAD (Lọc ồn và ngắt)
        if (avgEnergy > NOISE_THRESHOLD) {
          lastSpeechTime = millis();
          isSpeaking = true;
        }

        if (isSpeaking && (millis() - lastSpeechTime > SILENCE_TIMEOUT)) {
          // Im lặng đủ lâu -> Tự động chốt câu
          currentState = STATE_THINKING;
          drawGeminiLiveUI(""); // Bật UI bóng mờ suy nghĩ
        }
      } else {
        // Đầy bộ nhớ -> Chốt
        currentState = STATE_THINKING;
        drawGeminiLiveUI(""); 
      }
      break;
    }

    case STATE_THINKING: {
      drawEQBars(random(500, 3000)); // Sóng nhạc nhảy múa khi suy nghĩ
      
      createWavHeader(audioBuffer, audioLength - 44);
      HTTPClient http;
      http.begin(SERVER_URL);
      http.addHeader("Content-Type", "application/octet-stream");
      int httpCode = http.POST(audioBuffer, audioLength);
      
      if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument doc;
        deserializeJson(doc, payload);
        const char* aiText = doc["aiText"];
        const char* ttsUrl = doc["ttsUrl"];

        drawGeminiLiveUI(aiText); // In chữ Groq: <câu trả lời> lên bóng chat

        if (ttsUrl) {
          fileStream = new AudioFileSourceHTTPSStream(ttsUrl);
          float vol = analogRead(POT_PIN) / 4095.0 * 2.0;
          out->SetGain(vol);
          mp3->begin(fileStream, out);
          currentState = STATE_SPEAKING;
        } else {
          abortAndReset();
        }
      } else {
        tft.fillScreen(COLOR_RED);
        printText("Loi Server", 60, 120, COLOR_WHITE, &FreeSans9pt7b);
        delay(2000);
        abortAndReset();
      }
      http.end();
      break;
    }
      
    case STATE_SPEAKING:
      drawEQBars(random(1000, 4000)); // Sóng nhạc nhảy múa khi đang nói

      if (mp3->isRunning()) {
        if (!mp3->loop()) mp3->stop();
      } else {
        delete fileStream; fileStream = NULL;
        abortAndReset();
      }
      break;
  }
}