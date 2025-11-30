/*
  Будильник рассвета на Wemos Mini (ESP8266)
  Версия 2.0 - с веб-интерфейсом GyverPortal и множественными будильниками
  
  Особенности:
  - AC dimmer module от RobotDyn (встроенный детектор нуля)
  - NTP синхронизация времени
  - Веб-интерфейс на GyverPortal
  - До 20 будильников
  - Для каждого будильника: дни недели, время, продолжительность рассвета, продолжительность работы лампы
*/

// *************************** НАСТРОЙКИ ***************************
#define WIFI_SSID "SSID"        // SSID WiFi сети
#define WIFI_PASS "PASS"        // Пароль WiFi сети

#define GMT_OFFSET 5        // Часовой пояс (GMT+3 для Москвы)
#define NTP_SERVER "pool.ntp.org"

#define DEVICE_NAME "dawn-clock"      // Имя устройства для mDNS (.local доступ)

#define MAX_ALARMS 20       // Максимальное количество будильников

// Продолжительность рассвета по умолчанию (минуты)
#define DEFAULT_DAWN_DURATION 20
// Продолжительность работы лампы после будильника по умолчанию (минуты)
#define DEFAULT_LAMP_DURATION 30

// Настройки яркости
#define DAWN_MIN 50         // Минимальная яркость (0-255)
#define DAWN_MAX 200        // Максимальная яркость (0-255)

// ************ ПИНЫ ************
// Wemos Mini pin mapping:
// D1 = GPIO5, D2 = GPIO4, D3 = GPIO0, D4 = GPIO2, D5 = GPIO14, D6 = GPIO12, D7 = GPIO13, D8 = GPIO15
#define ZERO_PIN D1         // Пин детектора нуля (Z-C) от AC dimmer module
#define DIM_PIN D5          // Пин управления яркостью (DIM) от AC dimmer module
#define SWITCH_PIN D3       // Пин физического переключателя (GPIO0)

// ***************** СТРУКТУРЫ ДАННЫХ *****************
// Структура одного будильника
struct Alarm {
  bool enabled;              // Включен/выключен
  uint8_t hour;              // Час (0-23)
  uint8_t minute;            // Минута (0-59)
  uint8_t daysOfWeek;        // Дни недели (битовая маска: bit0=Пн, bit1=Вт, ..., bit6=Вс)
  uint8_t dawnDuration;      // Продолжительность рассвета (минуты, 5-60)
  uint16_t lampDuration;     // Продолжительность работы лампы после будильника (минуты)
};

// Структура всех будильников
struct AlarmData {
  Alarm alarms[MAX_ALARMS];
  uint8_t magic;             // Магическое число для проверки валидности данных
};

// Состояние активного будильника
struct ActiveAlarmState {
  uint8_t alarmIndex;        // Индекс активного будильника
  bool dawnActive;           // Рассвет активен
  bool alarmActive;          // Будильник активен (лампa работает)
  uint32_t dawnStartTime;    // Время начала рассвета (Unix timestamp)
  uint32_t alarmStartTime;   // Время начала будильника (Unix timestamp)
  uint32_t lampEndTime;      // Время окончания работы лампы (Unix timestamp)
  uint8_t currentBrightness; // Текущая яркость (0-255)
};

// ***************** ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ *****************
#include <ESP8266WiFi.h>
#include <time.h>
#include <EEPROM.h>
#include <GyverPortal.h>
#include <GyverTimer.h>

GyverPortal ui;
AlarmData alarmData;
ActiveAlarmState activeState;
bool settingsChanged = false;

// Таймеры
GTimer_ms ntpSyncTimer(3600000);      // Синхронизация NTP каждый час
GTimer_ms alarmCheckTimer(60000);     // Проверка будильников каждую минуту
GTimer_ms brightnessUpdateTimer(100); // Обновление яркости каждые 100мс

