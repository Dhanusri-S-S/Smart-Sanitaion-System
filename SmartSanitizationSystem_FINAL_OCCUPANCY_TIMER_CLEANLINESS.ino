/*
 * ============================================================
 *   SMART SANITIZATION SYSTEM
 *   Platform  : Arduino UNO R3
 *   Version   : 3.0  (Door PIR Toggle Logic)
 * ============================================================
 *
 *  LOGIC:
 *  ─────────────────────────────────────────────────────────
 *  PIR at DOOR — detects person crossing the doorway
 *
 *  1st PIR trigger → OCCUPIED
 *    • LCD shows "Restroom Occupied"
 *    • Gas + Temp monitored continuously
 *    • Gas > 120 OR Temp > 36°C → spray (5s cooldown)
 *
 *  2nd PIR trigger → EXIT SPRAY → VACANT
 *    • Servo sprays mandatory exit sanitization
 *    • LCD shows "Sanitizing..."
 *    • Then LCD shows "Restroom Vacant"
 *
 *  3rd PIR trigger → OCCUPIED again (cycle repeats)
 *
 *  NOTE: PIR must go LOW for 2s between triggers to count
 *        as a new crossing event (debounce)
 *
 *  PIN MAPPING
 *  ─────────────────────────────────────────────────────────
 *  A0  → MQ135 Gas Sensor         (Analog input)
 *  A4  → LCD I2C SDA              (Hardware I2C)
 *  A5  → LCD I2C SCL              (Hardware I2C)
 *  D2  → DHT22 DATA               (+ 10kΩ pull-up to 5V)
 *  D3  → PIR Sensor HC-SR501 OUT  (Digital input)
 *  D9  → SG90 Servo Signal        (PWM output)
 * ============================================================
 */

#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>

// ── Pin Definitions ──────────────────────────────────────────
#define MQ135_PIN      A0
#define DHT_PIN        2
#define PIR_PIN        3
#define SERVO_PIN      9

// ── Sensor Setup ─────────────────────────────────────────────
#define DHT_TYPE       DHT22
DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);  // change to 0x3F if needed
Servo sanitizerServo;

// ── Thresholds ────────────────────────────────────────────────
const int   GAS_THRESHOLD      = 120;  // normal room ~50-60, triggers above 120
const float TEMP_THRESHOLD     = 36.0; // normal room ~32°C, triggers above 36
const int   SERVO_DISPENSE_DEG = 180;
const int   SERVO_REST_DEG     = 0;
const unsigned long DISPENSE_MS       = 1000; // servo hold time ms
const unsigned long SPRAY_COOLDOWN_MS = 5000; // min gap between gas sprays
const unsigned long SENSOR_INTERVAL   = 2000; // DHT22 + MQ135 read interval
const unsigned long PIR_DEBOUNCE_MS   = 2000; // PIR must be LOW for 2s before
                                               // next trigger counts as new event

// ── State Machine ─────────────────────────────────────────────
enum RoomState { VACANT, OCCUPIED };
RoomState roomState = VACANT;

// ── PIR Edge Detection ────────────────────────────────────────
bool          pirLastState = LOW;
bool          pirArmed     = true;
unsigned long pirWentLow   = 0;

// ── Timing ────────────────────────────────────────────────────
unsigned long lastSensorRead = 0;
unsigned long lastSprayTime  = 0;
unsigned long dispenseStart  = 0;
bool          dispensing     = false;

// ── Sensor Values ─────────────────────────────────────────────
float temperature = 0.0;
float humidity    = 0.0;
int   gasLevel    = 0;

// ─────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────

// ===== EXTRA FEATURES =====
#define RED_LED_PIN 5
#define GREEN_LED_PIN 6
#define BUZZER_PIN 8

bool cleaningTimerStarted = false;
unsigned long lastCleaningReminder = 0;
const unsigned long CLEAN_INTERVAL_MS = 600000UL;

unsigned long occupiedAccumulatedTime = 0;
unsigned long lastOccupiedTick = 0;

int calculateCleanliness() {
  int gasScore  = constrain(map(gasLevel, 0, 150, 100, 0), 0, 100);
  int tempScore = constrain(map((int)(temperature * 10), 250, 450, 100, 0), 0, 100);
  return (gasScore * 70 + tempScore * 30) / 100;
}


const int   SEVERE_GAS_THRESHOLD  = 150;
const float SEVERE_TEMP_THRESHOLD = 45.0;

void severeCleaningAlert() {
  recoverLCD();
  lcd.setCursor(0,0);
  lcd.print("CLEAN REQUIRED");
  lcd.setCursor(0,1);
  lcd.print("TEMP/GAS HIGH ");

  for(int i=0;i<10;i++){
    digitalWrite(RED_LED_PIN,HIGH);
    tone(BUZZER_PIN,2000);
    delay(300);
    digitalWrite(RED_LED_PIN,LOW);
    noTone(BUZZER_PIN);
    delay(300);
  }
}


