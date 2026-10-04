#include <Arduino.h>
#include <SPI.h>
#include <MD_AD9833.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "driver/ledc.h"

// --- Конфигурация Периферии (Пины) ---
#define I2C_SDA       1  
#define I2C_SCL       3

#define ENC_A_PIN     0
#define ENC_B_PIN     2
#define ENC_BTN_PIN   9  

// Пины выбора чипов (FSYNC) для двух каналов расширения
#define DDS_CH1_FSYNC 7 
#define DDS_CH2_FSYNC 10

#define DDS_SCLK      4
#define DDS_MOSI      6

// Пины аппаратного ШИМ для регулировки скважности
#define PWM_CH1_PIN   5
#define PWM_CH2_PIN   8

// --- Настройки аппаратного ШИМ (LEDC) ---
#define PWM_FREQ      20000 // Частота ШИМ: 100 кГц для легкой фильтрации RC-цепью
#define PWM_RES       8      // Разрешение ШИМ: 8 бит (0 - 255)
#define PWM_CH1_INDEX 0      // Аппаратный канал LEDC для первого генератора (для ядра v2.x)
#define PWM_CH2_INDEX 1      // Аппаратный канал LEDC для второго генератора (для ядра v2.x)

// --- Глобальные Объекты ---
LiquidCrystal_I2C lcd(0x27, 20, 4);
MD_AD9833 genCH1(DDS_CH1_FSYNC);
MD_AD9833 genCH2(DDS_CH2_FSYNC);

// --- Глобальные Состояния и Переменные ---
volatile int encoderDelta = 0;
int frequencyCH1 = 1000;
int frequencyCH2 = 1000;
int phaseCH1 = 0;
int phaseCH2 = 0;
bool channelCH1Enabled = true;
bool channelCH2Enabled = true;
int selectedMenuItem = 0;
bool editingMenuItem = false;
bool menuNeedsRedraw = true;
bool lastButtonState = HIGH;
unsigned long lastButtonChange = 0;

// Начальные значения скважности для каналов (128 из 255 соответствует 50% скважности)
int dutyCH1 = 128;
int dutyCH2 = 128;

// --- Декларация тестовых методов (вынесены вниз) ---
void debug_serial_pins();
void debug_lcd_pins();
void lcd_run_test_counters();

void set_channels_duty(int duty1, int duty2);

// =========================================================================
// 1. МОДУЛЬ ЭНКОДЕРА (Обработчики и Инициализация)
// =========================================================================

void IRAM_ATTR readEncoderISR() {
  bool aState = digitalRead(ENC_A_PIN);
  bool bState = digitalRead(ENC_B_PIN);
  static bool lastA = HIGH;
  
  // Метод определения направления по спаду фазы А
  if (lastA == HIGH && aState == LOW) {
    if (bState == HIGH) {
      ++encoderDelta;
    } else {
      --encoderDelta;
    }
  }
  lastA = aState;
}

void encoder_init() {
  pinMode(ENC_A_PIN, INPUT_PULLUP);
  pinMode(ENC_B_PIN, INPUT_PULLUP);
  pinMode(ENC_BTN_PIN, INPUT_PULLUP);
  
  // Стабильное прерывание строго по спаду уровня фазы А
  attachInterrupt(digitalPinToInterrupt(ENC_A_PIN), readEncoderISR, FALLING);
}

void encoder_tick() {
  bool buttonState = digitalRead(ENC_BTN_PIN);
  unsigned long now = millis();
  if (buttonState != lastButtonState && now - lastButtonChange >= 30) {
    lastButtonChange = now;
    lastButtonState = buttonState;
    if (buttonState == LOW) {
      editingMenuItem = !editingMenuItem;
      menuNeedsRedraw = true;
    }
  }
}

// =========================================================================
// 2. МОДУЛЬ ДИСПЛЕЯ (Интерфейс и Отрисовка)
// =========================================================================

void lcd_init() {
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();                      
  lcd.backlight(); 
  lcd.clear();
}

void lcd_print_three_digits(int value) {
  lcd.print((value / 100) % 10);
  lcd.print((value / 10) % 10);
  lcd.print(value % 10);
}