// Переменные для работы с AC dimmer
volatile bool zeroCross = false;
volatile uint32_t lastZeroCrossTime = 0;
volatile uint32_t zeroCrossTime = 0;
uint8_t targetBrightness = 0;
uint8_t currentDuty = 0;
bool dimmerPulseNeeded = false;
uint32_t dimmerPulseTime = 0;

// Ручное управление лампой
bool manualMode = false;        // Режим ручного управления
uint8_t manualBrightness = 0;    // Яркость в ручном режиме (0-255)

// Физический переключатель
bool switchState = false;        // Текущее состояние переключателя
bool lastSwitchState = false;    // Предыдущее состояние для определения изменения
uint32_t lastSwitchDebounce = 0; // Время последнего изменения для debounce
const uint32_t SWITCH_DEBOUNCE_MS = 50; // Время debounce в миллисекундах
bool switchOverride = false;     // Флаг принудительного управления через переключатель

// Forward declaration для прерывания
void ICACHE_RAM_ATTR zeroCrossISR();

// ***************** ФУНКЦИИ ИНИЦИАЛИЗАЦИИ *****************
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n=== Dawn Clock v2.0 ==="));

  // Инициализация пинов
  pinMode(DIM_PIN, OUTPUT);
  digitalWrite(DIM_PIN, LOW);
  pinMode(ZERO_PIN, INPUT_PULLUP);
  pinMode(SWITCH_PIN, INPUT_PULLUP);  // Переключатель с подтяжкой к VCC
  
  // Настройка прерывания для детектора нуля
  // Для RobotDyn AC dimmer module требуется фазовое управление через прерывания
  attachInterrupt(digitalPinToInterrupt(ZERO_PIN), zeroCrossISR, FALLING);
  
  // Инициализация состояния переключателя
  lastSwitchState = digitalRead(SWITCH_PIN);
  switchState = lastSwitchState;

  // Инициализация EEPROM
  EEPROM.begin(sizeof(AlarmData) + 10);
  loadAlarms();

  // Подключение к WiFi
  connectWiFi();

  // Настройка NTP
  configTime(GMT_OFFSET * 3600, 0, NTP_SERVER);
  syncTime();

  // Инициализация GyverPortal с mDNS
  ui.attachBuild(build);
  ui.attach(action);
  ui.start(DEVICE_NAME);  // Запуск с mDNS именем для доступа через .local

  // Инициализация состояния
  activeState.alarmIndex = 255; // 255 = нет активного будильника
  activeState.dawnActive = false;
  activeState.alarmActive = false;
  activeState.currentBrightness = 0;

  Serial.println(F("Setup complete!"));
}

void loop() {
  ui.tick();
  
  // Синхронизация NTP
  if (ntpSyncTimer.isReady()) {
    syncTime();
  }

  // Проверка будильников
  if (alarmCheckTimer.isReady()) {
    checkAlarms();
  }

  // Обновление яркости
  if (brightnessUpdateTimer.isReady()) {
    updateBrightness();
  }

  // Обработка физического переключателя
  handleSwitch();

  // Управление диммером
  dimmerControl();

  // Сохранение настроек при изменении
  if (settingsChanged) {
    saveAlarms();
    settingsChanged = false;
  }
}

// ***************** ФУНКЦИИ РАБОТЫ С ВРЕМЕНЕМ *****************
void syncTime() {
  Serial.println(F("Syncing time with NTP..."));
  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 100000 && attempts < 10) {
    delay(500);
    now = time(nullptr);
    attempts++;
  }
  if (now >= 100000) {
    Serial.print(F("Time synced: "));
    Serial.println(ctime(&now));
  } else {
    Serial.println(F("Failed to sync time"));
  }
}

time_t getCurrentTime() {
  return time(nullptr);
}

struct tm* getCurrentTimeStruct() {
  static time_t now;
  now = getCurrentTime();
  return localtime(&now);
}