void blinkRedAndBuzz() {
  for(int i=0;i<10;i++){
    digitalWrite(RED_LED_PIN,HIGH);
    tone(BUZZER_PIN,2000);
    delay(250);
    noTone(BUZZER_PIN);
    digitalWrite(RED_LED_PIN,LOW);
    delay(250);
  }
}
void setup() {
  Serial.begin(9600);
  Serial.println(F("=== Smart Sanitization System v3.0 ==="));
  Serial.println(F("Door PIR Toggle Mode"));
  Serial.println(F("───────────────────────────────────────"));

  pinMode(MQ135_PIN, INPUT);
  pinMode(PIR_PIN,   INPUT);

  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, HIGH);

  dht.begin();
  delay(2000);

  lcd.init();
  lcd.backlight();
  lcd.clear();

  // Splash screen
  lcd.setCursor(0, 0);
  lcd.print(F("Smart Sanit.Sys"));
  lcd.setCursor(0, 1);
  lcd.print(F(" Initializing  "));
  delay(2000);

  // Servo to rest position
  sanitizerServo.attach(SERVO_PIN);
  sanitizerServo.write(SERVO_REST_DEG);
  delay(500);

  // MQ135 warm-up
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("MQ135 Warming Up"));
  lcd.setCursor(0, 1);
  lcd.print(F("Please wait...  "));
  Serial.println(F("MQ135 warming up..."));
  delay(5000);

  // Start in VACANT state
  showVacant();
  Serial.println(F("System ready — State: VACANT"));
  Serial.println(F("Waiting for door PIR trigger..."));
}

