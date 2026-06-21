#include <Arduino.h>
#include <SPI.h>
#include <MD_AD9833.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// --- Конфигурация Периферии (Пины) ---
#define I2C_SDA       1  
#define I2C_SCL       3

#define ENC_A_PIN     0
#define ENC_B_PIN     2
#define ENC_BTN_PIN   9  
#define HARDWARE_LED  8

#define DDS_CH1_FSYNC 7 
#define DDS_SCLK      4
#define DDS_MOSI      6

// --- Глобальные Объекты ---
LiquidCrystal_I2C lcd(0x27, 20, 4);
MD_AD9833 gen(DDS_CH1_FSYNC);

// --- Глобальные Состояния и Переменные ---
volatile int encoderCounter = 1000; // Стартовая частота 1 кГц
int lastFrequency = 0;              // 0 гарантирует обновление экрана при старте
bool buttonPressed = false;

// --- Декларация тестовых методов (вынесены вниз) ---
void debug_serial_pins();
void debug_lcd_pins();
void lcd_run_test_counters();

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
      encoderCounter += 100; // Шаг вверх (100 Гц)
    } else {
      encoderCounter -= 100; // Шаг вниз (100 Гц)
    }
    
    // Ограничительные рамки по частоте
    if (encoderCounter < 100) encoderCounter = 100;
    if (encoderCounter > 5000000) encoderCounter = 5000000;
  }
  lastA = aState;
}

void encoder_init() {
  pinMode(ENC_A_PIN, INPUT_PULLUP);
  pinMode(ENC_B_PIN, INPUT_PULLUP);
  pinMode(ENC_BTN_PIN, INPUT_PULLUP);
  
  // Стабильное прерывание строго по спаду уровня фазы А
  attachInterrupt(digitalPinToInterrupt(ENC_A_PIN), readEncoderISR, FALLING);
  
  Serial.println("[ INIT ] Энкодер на прерывании FALLING настроен.");
}

void encoder_tick() {
  // Опрос физического состояния кнопки
  buttonPressed = (digitalRead(ENC_BTN_PIN) == LOW);
  
  // Управление встроенным светодиодом по нажатию
  digitalWrite(HARDWARE_LED, buttonPressed ? LOW : HIGH);
}

// =========================================================================
// 2. МОДУЛЬ ДИСПЛЕЯ (Интерфейс и Отрисовка)
// =========================================================================

void lcd_init() {
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();                      
  lcd.backlight(); 
  lcd.clear();
  Serial.println("[ INIT ] Дисплей I2C 2004A запущен.");
}

void lcd_draw_interface() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("DDS Generator v1.5");
  lcd.setCursor(0, 1);
  lcd.print("--------------------"); 
  lcd.setCursor(0, 2);
  lcd.print(" CH1 Freq: ");
  lcd.setCursor(0, 3);
  lcd.print(" CH1 Duty: 50 %");
}

void lcd_tick() {
  int currentFreq;
  
  // Атомарное чтение переменной из прерывания
  noInterrupts();
  currentFreq = encoderCounter;
  interrupts();

  // Обновление значения частоты на экране при изменениях
  if (currentFreq != lastFrequency) {
    lastFrequency = currentFreq;
    
    lcd.setCursor(11, 2);
    lcd.print(currentFreq);
    lcd.print(" Hz      "); // Пробелы стирают артефакты старых цифр
  }
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

  gen.begin(); 
  gen.setFrequency(MD_AD9833::CHAN_0, encoderCounter);
  gen.setMode(MD_AD9833::MODE_TRIANGLE); 
  
  Serial.println("[ INIT ] Чип AD9833 инициализирован.");
}

void dds_tick() {
  static int ddsLastFreq = 0;
  int currentFreq;

  noInterrupts();
  currentFreq = encoderCounter;
  interrupts();

  // Обновляем частоту в самом чипе только при реальном сдвиге частоты
  if (currentFreq != ddsLastFreq) {
    ddsLastFreq = currentFreq;
    gen.setFrequency(MD_AD9833::CHAN_0, currentFreq);
    
    Serial.print("[ DDS ] Частота изменена: ");
    Serial.print(currentFreq);
    Serial.println(" Гц");
  }
}

// =========================================================================
// 4. СИСТЕМНЫЙ МОДУЛЬ И ЯДРО
// =========================================================================

void system_init() {
  Serial.begin(115200);
  
  unsigned long startWait = millis();
  while (!Serial && (millis() - startWait < 3000)) {
    delay(10);
  }

  Serial.println("\n=== SYSTEM START ===");
  pinMode(HARDWARE_LED, OUTPUT);
}

void setup() {
  system_init();      // Запуск базовых систем
  lcd_init();         // Инициализация экрана
  encoder_init();     // Инициализация крутилки
  dds_init();         // Инициализация генератора (Если зависает — временно закомментировать)
  
  startup_message();  
  lcd_draw_interface(); 
}

void loop() {
  // Основной рабочий цикл (без мусора и тестов)
  encoder_tick();     // Опрос кнопки и светодиода
  dds_tick();         // Мониторинг изменений и отправка в AD9833
  lcd_tick();         // Мониторинг изменений и вывод на дисплей

  // --- МЕСТО ДЛЯ ТЕСТОВ (Раскомментировать при необходимости) ---
  // debug_lcd_pins();     // Вывод состояния пинов в верхнюю строку экрана
  // debug_serial_pins();  // Вывод состояния пинов в последовательный порт
  // lcd_run_test_counters(); // Искусственный бег цифр на экране
  
  delay(10); // Защита от перегрузки процессора
}

// =========================================================================
// 5. ИЗОЛИРОВАННЫЕ ТЕСТОВЫЕ МЕТОДЫ (Отладка)
// =========================================================================

void debug_lcd_pins() {
  lcd.setCursor(0, 0);
  lcd.print("A:"); lcd.print(digitalRead(ENC_A_PIN));
  lcd.print(" B:"); lcd.print(digitalRead(ENC_B_PIN));
  lcd.print(" BTN:"); lcd.print(digitalRead(ENC_BTN_PIN));
  lcd.print("    "); 
}

void debug_serial_pins() {
  static unsigned long lastLog = 0;
  if (millis() - lastLog >= 100) {
    lastLog = millis();
    Serial.print("A: "); Serial.print(digitalRead(ENC_A_PIN));
    Serial.print(" | B: "); Serial.println(digitalRead(ENC_B_PIN));
  }
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