uint8_t getCurrentDayOfWeek() {
  struct tm* timeinfo = getCurrentTimeStruct();
  // tm_wday: 0=Вс, 1=Пн, ..., 6=Сб
  // Наша маска: bit0=Пн, bit1=Вт, ..., bit6=Вс
  if (timeinfo->tm_wday == 0) return 64; // Вс = bit6
  return 1 << (timeinfo->tm_wday - 1);   // Пн-Сб = bit0-bit5
}

// ***************** ФУНКЦИИ РАБОТЫ С БУДИЛЬНИКАМИ *****************
void loadAlarms() {
  EEPROM.get(0, alarmData);
  
  // Проверка валидности данных (магическое число = 0xAA)
  if (alarmData.magic != 0xAA) {
    Serial.println(F("EEPROM data invalid, initializing defaults..."));
    initDefaultAlarms();
    saveAlarms();
  } else {
    Serial.println(F("Alarms loaded from EEPROM"));
  }
}

void saveAlarms() {
  alarmData.magic = 0xAA;
  EEPROM.put(0, alarmData);
  EEPROM.commit();
  Serial.println(F("Alarms saved to EEPROM"));
}

void initDefaultAlarms() {
  for (int i = 0; i < MAX_ALARMS; i++) {
    alarmData.alarms[i].enabled = false;
    alarmData.alarms[i].hour = 7;
    alarmData.alarms[i].minute = 0;
    alarmData.alarms[i].daysOfWeek = 0b0111110; // Пн-Пт
    alarmData.alarms[i].dawnDuration = DEFAULT_DAWN_DURATION;
    alarmData.alarms[i].lampDuration = DEFAULT_LAMP_DURATION;
  }
}

void checkAlarms() {
  struct tm* timeinfo = getCurrentTimeStruct();
  uint8_t currentHour = timeinfo->tm_hour;
  uint8_t currentMinute = timeinfo->tm_min;
  uint8_t currentDay = getCurrentDayOfWeek();
  time_t currentTime = getCurrentTime();

  // Если уже есть активный будильник, проверяем его состояние
  if (activeState.alarmIndex < MAX_ALARMS) {
    Alarm& alarm = alarmData.alarms[activeState.alarmIndex];
    
    if (activeState.dawnActive && !activeState.alarmActive) {
      // Рассвет активен, проверяем, не настало ли время будильника
      if (currentTime >= activeState.alarmStartTime) {
        activeState.alarmActive = true;
        activeState.currentBrightness = DAWN_MAX;
        targetBrightness = DAWN_MAX;
        Serial.println(F("Alarm time reached!"));
      }
    } else if (activeState.alarmActive) {
      // Будильник активен, проверяем, не закончилось ли время работы лампы
      if (currentTime >= activeState.lampEndTime) {
        // Выключаем будильник
        stopAlarm();
        return;
      }
    }
  }

  // Если нет активного будильника, ищем новый
  if (activeState.alarmIndex >= MAX_ALARMS) {
    for (int i = 0; i < MAX_ALARMS; i++) {
      Alarm& alarm = alarmData.alarms[i];
      
      if (!alarm.enabled) continue;
      if (!(alarm.daysOfWeek & currentDay)) continue; // Не подходит день недели
      
      // Вычисляем время начала рассвета
      int dawnStartMinute = alarm.minute - alarm.dawnDuration;
      int dawnStartHour = alarm.hour;
      
      if (dawnStartMinute < 0) {
        dawnStartMinute += 60;
        dawnStartHour--;
        if (dawnStartHour < 0) dawnStartHour = 23;
      }
      
      // Проверяем, настало ли время начала рассвета
      if (currentHour == dawnStartHour && currentMinute == dawnStartMinute) {
        startDawn(i);
        break;
      }
    }
  }
}

