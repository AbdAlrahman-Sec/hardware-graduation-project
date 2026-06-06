

//  PINS setups
#define STEP1 22
#define DIR1  23
#define STEP2 24
#define DIR2  25
#define STEP3 30
#define DIR3  31

#define TRIG1 28
#define ECHO1 29
#define TRIG2 26
#define ECHO2 27
#define TRIG3 40
#define ECHO3 41

#define TRIG_FULL1 42
#define ECHO_FULL1 43
#define TRIG_FULL2 44
#define ECHO_FULL2 45
#define TRIG_FULL3 46
#define ECHO_FULL3 47

#define ENA 5
#define IN1 6
#define IN2 7

#define LIMIT1 34
#define LIMIT2 35
#define LIMIT3 36
#define LIMIT4 37
#define LIMIT5 39
#define LIMIT6 38

// constants for steppers and ultrasonics
#define STEP_DELAY_US   6000
#define MAX_STEPS       20000
#define OPEN_WAIT_MS    8000
#define SCAN_TIMEOUT_MS 30000  // 30 s — gives the API more room if it's slow
#define NEAR_CM         20
#define CLEAR_CM        25
#define FULL_CM         17

#define MIN_CLEAN_MS    15000
#define CLEAR_HOLD_MS   5000
#define MAX_CLEAN_MS    60000

//  stats
int  targetStation   = 0;
bool itemInSystem    = false;
bool readyForNewItem = true;
bool manualMode      = false;
String cmdBuf        = "";

// 
void setup() {
  Serial.begin(9600);

  pinMode(STEP1, OUTPUT); pinMode(DIR1, OUTPUT);
  pinMode(STEP2, OUTPUT); pinMode(DIR2, OUTPUT);
  pinMode(STEP3, OUTPUT); pinMode(DIR3, OUTPUT);

  pinMode(TRIG1, OUTPUT); pinMode(ECHO1, INPUT);
  pinMode(TRIG2, OUTPUT); pinMode(ECHO2, INPUT);
  pinMode(TRIG3, OUTPUT); pinMode(ECHO3, INPUT);

  pinMode(TRIG_FULL1, OUTPUT); pinMode(ECHO_FULL1, INPUT);
  pinMode(TRIG_FULL2, OUTPUT); pinMode(ECHO_FULL2, INPUT);
  pinMode(TRIG_FULL3, OUTPUT); pinMode(ECHO_FULL3, INPUT);

  pinMode(LIMIT1, INPUT_PULLUP); pinMode(LIMIT2, INPUT_PULLUP);
  pinMode(LIMIT3, INPUT_PULLUP); pinMode(LIMIT4, INPUT_PULLUP);
  pinMode(LIMIT5, INPUT_PULLUP); pinMode(LIMIT6, INPUT_PULLUP);

  pinMode(ENA, OUTPUT); pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);

  moveStepperUntilLimit(STEP1, DIR1, HIGH, LIMIT2);
  moveStepperUntilLimit(STEP2, DIR2, HIGH, LIMIT4);
  // moveStepperUntilLimit(STEP3, DIR3, HIGH, LIMIT6);  // enable when Station 3 is wired

  cleanBelt();
  startDC();
  Serial.println("STATUS:WORKING");
}

// 
void loop() {

  checkSerialCommands();
  if (manualMode) return;

  float d1 = readDistance(TRIG1, ECHO1);

  // ----- (A) waiting for a new item -----
  if (!itemInSystem) {

    if (readyForNewItem && isNear(d1)) {
      readyForNewItem = false;
      stopDC();

      String type = requestScan();

      // DEBUG: print exactly what the Pi sent back so you can see in the terminal
      Serial.print("SCAN_RESULT:[");
      Serial.print(type);
      Serial.println("]");

      targetStation = stationForType(type);

      Serial.print("CLASSIFIED:");
      Serial.print(type);
      Serial.print(" -> station ");
      Serial.println(targetStation);

      itemInSystem = true;

      if (targetStation == 1) {
        pushAtStation(1);
        itemInSystem = false;
      } else {
        startDC();
      }
    }
    else if (isFar(d1)) {
      readyForNewItem = true;
    }
  }

  // ----- (B) item is travelling to its station -----
  else {
    if (targetStation == 2) {
      float d2 = readDistance(TRIG2, ECHO2);

      static unsigned long lastDbg = 0;
      if (millis() - lastDbg > 500) {
        lastDbg = millis();
        Serial.print("ROUTING to station 2, d2 = ");
        Serial.println(d2);
      }

      if (isNear(d2)) {
        pushAtStation(2);
        itemInSystem = false;
      }
    }
    else {
      if (isFar(d1)) {
        itemInSystem = false;
      }
    }
  }
}

