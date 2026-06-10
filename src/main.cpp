#include <Arduino.h>
#include <SPI.h>
#include <MD_AD9833.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// Назначаем I2C на ваши прежние пины дисплея
#define I2C_SDA 1  
#define I2C_SCL 3

#define ENC_A_PIN    0
#define ENC_B_PIN    2
#define ENC_BTN_PIN  9  
#define HARDWARE_LED 8

#define DDS_CH1_FSYNC 7 // Пин выбора 1-го канала генератора

// Стандартный I2C адрес для модулей PCF8574 — обычно 0x27 (редко 0x3F)
// Дисплей у нас 20 символов на 4 строки
LiquidCrystal_I2C lcd(0x27, 20, 4);

// Создаем объект генератора для первого канала
MD_AD9833 gen(DDS_CH1_FSYNC);

// --- Глобальные переменные ---
volatile int encoderCounter = 1000;
int lastFrequency = 1000;
bool buttonPressed = false;

// Таблица состояний конечного автомата для энкодера.
// Она учитывает все стабильные переходы фаз и отсекает дребезг.
// Индекс — это комбинация: [предыдущее_состояние_А_В][текущее_состояние_А_В]
const int8_t encoderStates[] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
volatile uint8_t oldState = 0;

// --- Корректный обработчик для энкодеров с фиксацией в положении 11 ---
void IRAM_ATTR readEncoderISR() {
  oldState <<= 2;
  oldState |= (digitalRead(ENC_A_PIN) << 1) | digitalRead(ENC_B_PIN);
  
  // Получаем направление движения из таблицы состояний
  int8_t change = encoderStates[(oldState & 0x0F)];
  
  if (change != 0) {
    // Накапливаем направление
    static int8_t direction = 0;
    direction += change;
    
    // Текущее состояние пинов (последние 2 бита)
    uint8_t currentState = oldState & 0x03; 
    
    // Делаем шаг ТОЛЬКО тогда, когда энкодер встал на физический фиксатор (оба пина в HIGH -> 11 -> это 3 в десятичной)
    if (currentState == 3) {
      if (direction > 0) {
        encoderCounter++;
      } else if (direction < 0) {
        encoderCounter--;
      }
      direction = 0; // Сбрасываем накопитель направления для следующего щелчка
    }
  }
}

// --- Методы Инициализации (Setup) ---
void lcd_init() {
  // Явно инициализируем шину I2C на наших кастомных пинах
  Wire.begin(I2C_SDA, I2C_SCL);
  
  // Инициализация самого дисплея и включение подсветки
  lcd.init();                      
  lcd.backlight(); 
  lcd.clear();
  
  Serial.println("Дисплей 2004A успешно инициализирован.");
}

void encoder_init() {
  pinMode(ENC_A_PIN, INPUT_PULLUP);
  pinMode(ENC_B_PIN, INPUT_PULLUP);
  pinMode(ENC_BTN_PIN, INPUT_PULLUP);
  
  // Читаем стартовое состояние пинов перед запуском прерываний
  oldState = (digitalRead(ENC_A_PIN) << 1) | digitalRead(ENC_B_PIN);
  
  // Вешаем прерывания на ОБА пина для отслеживания полной матрицы состояний
  attachInterrupt(digitalPinToInterrupt(ENC_A_PIN), readEncoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B_PIN), readEncoderISR, CHANGE);
  
  Serial.println("Энкодер (State Machine) и прерывания настроены.");
}

void dds_init() {
  // Запуск железного SPI на пинах ESP32-C3: SCLK=4, MOSI=6
  SPI.begin(4, -1, 6, -1); 
  delay(10);

  gen.begin(); // Внутренний сброс и подготовка чипа AD9833
  
  // Установка стартовых параметров
  gen.setFrequency(MD_AD9833::CHAN_0, encoderCounter);
  gen.setMode(MD_AD9833::MODE_TRIANGLE); // Включаем ТРЕУГОЛЬНИК
  
  Serial.println("Генератор инициализирован через MD_AD9833.");
}

void system_init() {
  Serial.begin(115200);
  delay(500);
  Serial.println("--- DDS Generator System v1.0 ---");
  pinMode(HARDWARE_LED, OUTPUT);
}

// --- Методы Рабочего Цикла (Loop) ---

void check_encoder() {
  // Опрашиваем состояние кнопки (нажата = true)
  buttonPressed = (digitalRead(ENC_BTN_PIN) == LOW);
  
  // Управление светодиодом в зависимости от кнопки
  if (buttonPressed) {
    digitalWrite(HARDWARE_LED, LOW);
  } else {
    digitalWrite(HARDWARE_LED, HIGH);
  }

  // Если энкодер изменил значение — обновляем частоту "на лету"
  if (encoderCounter != lastFrequency) {
    gen.setFrequency(MD_AD9833::CHAN_0, encoderCounter); 
    lastFrequency = encoderCounter;
    
    Serial.print("Частота: ");
    Serial.print(encoderCounter);
    Serial.println(" Гц");
  }
}

// Базовый метод для вывода статического интерфейса
void lcd_draw_interface() {
  lcd.setCursor(0, 0);
  lcd.print("DDS Generator v1.5");
  lcd.setCursor(0, 1);
  lcd.print("--------------------"); // Разделительная линия на 20 символов
  lcd.setCursor(0, 2);
  lcd.print("CH1 Freq: ---- Hz");
  lcd.setCursor(0, 3);
  lcd.print("CH1 Duty: -- %");
}

// Метод динамического обновления тестовых данных
void lcd_run_test_counters() {
  static unsigned long lastUpdate = 0;
  static int testFreq = 1000;
  static int testDuty = 50;
  static bool direction = true;

  // Обновляем данные на экране раз в 100 миллисекунд (без блокирующего delay)
  if (millis() - lastUpdate >= 100) {
    lastUpdate = millis();

    // Симулируем изменение параметров для проверки динамики экрана
    if (direction) {
      testFreq += 100;
      testDuty += 1;
      if (testFreq >= 5000) direction = false;
    } else {
      testFreq -= 100;
      testDuty -= 1;
      if (testFreq <= 100) direction = true;
    }

    // Выводим только изменяющиеся значения, чтобы экран не мерцал
    // Очищаем старые цифры пробелами, если разрядность падает
    lcd.setCursor(10, 2);
    lcd.print(testFreq);
    lcd.print("    "); // Затираем хвосты

    lcd.setCursor(10, 3);
    lcd.print(testDuty);
    lcd.print("  ");   // Затираем хвосты
  }
}

void startup_message() {
  lcd.print("DDS Generator");
  delay(1000);      
}

// --- Главные функции Arduino ---
void setup() {
  system_init();   // Базовые настройки МК и Serial
  lcd_init();      // Запуск I2C и экрана
  encoder_init();  // Настройка энкодера
  dds_init();
  startup_message();
  lcd_draw_interface(); // Рисуем каркас меню один раз при старте
}

void loop() {
  check_encoder(); // Опрос кнопок и периферии
  lcd_run_test_counters();
  delay(30);       // Небольшая пауза для стабильности интерфейса
}