void startDawn(uint8_t alarmIndex) {
  Alarm& alarm = alarmData.alarms[alarmIndex];
  
  activeState.alarmIndex = alarmIndex;
  activeState.dawnActive = true;
  activeState.alarmActive = false;
  activeState.currentBrightness = DAWN_MIN;
  targetBrightness = DAWN_MIN;
  
  time_t currentTime = getCurrentTime();
  activeState.dawnStartTime = currentTime;
  
  // Вычисляем время начала будильника
  struct tm* timeinfo = localtime(&currentTime);
  timeinfo->tm_hour = alarm.hour;
  timeinfo->tm_min = alarm.minute;
  timeinfo->tm_sec = 0;
  activeState.alarmStartTime = mktime(timeinfo);
  
  // Вычисляем время окончания работы лампы
  activeState.lampEndTime = activeState.alarmStartTime + (alarm.lampDuration * 60);
  
  Serial.print(F("Dawn started for alarm #"));
  Serial.println(alarmIndex);
}

void stopAlarm() {
  activeState.alarmIndex = 255;
  activeState.dawnActive = false;
  activeState.alarmActive = false;
  activeState.currentBrightness = 0;
  targetBrightness = 0;
  digitalWrite(DIM_PIN, LOW);
  Serial.println(F("Alarm stopped"));
}

void updateBrightness() {
  // Если переключатель активен, не обновляем яркость от других источников
  if (switchOverride) {
    return;
  }
  
  // Если включен ручной режим, не обновляем яркость от будильников
  if (manualMode) {
    targetBrightness = manualBrightness;
    return;
  }

  if (!activeState.dawnActive && !activeState.alarmActive) {
    targetBrightness = 0;
    return;
  }

  if (activeState.alarmActive) {
    // После начала будильника яркость максимальная
    targetBrightness = DAWN_MAX;
    activeState.currentBrightness = DAWN_MAX;
    return;
  }

  // Во время рассвета плавно увеличиваем яркость
  if (activeState.dawnActive) {
    Alarm& alarm = alarmData.alarms[activeState.alarmIndex];
    time_t currentTime = getCurrentTime();
    uint32_t elapsed = currentTime - activeState.dawnStartTime;
    uint32_t totalDuration = alarm.dawnDuration * 60; // в секундах
    
    if (elapsed >= totalDuration) {
      targetBrightness = DAWN_MAX;
    } else {
      // Линейное нарастание яркости
      uint16_t brightnessRange = DAWN_MAX - DAWN_MIN;
      targetBrightness = DAWN_MIN + (brightnessRange * elapsed / totalDuration);
    }
    activeState.currentBrightness = targetBrightness;
  }
}

// ***************** ФУНКЦИИ ОБРАБОТКИ ПЕРЕКЛЮЧАТЕЛЯ *****************
void handleSwitch() {
  // Читаем состояние переключателя
  bool currentReading = digitalRead(SWITCH_PIN);
  
  // Debounce - проверяем, что состояние стабильно
  if (currentReading != lastSwitchState) {
    lastSwitchDebounce = millis();
  }
  
  // Если состояние стабильно дольше времени debounce
  if ((millis() - lastSwitchDebounce) > SWITCH_DEBOUNCE_MS) {
    // Если состояние изменилось
    if (currentReading != switchState) {
      switchState = currentReading;
      
      if (switchState == LOW) {  // Переключатель нажат (подтяжка к GND)
        // Включаем лампу
        switchOverride = true;
        manualMode = true;
        manualBrightness = 200;  // Устанавливаем яркость на 200 (почти максимум)
        targetBrightness = manualBrightness;
        
        // Останавливаем активный будильник при включении переключателя
        if (activeState.alarmIndex < MAX_ALARMS) {
          stopAlarm();
        }
        
        Serial.println(F("Switch: Lamp ON"));
      } else {  // Переключатель отпущен
        // Выключаем лампу
        switchOverride = true;
        targetBrightness = 0;
        manualBrightness = 0;
        digitalWrite(DIM_PIN, LOW);
        
        // Останавливаем активный будильник при выключении переключателя
        if (activeState.alarmIndex < MAX_ALARMS) {
          stopAlarm();
        }
        
        Serial.println(F("Switch: Lamp OFF"));
      }
    }
  }
  
  lastSwitchState = currentReading;
  
  // Если переключатель активен, принудительно устанавливаем яркость
  if (switchOverride) {
    if (switchState == LOW) {
      // Переключатель включен - принудительно включаем лампу
      targetBrightness = manualBrightness;
    } else {
      // Переключатель выключен - принудительно выключаем лампу
      targetBrightness = 0;
      digitalWrite(DIM_PIN, LOW);
    }
  }
}

