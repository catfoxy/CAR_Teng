#include <GyverDS18.h>
#include <EncButton.h>
#include <GyverSegment.h>

#define DIO_PIN 6
#define CLK_PIN 7

// Инициализация дисплея
Disp1637_4 disp(DIO_PIN, CLK_PIN);

enum SettingMode {
  NO_SETTING = 0,
  SETTING_MIN = 1,
  SETTING_MAX = 2
};

enum AlarmType {
  OVERHEAT_FAN_ON,
  OVERCOOL_FAN_ON,
  OVERHEAT_FAN_OFF,
  OVERCOOL_FAN_OFF,
  OVERHEAT_100_DRIVER,
  OVERHEAT_100_NO_DRIVER,
  OVERHEAT_102,
  NO_TEMP_SIGNAL,
  ALARM_COUNT
};

EncButton eb(2, 3, 4);  // пины для энкодера (CLK, DT, SW)
GyverDS18 ds(5);        // пин для датчика температуры

uint8_t engTset_min = 70;  // Минимальная температура (начальное значение)
uint8_t engTset_max = 90;  // Максимальная температура (начальное значение)

bool engineOffMessageSent = false;

SettingMode settingMode = NO_SETTING;   // 0 - нет настройки, 1 - настройка min, 2 - настройка max
unsigned long lastSettingActivity = 0;  // Время последней активности в режиме настройки

const unsigned long settingTimeout = 10000;       // timeout exit from settings
const unsigned long interval = 1000;              // Interval for temperature updates
const unsigned long alarmCooldownPeriod = 10000;  // alarm cooldown
const unsigned long CoolingUpdateTime = 30000;    // CoolingUpdateTime
const unsigned long ENGINE_OFF_DELAY = 60000;     // 60 секунд
unsigned long lastWarningTime = 0;
unsigned long lastManualResetCheck = 0;
const unsigned long MANUAL_RESET_TIMEOUT = 60000;  // 60 секунд

uint64_t addr = 0xBF3CE1E3800D4228;

bool engT_cool;             // (1 or 0) cooling engine
bool engT_cool_mon;         // check cooling really work? (use stabilitron)
int8_t engT;                // (-40...+120) current temperature
uint8_t engTthreshold = 3;  // Temperature threshold for alarms
int8_t engTmax = 0;         // Maximum temperature for current ride
int8_t engT_aver = 0;       // Average engine temperature
bool engTmodeX;             // Fan always on mode

// alarms
bool alarms[ALARM_COUNT] = { false };

unsigned long noSignalStartTime = 0;  // Время начала отсутствия сигнала

bool driver;                 // If driver is in the car
bool coolWhenEngineOff = 0;  // 0 - не охлаждать при выключенном двигателе, 1 - охлаждать всегда

bool engStatus_engT;  // this one will be sent to main board for turn engine off (0 is eng will off)
bool engStatus;       // 1 when eng works

bool manuallyReset[ALARM_COUNT] = { false };

unsigned long lastMillis = 0;  // Stores the last time the temperature was updated

int tempCount = 0;  // Counter for temperature readings

unsigned long alarmResetTime[ALARM_COUNT] = { 0 };

int anyAlarmActive() {
  for (int i = 0; i < ALARM_COUNT; i++) {
    if (alarms[i]) return i;
  }
  return -1;
}

void setup() {
  Serial.begin(9600);
  Serial.println("Setup started");
  ds.requestTemp();

  eb.setClickTimeout(400);  // Установим таймаут для определения двойного клика

  // Инициализация дисплея
  disp.brightness(7);  // Установить яркость дисплея
  disp.clear();        // Очистка дисплея
}