// ─────────────────────────────────────────────────────────────
//  MAIN LOOP
// ─────────────────────────────────────────────────────────────
void loop() {

  // ===== OCCUPANCY LED LOGIC =====
  if (roomState == OCCUPIED)
  {
    digitalWrite(RED_LED_PIN, HIGH);
    digitalWrite(GREEN_LED_PIN, LOW);

    if (!cleaningTimerStarted)
    {
      cleaningTimerStarted = true;
      lastCleaningReminder = millis();
    }
  }
  else
  {
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(GREEN_LED_PIN, HIGH);
  }

  // ===== Cleaning Reminder =====
  if (cleaningTimerStarted &&
      millis() - lastCleaningReminder >= CLEAN_INTERVAL_MS) {

      lastCleaningReminder = millis();

      lcd.clear();
      lcd.setCursor(0,0);
      lcd.print("CLEANING REQ");

      lcd.setCursor(0,1);
      lcd.print("PLEASE CLEAN");

      blinkRedAndBuzz();
  }


  unsigned long now = millis();

  if (roomState == OCCUPIED) {
    if (lastOccupiedTick == 0) lastOccupiedTick = now;
    occupiedAccumulatedTime += (now - lastOccupiedTick);
    lastOccupiedTick = now;
  } else {
    lastOccupiedTick = now;
  }

  if (occupiedAccumulatedTime >= CLEAN_INTERVAL_MS) {
    occupiedAccumulatedTime = 0;

    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("CLEANING REQ");

    lcd.setCursor(0,1);
    lcd.print("Please Clean");

    blinkRedAndBuzz();
  }


  // Non-blocking servo return after dispense
  if (dispensing && (now - dispenseStart >= DISPENSE_MS)) {
    sanitizerServo.write(SERVO_REST_DEG);
    dispensing = false;
    Serial.println(F("Servo returned to rest."));
  }

  bool pirNow = (digitalRead(PIR_PIN) == HIGH);

  // ── PIR EDGE DETECTION ────────────────────────────────────
  // Track when PIR goes LOW
  if (!pirNow && pirLastState == HIGH) {
    pirWentLow = now;
  }

  // Re-arm after PIR has been LOW for debounce duration
  if (!pirNow && !pirArmed && (now - pirWentLow >= PIR_DEBOUNCE_MS)) {
    pirArmed = true;
    Serial.println(F("PIR armed — ready for next crossing"));
  }

  // Valid crossing = rising edge + armed
  if (pirNow && !pirLastState && pirArmed) {
    pirArmed = false;

    if (roomState == VACANT) {
      // ── 1st TRIGGER: ENTRY ──
      roomState = OCCUPIED;
      showOccupied();
      Serial.println(F(">>> Door trigger — ENTRY detected"));
      Serial.println(F("State: OCCUPIED"));

    } else {
      // ── 2nd TRIGGER: EXIT ──
      Serial.println(F(">>> Door trigger — EXIT detected"));
      Serial.println(F("Performing mandatory exit spray..."));

      recoverLCD();
      lcd.setCursor(0, 0);
      lcd.print(F("  Sanitizing... "));
      lcd.setCursor(0, 1);
      lcd.print(F("  Please wait   "));

      // Blocking exit spray
      sanitizerServo.write(SERVO_DISPENSE_DEG);
      delay(DISPENSE_MS);
      sanitizerServo.write(SERVO_REST_DEG);
      delay(300);

      roomState = VACANT;
      showVacant();
      Serial.println(F("Exit spray done — State: VACANT"));
    }
  }

  pirLastState = pirNow;

  // ── SENSOR MONITORING (only while OCCUPIED) ──────────────
  if (roomState == OCCUPIED) {
    if (now - lastSensorRead >= SENSOR_INTERVAL) {
      lastSensorRead = now;
      readSensors();
      logToSerial();
      updateLCDOccupied();

      bool gasBad  = (gasLevel    > GAS_THRESHOLD);
      bool tempBad = (temperature > TEMP_THRESHOLD);


      bool severeGas = (gasLevel > SEVERE_GAS_THRESHOLD);
      bool severeTemp = (temperature > SEVERE_TEMP_THRESHOLD);

      if (severeGas || severeTemp){
        severeCleaningAlert();
      }

      if ((gasBad || tempBad) && !dispensing &&
          !(severeGas || severeTemp) &&
          (now - lastSprayTime >= SPRAY_COOLDOWN_MS)) {
        Serial.print(F(">>> Air quality spray — Gas:"));
        Serial.print(gasLevel);
        Serial.print(F("  Temp:"));
        Serial.println(temperature, 1);
        triggerSpray();
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────
//  TRIGGER GAS/TEMP SPRAY (non-blocking)
// ─────────────────────────────────────────────────────────────
void triggerSpray() {
  dispensing    = true;
  dispenseStart = millis();
  lastSprayTime = millis();
  sanitizerServo.write(SERVO_DISPENSE_DEG);
}

// ─────────────────────────────────────────────────────────────
//  READ SENSORS
// ─────────────────────────────────────────────────────────────
void readSensors() {
  float newTemp = dht.readTemperature();
  float newHum  = dht.readHumidity();

  if (!isnan(newTemp) && !isnan(newHum)) {
    temperature = newTemp;
    humidity    = newHum;
  } else {
    Serial.println(F("WARNING: DHT22 read failed."));
  }

  gasLevel = analogRead(MQ135_PIN);
}

// ─────────────────────────────────────────────────────────────
//  LCD RECOVERY — re-inits LCD to clear any garbage from
//  loose connections or I2C noise
// ─────────────────────────────────────────────────────────────
void recoverLCD() {
  lcd.init();
  lcd.backlight();
  lcd.clear();
}

// ─────────────────────────────────────────────────────────────
//  LCD SCREENS
// ─────────────────────────────────────────────────────────────
void showVacant() {
  recoverLCD();
  lcd.setCursor(0, 0);
  lcd.print(F("Restroom        "));
  lcd.setCursor(0, 1);
  lcd.print(F("   VACANT       "));
}

void showOccupied() {
  recoverLCD();
  lcd.setCursor(0, 0);
  lcd.print(F("Restroom        "));
  lcd.setCursor(0, 1);
  lcd.print(F("  OCCUPIED      "));
}


void updateLCDOccupied() {
  if (dispensing) return;

  int cleanliness = calculateCleanliness();

  unsigned long remaining =
      (occupiedAccumulatedTime >= CLEAN_INTERVAL_MS)
      ? 0
      : (CLEAN_INTERVAL_MS - occupiedAccumulatedTime);

  unsigned long totalSec = remaining / 1000;
  int mins = totalSec / 60;
  int secs = totalSec % 60;

  recoverLCD();

  lcd.setCursor(0,0);
  lcd.print("C:");
  lcd.print(cleanliness);
  lcd.print("% ");

  if(mins < 10) lcd.print("0");
  lcd.print(mins);
  lcd.print(":");
  if(secs < 10) lcd.print("0");
  lcd.print(secs);

  lcd.setCursor(0,1);
  lcd.print("G:");
  lcd.print(gasLevel);
  lcd.print(" T:");
  lcd.print(temperature,1);
}


// ─────────────────────────────────────────────────────────────
//  SERIAL LOGGING
// ─────────────────────────────────────────────────────────────
void logToSerial() {
  Serial.print(F("T:"));
  Serial.print(temperature, 1);
  Serial.print(F("C  H:"));
  Serial.print(humidity, 1);
  Serial.print(F("%  Gas:"));
  Serial.print(gasLevel);
  Serial.print(F("  Threshold:"));
  Serial.print(GAS_THRESHOLD);
  Serial.print(F("  State:"));
  Serial.println(roomState == VACANT ? F("VACANT") : F("OCCUPIED"));
}
