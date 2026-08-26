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
volatile int encoderCounter = 1000; // Стартовая частота 1 кГц
int lastFrequency = 0;              // 0 гарантирует обновление экрана при старте
bool buttonPressed = false;
volatile bool change = false;

// Начальные значения скважности для каналов (128 из 255 соответствует 50% скважности)
volatile int dutyCH1 = 128;
volatile int dutyCH2 = 128;

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
      //encoderCounter += 100; // Шаг вверх (100 Гц)

      ++dutyCH1;
      ++dutyCH2;
    } else {
      //encoderCounter -= 100; // Шаг вниз (100 Гц)

      --dutyCH1;
      --dutyCH2;
    }
    
    // Ограничительные рамки по частоте
    if (encoderCounter < 100) encoderCounter = 100;
    if (encoderCounter > 5000000) encoderCounter = 5000000;

    change = true;  // Change in state detected
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
  // Опрос физического состояния кнопки
  buttonPressed = (digitalRead(ENC_BTN_PIN) == LOW);
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

void lcd_tick() {
 // 1. Отрисовка Частоты (Строка 2, начиная с 10-го столбца)
  lcd.setCursor(10, 2);
  lcd.print(encoderCounter);
  lcd.print(" Hz      "); // Пробелы затирают старые хвосты длинных чисел

  // 2. Отрисовка Скважности (Строка 3, начиная с 10-го столбца)
  int dutyPercent1 = map(dutyCH1, 0, 255, 0, 100);
  lcd.setCursor(10, 3);
  
  // Форматирование пробелами для красивого выравнивания
  if (dutyPercent1 < 10) {
    lcd.print("  ");
  } else if (dutyPercent1 < 100) {
    lcd.print(" ");
  }
  
  lcd.print(dutyPercent1);
  lcd.print("%   ");
}

void lcd_draw_interface() {
 lcd.clear();
  
  // Строка 0: Заголовок
  lcd.setCursor(0, 0);
  lcd.print("DDS Generator v1.5");
  
  // Строка 1: Разделительная полоса
  lcd.setCursor(0, 1);
  lcd.print("--------------------"); 
  
  // Строка 2: Статическая метка частоты
  lcd.setCursor(0, 2);
  lcd.print("CH1 Freq: ");
  
  // Строка 3: Статическая метка скважности
  lcd.setCursor(0, 3);          
  lcd.print("CH1 Duty: ");
  
  // Принудительно отрисовываем начальные значения на экране
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
  genCH1.setFrequency(MD_AD9833::CHAN_0, encoderCounter);
  genCH1.setMode(MD_AD9833::MODE_TRIANGLE); // Установка режима ПРЯМОУГОЛЬНИК (меандр)
  
  // Инициализация Канала 2 (GPIO 10)
  genCH2.begin(); 
  genCH2.setFrequency(MD_AD9833::CHAN_0, encoderCounter);
  genCH2.setMode(MD_AD9833::MODE_TRIANGLE); // Установка режима ПРЯМОУГОЛЬНИК (меандр)
}

void dds_tick() {
  static int ddsLastFreq = 0;
  int currentFreq;

  noInterrupts();
  currentFreq = encoderCounter;
  interrupts();
 
  // Библиотека сама опустит и поднимет нужный FSYNC во время отправки данных по SPI
  genCH1.setFrequency(MD_AD9833::CHAN_0, currentFreq);
  genCH2.setFrequency(MD_AD9833::CHAN_0, currentFreq);

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
  // Основной рабочий цикл (без мусора и тестов)
  encoder_tick();     // Опрос кнопки и светодиода

  if(change) {
    lcd_tick();
    dds_tick();
    change = false;
  }

  delay(10); // Защита от перегрузки процессора
}