//  control the belt from the website 
void checkSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') { handleCommand(cmdBuf); cmdBuf = ""; }
    else if (c != '\r') cmdBuf += c;
  }
}

void handleCommand(String c) {
  c.trim();
  if (c == "CMD:BELT_ON") {
    manualMode = true; startDC();
    Serial.println("BELT MANUAL ON");
  } else if (c == "CMD:BELT_OFF") {
    manualMode = true; stopDC();
    Serial.println("BELT MANUAL OFF");
  } else if (c == "CMD:AUTO") {
    manualMode = false;
    itemInSystem = false;
    readyForNewItem = true;
    startDC();
    Serial.println("STATUS:WORKING");
  }
}

// belt cleaning 
void cleanBelt() {
  Serial.println("CLEANING BELT...");
  startDC();

  unsigned long startTime = millis();
  unsigned long lastSeen  = millis();

  while (true) {
    bool something = isNear(readDistance(TRIG1, ECHO1)) ||
                     isNear(readDistance(TRIG2, ECHO2));
    if (something) lastSeen = millis();

    unsigned long runFor      = millis() - startTime;
    bool          emptyEnough = (millis() - lastSeen > CLEAR_HOLD_MS);

    if (runFor > MIN_CLEAN_MS && emptyEnough) break;
    if (runFor > MAX_CLEAN_MS) {
      Serial.println("CLEANING timeout - possible jam, check the belt");
      break;
    }
  }

  stopDC();
  Serial.println("BELT CLEAR");
}

// scan request from first ultrasonic to raspberry 
String requestScan() {
  while (Serial.available()) Serial.read();   // flush stale bytes

  Serial.println("SCAN");

  unsigned long start = millis();
  String resp = "";
  while (millis() - start < SCAN_TIMEOUT_MS) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\n') break;
      if (c != '\r') resp += c;
    }
  }
  resp.trim();
  return resp;                                // "" if Pi never answered
}

// decide which container to move based on raspberry response 
int stationForType(String t) {
  t.toUpperCase();
  if (t.indexOf("PET")  >= 0) return 1;
  if (t.indexOf("HDPE") >= 0) return 2;
  // if (t.indexOf("PP") >= 0) return 3;   // enable when Station 3 is wired
  return 0;
}

// moving the stepper motors to grab the plastic
void pushAtStation(int s) {
  int stepPin, dirPin, limExtend, limHome, trigFull, echoFull;

  switch (s) {
    case 1: stepPin=STEP1; dirPin=DIR1; limExtend=LIMIT1; limHome=LIMIT2;
            trigFull=TRIG_FULL1; echoFull=ECHO_FULL1; break;
    case 2: stepPin=STEP2; dirPin=DIR2; limExtend=LIMIT3; limHome=LIMIT4;
            trigFull=TRIG_FULL2; echoFull=ECHO_FULL2; break;
    // case 3: stepPin=STEP3; dirPin=DIR3; limExtend=LIMIT5; limHome=LIMIT6;
    //         trigFull=TRIG_FULL3; echoFull=ECHO_FULL3; break;
    default: return;
  }

  stopDC();
  moveStepperUntilLimit(stepPin, dirPin, LOW, limExtend);   // OPEN

  startDC();
  delay(OPEN_WAIT_MS);                                       // feed 8 s
  stopDC();

  moveStepperUntilLimit(stepPin, dirPin, HIGH, limHome);    // CLOSE / drag

  float f = readDistance(trigFull, echoFull);
  Serial.print("BIN"); Serial.print(s);
  Serial.println(isFull(f) ? " FULL" : " OK");

  Serial.print("SORTED:"); Serial.println(s);

  startDC();
}

// limit switch control
void moveStepperUntilLimit(int stepPin, int dirPin, bool dir, int limitPin) {
  digitalWrite(dirPin, dir);
  long guard = MAX_STEPS;
  while (digitalRead(limitPin) == HIGH && guard-- > 0) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(STEP_DELAY_US);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(STEP_DELAY_US);
  }
}

// configure containers ultrasonics to check if containers are full or not 
float readDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH); delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH, 30000);
  return duration * 0.034 / 2.0;
}

bool isNear(float d) { return (d > 0 && d < NEAR_CM); }
bool isFar (float d) { return (d > CLEAR_CM); }
bool isFull(float d) { return (d > 0 && d < FULL_CM); }

// ===================== DC CONVEYOR =====================
void startDC() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  analogWrite(ENA, 255);
}

void stopDC() {
  analogWrite(ENA, 0);
}