// ***************** ФУНКЦИИ УПРАВЛЕНИЯ ДИММЕРОМ *****************
// Для RobotDyn AC dimmer module с встроенным детектором нуля
// Модуль сам обрабатывает синхронизацию с нулем, достаточно подавать PWM
// Но для более точного управления используем прерывания

// Forward declaration
void ICACHE_RAM_ATTR zeroCrossISR();

void ICACHE_RAM_ATTR zeroCrossISR() {
  // Минимальная работа в прерывании - только установка флага и времени
  zeroCross = true;
  zeroCrossTime = micros();
}

void dimmerControl() {
  // Для RobotDyn AC dimmer module требуется фазовое управление через прерывания
  // Простой PWM не работает для AC диммера
  
  // Если переключатель принудительно выключен, не обрабатываем диммер
  if (switchOverride && switchState == HIGH) {
    digitalWrite(DIM_PIN, LOW);
    dimmerPulseNeeded = false;
    return;
  }
  
  if (targetBrightness == 0) {
    digitalWrite(DIM_PIN, LOW);
    dimmerPulseNeeded = false;
    return;
  }

  // Обработка детектора нуля (без задержек в прерывании)
  if (zeroCross) {
    zeroCross = false;
    lastZeroCrossTime = zeroCrossTime;
    
    // Вычисляем задержку в микросекундах для фазового управления
    // Для 50Hz: период = 10000 мкс, полупериод = 5000 мкс
    // Яркость обратно пропорциональна задержке (больше яркость = меньше задержка)
    // Минимальная задержка ~100 мкс, максимальная ~5000 мкс (почти полный полупериод)
    uint16_t delay_us = map(255 - targetBrightness, 0, 255, 100, 5000);
    
    // Планируем импульс через заданную задержку
    dimmerPulseNeeded = true;
    dimmerPulseTime = zeroCrossTime + delay_us;
  }
  
  // Подаем импульс в нужное время (в основном цикле, без блокировки прерываний)
  if (dimmerPulseNeeded) {
    uint32_t currentTime = micros();
    // Учитываем переполнение micros() (происходит каждые ~70 секунд)
    if ((currentTime >= dimmerPulseTime) || 
        (dimmerPulseTime > 5000000 && currentTime < 1000000)) {
      // Включаем симистор (подаем импульс на DIM пин)
      digitalWrite(DIM_PIN, HIGH);
      
      // Держим импульс небольшое время (для надежного открытия симистора)
      delayMicroseconds(50);
      
      // Выключаем (симистор останется открытым до следующего нуля)
      digitalWrite(DIM_PIN, LOW);
      
      dimmerPulseNeeded = false;
    }
  }
}

// ***************** ВЕБ-ИНТЕРФЕЙС GYVERPORTAL *****************
// Вспомогательная функция для вывода дней недели
void buildDaysOfWeek(int alarmIndex, uint8_t daysOfWeek) {
  const char* dayNames[] = {"Пн", "Вт", "Ср", "Чт", "Пт", "Сб", "Вс"};
  for (int d = 0; d < 7; d++) {
    bool dayChecked = (daysOfWeek >> d) & 1;
    GP.LABEL(dayNames[d]);
    GP.CHECK("d" + String(alarmIndex) + "d" + String(d), dayChecked);
  }
}