void loop() {
  if (ds.ready()) {
    if (ds.readTemp(addr)) {
      engT = ds.getTemp();
      Serial.print("Current temperature: ");
      Serial.println(engT);
      alarms[NO_TEMP_SIGNAL] = false;
      noSignalStartTime = 0;
      lastWarningTime = 0;
    } else {
      handleSensorError();
    }
    ds.requestTemp();
  }

  eb.tick();  // обновление состояния энкодера

  // Проверка таймаута режима настройки
  if (settingMode != NO_SETTING && millis() - lastSettingActivity > settingTimeout) {
    settingMode = NO_SETTING;
    Serial.println("Setting mode timeout");
  }

  // Проверка на одно нажатие кнопки
  if (eb.click()) {
    int activeAlarm = anyAlarmActive();
    if (activeAlarm != -1) {
      resetAlarms();
    } else if (settingMode == NO_SETTING) {
      settingMode = SETTING_MIN;
      lastSettingActivity = millis();
      Serial.println("Switched to setting mode for engTset_min");
    }
  }

  // Проверка на двойное нажатие кнопки
  if (eb.hasClicks(2)) {
    if (!anyAlarmActive()) {
      settingMode = SETTING_MAX;  // Переключаемся в режим настройки engTset_max
      lastSettingActivity = millis();
      Serial.println("Switched to setting mode for engTset_max");
    }
  }

  // Проверка на удержание кнопки
  if (settingMode != NO_SETTING && eb.hold()) {
    settingMode = NO_SETTING;  // Выход из режима настройки
    Serial.println("Exited setting mode due to button hold");
  }

  // Проверка на вращение энкодера в режиме настройки
  if (settingMode != NO_SETTING) {
    if (eb.turn()) {
      lastSettingActivity = millis();
      int8_t change = -eb.dir() * 5;  // Изменен знак
      switch (settingMode) {
        case SETTING_MIN:
          engTset_min = constrain(engTset_min + change, 70, 95);
          Serial.print("Adjusted engTset_min to: ");
          Serial.println(engTset_min);
          break;
        case SETTING_MAX:
          engTset_max = constrain(engTset_max + change, 75, 100);
          Serial.print("Adjusted engTset_max to: ");
          Serial.println(engTset_max);
          break;
      }
    }
  }

  updateCooling();
  checkAndSetAlarms();

  static unsigned long lastManualResetCheck = 0;
  if (millis() - lastManualResetCheck > 60000) {  // Проверяем каждую минуту
    for (int i = 0; i < ALARM_COUNT; i++) {
      manuallyReset[i] = false;
    }
    lastManualResetCheck = millis();
  }

  checkManualReset();

  // Обновление дисплея в зависимости от состояния
  updateDisplay();
}

// Обновление дисплея
void updateDisplay() {
  disp.clear();  // Очистка дисплея перед обновлением

  if (alarms[NO_TEMP_SIGNAL]) {
    disp.setCursor(0);
    disp.print("Err");  // Отображение "Err" на дисплее
  } else if (alarms[OVERHEAT_FAN_ON]) {
    disp.setCursor(0);
    disp.print("OHFO");  // Отображение "OHFO" на дисплее
  } else if (alarms[OVERCOOL_FAN_ON]) {
    disp.setCursor(0);
    disp.print("OCFO");  // Отображение "OCFO" на дисплее
  } else if (alarms[OVERHEAT_FAN_OFF]) {
    disp.setCursor(0);
    disp.print("OHFF");  // Отображение "OHFF" на дисплее
  } else if (alarms[OVERCOOL_FAN_OFF]) {
    disp.setCursor(0);
    disp.print("OCFF");  // Отображение "OCFF" на дисплее
  } else if (alarms[OVERHEAT_100_DRIVER]) {
    disp.setCursor(0);
    disp.print("100D");  // Отображение "100D" на дисплее
  } else if (alarms[OVERHEAT_100_NO_DRIVER]) {
    disp.setCursor(0);
    disp.print("100N");  // Отображение "100N" на дисплее
  } else if (alarms[OVERHEAT_102]) {
    disp.setCursor(0);
    disp.print("102C");  // Отображение "102C" на дисплее
  } else if (settingMode == SETTING_MIN) {
    disp.setCursor(0);
    disp.print("StMn");  // Отображение "StMn" на дисплее
  } else if (settingMode == SETTING_MAX) {
    disp.setCursor(0);
    disp.print("StMx");  // Отображение "StMx" на дисплее
  } else {
    // Отображение температуры и состояния охлаждения
    displayTemperatureAndCooling();
  }

  disp.update();  // Обновление дисплея
}

// Обработка ошибки датчика
void handleSensorError() {
  unsigned long currentTime = millis();

  if (!alarms[NO_TEMP_SIGNAL] && !manuallyReset[NO_TEMP_SIGNAL]) {
    alarms[NO_TEMP_SIGNAL] = true;
    noSignalStartTime = currentTime;
    alarmResetTime[NO_TEMP_SIGNAL] = currentTime;
    Serial.println("NO_TEMP_SIGNAL alarm activated");
    engineOffMessageSent = false;
    lastWarningTime = 0;
  } else if (alarms[NO_TEMP_SIGNAL] && !manuallyReset[NO_TEMP_SIGNAL]) {
    if (currentTime - noSignalStartTime >= ENGINE_OFF_DELAY) {
      if (!engineOffMessageSent) {
        engStatus_engT = 0;
        Serial.println("Engine status set to OFF due to NO_TEMP_SIGNAL");
        engineOffMessageSent = true;
      }
    } else if (currentTime - noSignalStartTime >= 10000) {
      if (currentTime - lastWarningTime >= 5000) {
        Serial.println("Warning: Engine will be turned off in " + String((ENGINE_OFF_DELAY - (currentTime - noSignalStartTime)) / 1000) + " seconds if NO_TEMP_SIGNAL persists");
        lastWarningTime = currentTime;
      }
    }
  }

  ds.requestTemp();
}