void lcd_draw_channel(int channel) {
  int row = channel * 2;
  int frequency = channel == 0 ? frequencyCH1 : frequencyCH2;
  int duty = channel == 0 ? dutyCH1 : dutyCH2;
  int phase = channel == 0 ? phaseCH1 : phaseCH2;
  bool enabled = channel == 0 ? channelCH1Enabled : channelCH2Enabled;

  lcd.setCursor(0, row);
  lcd.print(channel == 0 ? "CH1 " : "CH2 ");
  lcd.print(frequency / 1000000);
  lcd.print(' ');
  lcd_print_three_digits((frequency / 1000) % 1000);
  lcd.print(' ');
  lcd_print_three_digits(frequency % 1000);
  lcd.print("Hz");

  lcd.setCursor(0, row + 1);
  lcd.print("D ");
  lcd_print_three_digits(map(duty, 0, 255, 0, 100));
  lcd.print("% P ");
  lcd_print_three_digits(phase);
  lcd.print(' ');
  lcd.print(enabled ? "ON " : "OFF");
}

void lcd_cursor_position(int item, int &column, int &row) {
  static const int frequencyColumns[] = { 4, 6, 7, 8, 10, 11, 12 };
  int channel = item / 14;
  int symbol = item % 14;
  row = channel * 2;

  if (symbol < 7) {
    column = frequencyColumns[symbol];
  } else {
    row += 1;
    if (symbol < 10) {
      column = 2 + symbol - 7;
    } else if (symbol < 13) {
      column = 9 + symbol - 10;
    } else {
      column = 13;
    }
  }
}

void lcd_tick() {
  lcd.clear();
  lcd_draw_channel(0);
  lcd_draw_channel(1);

  int cursorColumn;
  int cursorRow;
  lcd_cursor_position(selectedMenuItem, cursorColumn, cursorRow);
  lcd.setCursor(cursorColumn, cursorRow);
  if (editingMenuItem) {
    lcd.cursor();
    lcd.noBlink();
  } else {
    lcd.noCursor();
    lcd.blink();
  }
}

void lcd_draw_interface() {
  lcd_tick();
}

void startup_message() {
  lcd.setCursor(3, 1);
  lcd.print("DDS Generator");
  delay(1000);      
}

// =========================================================================
// 3. МОДУЛЬ СИНТЕЗАТОРА ЧАСТОТЫ (AD9833 DDS)
// =========================================================================

void dds_init() {
  SPI.begin(DDS_SCLK, -1, DDS_MOSI, -1); 
  delay(10);

  // Инициализация Канала 1 (GPIO 7)
  genCH1.begin(); 
  genCH1.setFrequency(MD_AD9833::CHAN_0, frequencyCH1);
  genCH1.setPhase(MD_AD9833::CHAN_0, phaseCH1 * 10);
  genCH1.setMode(MD_AD9833::MODE_TRIANGLE); // Установка режима ПРЯМОУГОЛЬНИК (меандр)
  
  // Инициализация Канала 2 (GPIO 10)
  genCH2.begin(); 
  genCH2.setFrequency(MD_AD9833::CHAN_0, frequencyCH2);
  genCH2.setPhase(MD_AD9833::CHAN_0, phaseCH2 * 10);
  genCH2.setMode(MD_AD9833::MODE_TRIANGLE); // Установка режима ПРЯМОУГОЛЬНИК (меандр)
}

void dds_tick() {
  genCH1.setFrequency(MD_AD9833::CHAN_0, frequencyCH1);
  genCH1.setPhase(MD_AD9833::CHAN_0, phaseCH1 * 10);
  genCH1.setMode(channelCH1Enabled ? MD_AD9833::MODE_TRIANGLE : MD_AD9833::MODE_OFF);

  genCH2.setFrequency(MD_AD9833::CHAN_0, frequencyCH2);
  genCH2.setPhase(MD_AD9833::CHAN_0, phaseCH2 * 10);
  genCH2.setMode(channelCH2Enabled ? MD_AD9833::MODE_TRIANGLE : MD_AD9833::MODE_OFF);

  set_channels_duty(dutyCH1, dutyCH2);
}

// =========================================================================
// 4. ИЗОЛИРОВАННЫЕ ТЕСТОВЫЕ МЕТОДЫ (Отладка)
// =========================================================================

void debug_lcd_pins() {
  lcd.setCursor(0, 0);
  lcd.print("A:"); lcd.print(digitalRead(ENC_A_PIN));
  lcd.print(" B:"); lcd.print(digitalRead(ENC_B_PIN));
  lcd.print(" BTN:"); lcd.print(digitalRead(ENC_BTN_PIN));
  lcd.print("    "); 
}