void build() {
  GP.BUILD_BEGIN();
  GP.THEME(GP_DARK);
  GP.TITLE("Dawn Clock - Alarm with Sunrise Simulation");

  // Ручное управление лампой
  GP.HR();
  GP.TITLE("Ручное управление лампой");
  GP.LABEL("Яркость: ");
  GP.LABEL(String(manualBrightness), "brightness_value");
  GP.BREAK();
  GP.SLIDER("manual_brightness", manualBrightness, 0, 255);
  GP.BREAK();
  GP.SWITCH("manual_mode", manualMode);
  GP.LABEL("Ручной режим");
  GP.BREAK();
  if (manualMode) {
    GP.LABEL("Внимание: Ручной режим активен. Будильники не будут управлять лампой.");
  }
  GP.BREAK();

  // Текущее время
  GP.HR();
  GP.LABEL("Текущее время:");
  struct tm* timeinfo = getCurrentTimeStruct();
  char timeStr[20];
  sprintf(timeStr, "%02d:%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
  GP.LABEL(timeStr);
  GP.BREAK();

  // Статус активного будильника
  if (activeState.alarmIndex < MAX_ALARMS) {
    GP.LABEL("Активный будильник:");
    if (activeState.dawnActive) {
      GP.LABEL("Рассвет активен");
      char progress[50];
      Alarm& alarm = alarmData.alarms[activeState.alarmIndex];
      time_t currentTime = getCurrentTime();
      uint32_t elapsed = currentTime - activeState.dawnStartTime;
      uint32_t total = alarm.dawnDuration * 60;
      int percent = (elapsed * 100) / total;
      sprintf(progress, "Прогресс: %d%%", percent);
      GP.LABEL(progress);
    }
    if (activeState.alarmActive) {
      GP.LABEL("Будильник активен");
    }
    GP.BREAK();
  }

  GP.HR();
  GP.TITLE("Будильники");

  // Таблица будильников с растянутой колонкой для дней недели
  // Добавляем стиль через HTML тег
  extern String* _GPP;
  *_GPP += F("<style>table td:nth-child(4) { min-width: 250px; width: 30%; }</style>\n");
  
  GP.TABLE_BEGIN();
  
  // Заголовок таблицы
  GP.TR();
  GP.TD(GP_CENTER); GP.LABEL("№");
  GP.TD(GP_CENTER); GP.LABEL("Вкл");
  GP.TD(GP_CENTER); GP.LABEL("Время");
  GP.TD(GP_CENTER); GP.LABEL("Дни недели");
  GP.TD(GP_CENTER); GP.LABEL("Рассвет (мин)");
  GP.TD(GP_CENTER); GP.LABEL("Лампа (мин)");
  GP.TD(GP_CENTER); GP.LABEL("Действия");

  // Строки будильников - показываем только активные (enabled) или те, у которых есть настройки
  int visibleCount = 0;
  for (int i = 0; i < MAX_ALARMS; i++) {
    Alarm& alarm = alarmData.alarms[i];
    
    // Пропускаем полностью пустые будильники (не созданные)
    // Показываем только те, которые enabled или имеют хотя бы один день недели
    if (!alarm.enabled && alarm.daysOfWeek == 0) {
      continue; // Пропускаем удаленные/пустые будильники
    }
    
    visibleCount++;
    GP.TR();
    
    // Номер (показываем порядковый номер видимых будильников)
    GP.TD(GP_CENTER);
    GP.LABEL(String(visibleCount));
    
    // Включен/выключен
    GP.TD(GP_CENTER);
    GP.SWITCH("en" + String(i), alarm.enabled);
    
    // Время
    GP.TD(GP_CENTER);
    GPtime alarmTime;
    alarmTime.hour = alarm.hour;
    alarmTime.minute = alarm.minute;
    GP.TIME("tm" + String(i), alarmTime);
    
    // Дни недели (чекбоксы) - растянутая колонка
    GP.TD();
    buildDaysOfWeek(i, alarm.daysOfWeek);
    
    // Продолжительность рассвета
    GP.TD(GP_CENTER);
    GP.NUMBER("dd" + String(i), "", alarm.dawnDuration);
    
    // Продолжительность работы лампы
    GP.TD(GP_CENTER);
    GP.NUMBER("ld" + String(i), "", alarm.lampDuration);
    
    // Кнопка удаления
    GP.TD(GP_CENTER);
    GP.BUTTON("del" + String(i), "Удалить");
  }
  
  // Если нет видимых будильников, показываем сообщение
  if (visibleCount == 0) {
    GP.TR();
    GP.TD(GP_CENTER, 7); // Объединяем все колонки
    GP.LABEL("Нет будильников. Нажмите 'Добавить будильник' для создания.");
  }
  
  GP.TABLE_END();

  // Кнопка добавления нового будильника
  GP.BREAK();
  GP.BUTTON("add", "Добавить будильник");

  // Кнопка остановки текущего будильника
  if (activeState.alarmIndex < MAX_ALARMS) {
    GP.BREAK();
    GP.BUTTON("stop", "Остановить будильник");
  }

  GP.BUILD_END();
}

void action() {
  // Обработка ручного режима
  if (ui.clickBool("manual_mode", manualMode)) {
    if (manualMode) {
      // Включаем ручной режим - останавливаем активный будильник
      if (activeState.alarmIndex < MAX_ALARMS) {
        stopAlarm();
      }
      // Сбрасываем переключатель, если управление через веб-интерфейс
      switchOverride = false;
      targetBrightness = manualBrightness;
      Serial.println(F("Manual mode enabled"));
    } else {
      // Выключаем ручной режим
      targetBrightness = 0;
      digitalWrite(DIM_PIN, LOW);
      // Сбрасываем переключатель
      switchOverride = false;
      Serial.println(F("Manual mode disabled"));
    }
  }

  // Обработка изменения яркости в ручном режиме
  // Пробуем сначала через int (слайдер обычно возвращает int)
  int brightnessInt = manualBrightness;
  if (ui.clickInt("manual_brightness", brightnessInt)) {
    if (brightnessInt >= 0 && brightnessInt <= 255) {
      manualBrightness = brightnessInt;
      if (manualMode) {
        // Сбрасываем переключатель при управлении через веб-интерфейс
        switchOverride = false;
        targetBrightness = manualBrightness;
        // Яркость будет применена при следующем прерывании от детектора нуля
        Serial.print(F("Manual brightness set to: "));
        Serial.println(manualBrightness);
      }
    }
  }
  
  // Также пробуем через float на случай, если слайдер возвращает float
  float brightnessFloat = manualBrightness;
  if (ui.clickFloat("manual_brightness", brightnessFloat)) {
    int brightnessInt = (int)brightnessFloat;
    if (brightnessInt >= 0 && brightnessInt <= 255 && brightnessInt != manualBrightness) {
      manualBrightness = brightnessInt;
      if (manualMode) {
        // Сбрасываем переключатель при управлении через веб-интерфейс
        switchOverride = false;
        targetBrightness = manualBrightness;
        // Яркость будет применена при следующем прерывании от детектора нуля
        Serial.print(F("Manual brightness (float) set to: "));
        Serial.println(manualBrightness);
      }
    }
  }

  // Обработка переключения включен/выключен
  for (int i = 0; i < MAX_ALARMS; i++) {
    if (ui.clickBool("en" + String(i), alarmData.alarms[i].enabled)) {
      settingsChanged = true;
    }
  }

  // Обработка изменения времени
  for (int i = 0; i < MAX_ALARMS; i++) {
    GPtime timeVal;
    if (ui.clickTime("tm" + String(i), timeVal)) {
      alarmData.alarms[i].hour = timeVal.hour;
      alarmData.alarms[i].minute = timeVal.minute;
      settingsChanged = true;
    }
  }

  // Обработка дней недели
  for (int i = 0; i < MAX_ALARMS; i++) {
    uint8_t days = alarmData.alarms[i].daysOfWeek; // Сохраняем текущее состояние
    for (int d = 0; d < 7; d++) {
      bool checked = false;
      if (ui.clickBool("d" + String(i) + "d" + String(d), checked)) {
        if (checked) {
          days |= (1 << d);
        } else {
          days &= ~(1 << d);
        }
        settingsChanged = true;
      }
    }
    alarmData.alarms[i].daysOfWeek = days;
  }

  // Обработка продолжительности рассвета
  for (int i = 0; i < MAX_ALARMS; i++) {
    int val = alarmData.alarms[i].dawnDuration;
    if (ui.clickInt("dd" + String(i), val)) {
      if (val >= 5 && val <= 60) {
        alarmData.alarms[i].dawnDuration = val;
        settingsChanged = true;
      }
    }
  }

  // Обработка продолжительности работы лампы
  for (int i = 0; i < MAX_ALARMS; i++) {
    int val = alarmData.alarms[i].lampDuration;
    if (ui.clickInt("ld" + String(i), val)) {
      if (val > 0 && val <= 600) {
        alarmData.alarms[i].lampDuration = val;
        settingsChanged = true;
      }
    }
  }

  // Обработка удаления будильника
  for (int i = 0; i < MAX_ALARMS; i++) {
    if (ui.click("del" + String(i))) {
      // Полностью очищаем будильник при удалении
      alarmData.alarms[i].enabled = false;
      alarmData.alarms[i].daysOfWeek = 0; // Очищаем дни недели, чтобы будильник не отображался
      alarmData.alarms[i].hour = 0;
      alarmData.alarms[i].minute = 0;
      alarmData.alarms[i].dawnDuration = DEFAULT_DAWN_DURATION;
      alarmData.alarms[i].lampDuration = DEFAULT_LAMP_DURATION;
      settingsChanged = true;
      Serial.print(F("Alarm #"));
      Serial.print(i);
      Serial.println(F(" deleted"));
    }
  }

  // Обработка добавления нового будильника
  if (ui.click("add")) {
    // Ищем первый пустой слот (где daysOfWeek == 0)
    for (int i = 0; i < MAX_ALARMS; i++) {
      if (alarmData.alarms[i].daysOfWeek == 0) {
        alarmData.alarms[i].enabled = true;
        alarmData.alarms[i].hour = 7;
        alarmData.alarms[i].minute = 0;
        alarmData.alarms[i].daysOfWeek = 0b0111110; // Пн-Пт
        alarmData.alarms[i].dawnDuration = DEFAULT_DAWN_DURATION;
        alarmData.alarms[i].lampDuration = DEFAULT_LAMP_DURATION;
        settingsChanged = true;
        Serial.print(F("New alarm added at slot #"));
        Serial.println(i);
        break;
      }
    }
  }

  // Обработка остановки будильника
  if (ui.click("stop")) {
    stopAlarm();
  }
}

// ***************** ФУНКЦИИ ПОДКЛЮЧЕНИЯ К WIFI *****************
void connectWiFi() {
  Serial.print(F("Connecting to WiFi: "));
  Serial.println(WIFI_SSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print(F("Connected! IP: "));
    Serial.println(WiFi.localIP());
    Serial.print(F("Access device at: http://"));
    Serial.print(DEVICE_NAME);
    Serial.println(F(".local"));
  } else {
    Serial.println();
    Serial.println(F("Failed to connect to WiFi"));
    // Запускаем точку доступа для настройки
    WiFi.mode(WIFI_AP);
    WiFi.softAP("DawnClock_Setup");
    Serial.print(F("AP started. IP: "));
    Serial.println(WiFi.softAPIP());
  }
}
