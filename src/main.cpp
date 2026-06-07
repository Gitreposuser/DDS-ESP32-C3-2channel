#include <Arduino.h>
#include <U8g2lib.h>
#include <SPI.h>
#include <MD_AD9833.h>

// --- Настройки Пинов ---
#define DISP_CLOCK   3
#define DISP_DATA    1
#define DISP_CS      5

#define ENC_A_PIN    0
#define ENC_B_PIN    2
#define ENC_BTN_PIN  9  
#define HARDWARE_LED 8

#define DDS_CH1_FSYNC 7 // Пин выбора 1-го канала генератора

// --- Инициализация объектов ---
U8G2_ST7920_128X64_F_SW_SPI u8g2(U8G2_R0, DISP_CLOCK, DISP_DATA, DISP_CS, U8X8_PIN_NONE);

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

/*
// --- Низкоуровневые методы управления AD9833 ---
void dds_write(uint16_t data) {
  // Настройки SPI: 4 МГц, MSBFIRST, SPI_MODE2 (для AD9833 такты CPOL=1, CPHA=0)
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE2));
  
  digitalWrite(DDS_CH1_FSYNC, LOW);  // Активируем чип
  SPI.transfer16(data);              // Передаем 16 бит данных
  
  digitalWrite(DDS_CH1_FSYNC, HIGH); // Деактивируем чип
  SPI.endTransaction();
}

void dds_set_frequency(uint32_t frequency) {
  // Рассчитываем 28-битное слово частоты для кварца 25 МГц
  // Формула: (Freq * 2^28) / 25000000. Коэффициент равен 10.73741824
  uint32_t freqWord = (uint32_t)((double)frequency * 10.73741824);

  // Разбиваем 28 бит на два 14-битных куска
  uint16_t freqLSB = (freqWord & 0x3FFF) | 0x4000;         // Регистр FREQ0 (Бит 14 = 0, Бит 15 = 1)
  uint16_t freqMSB = ((freqWord >> 14) & 0x3FFF) | 0x4000;  // Туда же оставшиеся биты

  // Важно: Управляющее слово БЕЗ полного Reset, но с битом B28=1 (загрузка в два захода)
  // И с установленным битом MODE=1 (0x0002) для генерации ТРЕУГОЛЬНИКА
  uint16_t controlWord = 0x2002;

  // Отправляем последовательность
  dds_write(controlWord); // Говорим чипу: "Сейчас загрузим частоту и включим треугольник"
  dds_write(freqLSB);     // Шлем младшие 14 бит
  dds_write(freqMSB);     // Шлем старшие 14 бит
}
*/

// --- Методы Инициализации (Setup) ---
void screen_init() {
  u8g2.begin();
  Serial.println("Экран успешно инициализирован.");
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

/*
void dds_init() {
  pinMode(DDS_CH1_FSYNC, OUTPUT);
  digitalWrite(DDS_CH1_FSYNC, HIGH); // Изначально отключаем шину генератора
  
  // Инициализируем аппаратный SPI
  // По умолчанию на ESP32-C3: SCLK = GPIO4, MOSI = GPIO6
  SPI.begin(4, -1, 6, -1); 
  delay(10);
  
  // Сброс при старте
  dds_write(0x2100); 
  delay(10);
  
  dds_set_frequency(encoderCounter);
  Serial.println("Генератор инициализирован в режим ТРЕУГОЛЬНИКА.");
}
*/

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

void update_screen() {
  u8g2.clearBuffer();         
  
  // Шапка
  u8g2.setFont(u8g2_font_7x14_tr); 
  u8g2.drawStr(15, 18, "DDS Generator"); 
  u8g2.drawLine(0, 24, 128, 24);
  
  // Инфо о канале 1
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(5, 38, "CH1 Wave: TRIANGLE");
  
  // Крупный вывод текущей частоты
  u8g2.setFont(u8g2_font_ncenB12_tr); 
  char freqStr[20];
  sprintf(freqStr, "F: %d Hz", encoderCounter);
  u8g2.drawStr(5, 56, freqStr);

  u8g2.sendBuffer();
}

void startup_message() {
  u8g2.clearBuffer();         
  u8g2.setFont(u8g2_font_7x14_tr); 
  u8g2.drawStr(18, 24, "DDS Generator"); 

  // Рисуем красивую разделительную линию под заголовком для солидности!
  // drawLine(x1, y1, x2, y2)
  u8g2.drawLine(0, 32, 128, 32);
  
  // Выведем тестовую подпись чуть ниже
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(30, 50, "System Ready");

  u8g2.sendBuffer();    
  delay(1000);      
}

// --- Главные функции Arduino ---
void setup() {
  system_init();   // Базовые настройки МК и Serial
  screen_init();   // Настройка экрана
  encoder_init();  // Настройка энкодера
  dds_init();
  startup_message();
}

void loop() {
  check_encoder(); // Опрос кнопок и периферии
  update_screen(); // Вывод графики на экран
  delay(30);       // Небольшая пауза для стабильности интерфейса
}