void lcd_run_test_counters() {
  static unsigned long lastUpdate = 0;
  static int testFreq = 1000;
  static int testDuty = 50;
  static bool direction = true;

  if (millis() - lastUpdate >= 100) {
    lastUpdate = millis();

    if (direction) {
      testFreq += 100; testDuty += 1;
      if (testFreq >= 5000) direction = false;
    } else {
      testFreq -= 100; testDuty -= 1;
      if (testFreq <= 100) direction = true;
    }

    lcd.setCursor(11, 2);
    lcd.print(testFreq); lcd.print(" Hz   "); 
    lcd.setCursor(11, 3);
    lcd.print(testDuty); lcd.print(" %  ");   
  }
}

// =========================================================================
// 5. МОДУЛЬ АППАРАТНОГО ШИМ (LEDC) — 50% НАПРЯЖЕНИЯ (МЕАНДР / СМЕЩЕНИЕ)
// =========================================================================

void set_channels_duty(int duty1, int duty2) {
  dutyCH1 = constrain(duty1, 0, 255);
  dutyCH2 = constrain(duty2, 0, 255);

#if defined(ESP_IDF_VERSION_MAJOR) && (ESP_IDF_VERSION_MAJOR >= 5)
  // Arduino ESP32 v3.x
  ledcWrite(PWM_CH1_PIN, dutyCH1);
  ledcWrite(PWM_CH2_PIN, dutyCH2);
#else
  // Arduino ESP32 v2.x
  ledcWrite(0, dutyCH1); // Канал 0
  ledcWrite(1, dutyCH2); // Канал 1
#endif
}

void pwm_init() {
#if defined(ESP_IDF_VERSION_MAJOR) && (ESP_IDF_VERSION_MAJOR >= 5)
  // Arduino ESP32 v3.x — привязываем напрямую к пину
  ledcAttach(PWM_CH1_PIN, PWM_FREQ, PWM_RES);
  ledcAttach(PWM_CH2_PIN, PWM_FREQ, PWM_RES);
#else
  // Arduino ESP32 v2.x — привязываем через каналы
  ledcSetup(0, PWM_FREQ, PWM_RES);
  ledcAttachPin(PWM_CH1_PIN, 0);

  ledcSetup(1, PWM_FREQ, PWM_RES);
  ledcAttachPin(PWM_CH2_PIN, 1);
#endif

  delay (10);

  set_channels_duty(dutyCH1, dutyCH2);
}

// =========================================================================
// 6. СИСТЕМНЫЙ МОДУЛЬ И ЯДРО
// =========================================================================

void setup() {
  lcd_init();         // Инициализация экрана
  encoder_init();     // Инициализация крутилки
  dds_init();         // Инициализация генератора (Если зависает — временно закомментировать)
  
  startup_message();  
  lcd_draw_interface(); 
  pwm_init();
}

void loop() {
  encoder_tick();

  int delta;
  noInterrupts();
  delta = encoderDelta;
  encoderDelta = 0;
  interrupts();

  if (delta != 0) {
    if (editingMenuItem) {
      int channel = selectedMenuItem / 14;
      int symbol = selectedMenuItem % 14;

      if (symbol < 7) {
        static const int placeValues[] = { 1000000, 100000, 10000, 1000, 100, 10, 1 };
        int &frequency = channel == 0 ? frequencyCH1 : frequencyCH2;
        frequency = constrain(frequency + delta * placeValues[symbol], 100, 5000000);
      } else if (symbol < 10) {
        int &duty = channel == 0 ? dutyCH1 : dutyCH2;
        int dutyPercent = map(duty, 0, 255, 0, 100);
        static const int placeValues[] = { 100, 10, 1 };
        dutyPercent = constrain(dutyPercent + delta * placeValues[symbol - 7], 0, 100);
        duty = map(dutyPercent, 0, 100, 0, 255);
      } else if (symbol < 13) {
        int &phase = channel == 0 ? phaseCH1 : phaseCH2;
        static const int placeValues[] = { 100, 10, 1 };
        phase = constrain(phase + delta * placeValues[symbol - 10], 0, 360);
      } else {
        bool &enabled = channel == 0 ? channelCH1Enabled : channelCH2Enabled;
        enabled = delta > 0;
      }
      dds_tick();
    } else {
      selectedMenuItem = (selectedMenuItem + delta % 28 + 28) % 28;
    }
    menuNeedsRedraw = true;
  }

  if (menuNeedsRedraw) {
    lcd_tick();
    menuNeedsRedraw = false;
  }

  delay(10);
}