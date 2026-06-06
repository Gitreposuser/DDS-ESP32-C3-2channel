#include <Arduino.h>
#include <U8g2lib.h>

// --- Настройки Пинов ---
#define DISP_CLOCK   3
#define DISP_DATA    1
#define DISP_CS      5

#define ENC_A_PIN    0
#define ENC_B_PIN    2
#define ENC_BTN_PIN  9  
#define HARDWARE_LED 8

// --- Инициализация объектов ---
U8G2_ST7920_128X64_F_SW_SPI u8g2(U8G2_R0, DISP_CLOCK, DISP_DATA, DISP_CS, U8X8_PIN_NONE);

// --- Глобальные переменные ---
volatile int encoderCounter = 0;
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

void system_init() {
  Serial.begin(115200);
  delay(500);
  Serial.println("--- Старт модульной системы ---");
  
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
}

void update_screen() {
  u8g2.clearBuffer();         
  
  // Шапка
  u8g2.setFont(u8g2_font_7x14_tr); 
  u8g2.drawStr(12, 20, "Encoder Test"); 
  u8g2.drawLine(0, 26, 128, 26);
  
  // Вывод значения энкодера
  u8g2.setFont(u8g2_font_ncenB14_tr); 
  char countStr[15];
  sprintf(countStr, "Val: %d", encoderCounter);
  u8g2.drawStr(15, 48, countStr);
  
  // Вывод статуса кнопки
  u8g2.setFont(u8g2_font_6x10_tr);
  if (buttonPressed) {
    u8g2.drawStr(5, 62, "Button: CLICK!");
  } else {
    u8g2.drawStr(5, 62, "Button: Wait...");
  }

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
  startup_message();
}

void loop() {
  check_encoder(); // Опрос кнопок и периферии
  update_screen(); // Вывод графики на экран
  delay(30);       // Небольшая пауза для стабильности интерфейса
}