// Проверка и установка тревог
void checkAndSetAlarms() {
  unsigned long currentTime = millis();

  // Проверка подключения датчика
  bool sensorConnected = ds.readTemp(addr);
  if (!sensorConnected) {
    handleSensorError();
  } else {
    engT = ds.getTemp();
    alarms[NO_TEMP_SIGNAL] = false;
    manuallyReset[NO_TEMP_SIGNAL] = false;  // Сбрасываем флаг ручного сброса
    noSignalStartTime = 0;
  }

  // Функция для проверки и установки аларма
  auto setAlarm = [&](AlarmType type, bool condition, unsigned long& resetTime) {
    if (condition) {
      if (!alarms[type] && (currentTime - resetTime > alarmCooldownPeriod) && !manuallyReset[type]) {
        alarms[type] = true;
        resetTime = currentTime;
        Serial.print("Alarm ");
        Serial.print(type);
        Serial.println(" activated");
      }
    } else {
      alarms[type] = false;
      manuallyReset[type] = false;  // Сбрасываем флаг ручного сброса, если условие больше не истинно
    }
  };

  // Проверка всех алармов
  setAlarm(OVERHEAT_FAN_ON, engT > engTset_max + engTthreshold && engT_cool_mon, alarmResetTime[OVERHEAT_FAN_ON]);
  setAlarm(OVERCOOL_FAN_ON, engT < engTset_min - engTthreshold && engT_cool_mon, alarmResetTime[OVERCOOL_FAN_ON]);
  setAlarm(OVERHEAT_FAN_OFF, engT > engTset_max + engTthreshold && !engT_cool_mon, alarmResetTime[OVERHEAT_FAN_OFF]);
  setAlarm(OVERCOOL_FAN_OFF, engT < engTset_min - engTthreshold && !engT_cool_mon && engStatus, alarmResetTime[OVERCOOL_FAN_OFF]);
  setAlarm(OVERHEAT_100_DRIVER, engT > 100 && driver, alarmResetTime[OVERHEAT_100_DRIVER]);
  setAlarm(OVERHEAT_100_NO_DRIVER, engT > 100 && !driver, alarmResetTime[OVERHEAT_100_NO_DRIVER]);
  setAlarm(OVERHEAT_102, engT > 102, alarmResetTime[OVERHEAT_102]);

  // Дополнительная логика для некоторых алармов
  if (alarms[OVERHEAT_100_NO_DRIVER] || alarms[OVERHEAT_102]) {
    engStatus_engT = 0;
  }
}

unsigned long lastCoolingUpdate = 0;  // Время последнего обновления состояния охлаждения


// Обновление состояния охлаждения
void updateCooling() {
  engT_cool = (engStatus || coolWhenEngineOff)
                // Если двигатель работает (engStatus) или разрешено охлаждение при выключенном двигателе (coolWhenEngineOff)
                ? (engT > engTset_max)   ? true       // Если температура выше максимальной, включаем охлаждение
                  : (engT < engTset_min) ? false      // Если температура ниже минимальной, выключаем охлаждение
                                         : engT_cool  // Если температура между min и max, сохраняем текущее состояние охлаждения
                : false;                              // Если двигатель выключен и не разрешено охлаждение при выключенном двигателе, охлаждение всегда выключено

  // Вывод состояния охлаждения только раз в секунду
  if (millis() - lastCoolingUpdate >= CoolingUpdateTime) {  // 30000 мс = 30 секунд
    Serial.print("Cooling status updated: ");
    Serial.println(engT_cool ? "Cooling ON" : "Cooling OFF");
    lastCoolingUpdate = millis();  // Обновляем время последнего вывода
  }
}

void checkManualReset() {
  unsigned long currentTime = millis();
  if (currentTime - lastManualResetCheck >= MANUAL_RESET_TIMEOUT) {
    for (int i = 0; i < ALARM_COUNT; i++) {
      manuallyReset[i] = false;
    }
    lastManualResetCheck = currentTime;
  }
}

// Сброс тревог
void resetAlarms() {
  unsigned long currentTime = millis();
  for (int i = 0; i < ALARM_COUNT; i++) {
    if (alarms[i]) {
      alarms[i] = false;
      alarmResetTime[i] = currentTime;
      manuallyReset[i] = true;
      Serial.print("Alarm ");
      Serial.print(i);
      Serial.println(" reset manually");
      if (i == NO_TEMP_SIGNAL) {
        engineOffMessageSent = false;
        noSignalStartTime = 0;
        lastWarningTime = 0;
      }
    }
  }
  Serial.println("All alarms have been reset");
}

void displayTemperatureAndCooling() {
  // Очистка дисплея
  disp.clear();

  // Установка курсора в начало
  disp.setCursor(0);

  // Вывод температуры (первые два знака)
  disp.print(engT);

  // Печать пробела
  disp.print(" ");

  // Вывод состояния охлаждения (последние два знака)
  disp.print(engT_cool ? "1" : "0");

  // Обновление дисплея
  disp.update();
}
