/*
  =====================================================================
  VINTAGE LED CALCULATOR - BOARD TEST
  ATmega328P-AU + TM1640 + 12-digit HP LED display + 6x4 key matrix
  Arduino IDE, Board: "Arduino Uno" (16 MHz crystal, Uno bootloader)
  =====================================================================

  Pin map (taken from the schematic "TM1640 with 9V Batt"):

    TM1640 SCLK (pin 8)  <- CLK  = PB0 = D8
    TM1640 DIN  (pin 7)  <- DIN  = PB1 = D9

    Key matrix rows    R1..R6 = PD2..PD7 = D2..D7   (driven LOW one at a time)
    Key matrix columns C1..C4 = PC0..PC3 = A0..A3   (INPUT_PULLUP, read)
    BATTSENSE (ADC6, 220k/100k divider after D1) -> analogRead(6)

    TM1640 segment outputs:  SEG1=A SEG2=B SEG3=C SEG4=D SEG5=E SEG6=F
                             SEG7=G SEG8=DP   (so bit0=A ... bit6=G, bit7=DP)
    TM1640 grid outputs:     GRID1..GRID12 = display digits D1..D12

  What this sketch does
    1. Keyboard wiring self-check (shorts to GND, row/row and col/col bridges,
       keys stuck at power-up) - reported on the Serial Monitor.
    2. Display test: all segments, segment-by-segment walk, digit-by-digit
       walk, 0-9 on every digit, brightness ramp.
    3. Key test: press any key -> row/column, SW number and label are printed
       on the Serial Monitor and "r3 c2  12-24" style info is shown on the
       LED display (r/c = row/column, then "keys tested so far - 24").
       When all 24 keys have been pressed at least once the display shows
       "PASS  ALL 24".  The BATT key shows the battery voltage.

  IMPORTANT
    - Slide switch ON (9V battery connected) when uploading and testing:
      USB only powers the CH340, not the 5V rail.
    - Serial Monitor: 115200 baud.

  Serial commands:  d = repeat display test   r = reset key coverage
                    + / - = brightness         v = battery voltage
                    ? = help
*/

#include <Arduino.h>
#include <stdio.h>      // snprintf

// ----------------------------- Pin map ------------------------------
const uint8_t PIN_TM_CLK = 8;    // PB0 -> TM1640 SCLK
const uint8_t PIN_TM_DIN = 9;    // PB1 -> TM1640 DIN

const uint8_t NUM_ROWS = 6;
const uint8_t NUM_COLS = 4;
const uint8_t ROW_PINS[NUM_ROWS] = {2, 3, 4, 5, 6, 7};     // R1..R6
const uint8_t COL_PINS[NUM_COLS] = {A0, A1, A2, A3};       // C1..C4

const uint8_t ADC_BATT_CHANNEL = 6;       // ADC6 (BATTSENSE)
const float   BATT_DIV_TOP     = 220.0;   // R17 kOhm
const float   BATT_DIV_BOTTOM  = 100.0;   // R18 kOhm
const float   ADC_VREF         = 5.0;     // AVCC = +5V rail
const float   DIODE_DROP       = 0.35;    // D1 (1N5819) drop added back

const uint8_t NUM_DIGITS = 12;
// Set to true if digit D1 turns out to be the RIGHT-most digit on your
// display. Only affects text (key info, HELLO...), not the hardware tests.
const bool REVERSE_DIGITS = false;

// --------------------------- Segment bits ---------------------------
const uint8_t SEG_A  = 0x01;
const uint8_t SEG_B  = 0x02;
const uint8_t SEG_C  = 0x04;
const uint8_t SEG_D  = 0x08;
const uint8_t SEG_E  = 0x10;
const uint8_t SEG_F  = 0x20;
const uint8_t SEG_G  = 0x40;
const uint8_t SEG_DP = 0x80;

const char* const SEG_NAMES[8] = {"A", "B", "C", "D", "E", "F", "G", "DP"};

// Key labels as drawn on the schematic [row][col]
const char* const KEY_LABELS[NUM_ROWS][NUM_COLS] = {
  {"1/x", "X2",  "SQRT", "C"},
  {"CD",  "+-",  "EE",   "/"},
  {"7",   "8",   "9",    "X"},
  {"4",   "5",   "6",    "-"},
  {"1",   "2",   "3",    "+"},
  {"0",   ".",   "=",    "BATT"}
};
const uint8_t BATT_KEY_ROW = 5;
const uint8_t BATT_KEY_COL = 3;

// ------------------------------ State -------------------------------
uint8_t  dispBuf[16];            // one byte per TM1640 grid (D1..D16)
uint8_t  brightness = 7;         // 0..7 (7 = brightest, chosen after brightness test)
uint32_t stableMask = 0;         // debounced keys currently held
uint32_t testedMask = 0;         // keys pressed at least once

// ------------------------------ Font --------------------------------
uint8_t fontFor(char c) {
  switch (c) {
    case '0': case 'O': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F;
    case '1': return SEG_B | SEG_C;
    case '2': return SEG_A | SEG_B | SEG_D | SEG_E | SEG_G;
    case '3': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_G;
    case '4': return SEG_B | SEG_C | SEG_F | SEG_G;
    case '5': case 'S': case 's': return SEG_A | SEG_C | SEG_D | SEG_F | SEG_G;
    case '6': return SEG_A | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G;
    case '7': return SEG_A | SEG_B | SEG_C;
    case '8': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G;
    case '9': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G;
    case 'A': case 'a': return SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G;
    case 'b': return SEG_C | SEG_D | SEG_E | SEG_F | SEG_G;
    case 'C': return SEG_A | SEG_D | SEG_E | SEG_F;
    case 'c': return SEG_D | SEG_E | SEG_G;
    case 'd': return SEG_B | SEG_C | SEG_D | SEG_E | SEG_G;
    case 'E': case 'e': return SEG_A | SEG_D | SEG_E | SEG_F | SEG_G;
    case 'F': case 'f': return SEG_A | SEG_E | SEG_F | SEG_G;
    case 'G': return SEG_A | SEG_C | SEG_D | SEG_E | SEG_F;
    case 'H': return SEG_B | SEG_C | SEG_E | SEG_F | SEG_G;
    case 'h': return SEG_C | SEG_E | SEG_F | SEG_G;
    case 'L': case 'l': return SEG_D | SEG_E | SEG_F;
    case 'n': return SEG_C | SEG_E | SEG_G;
    case 'o': return SEG_C | SEG_D | SEG_E | SEG_G;
    case 'P': case 'p': return SEG_A | SEG_B | SEG_E | SEG_F | SEG_G;
    case 'r': return SEG_E | SEG_G;
    case 't': return SEG_D | SEG_E | SEG_F | SEG_G;
    case 'U': case 'u': return SEG_B | SEG_C | SEG_D | SEG_E | SEG_F;
    case 'y': return SEG_B | SEG_C | SEG_D | SEG_F | SEG_G;
    case '-': return SEG_G;
    case '_': return SEG_D;
    case '=': return SEG_D | SEG_G;
    default:  return 0;               // space and anything unknown
  }
}

// ----------------------- TM1640 bit-banged driver -------------------
// Two-wire, LSB first, no ACK. Start = DIN falling while CLK high,
// stop = DIN rising while CLK high.
void tmStart() {
  digitalWrite(PIN_TM_DIN, HIGH);
  digitalWrite(PIN_TM_CLK, HIGH);
  digitalWrite(PIN_TM_DIN, LOW);
  digitalWrite(PIN_TM_CLK, LOW);
}

void tmStop() {
  digitalWrite(PIN_TM_CLK, LOW);
  digitalWrite(PIN_TM_DIN, LOW);
  digitalWrite(PIN_TM_CLK, HIGH);
  digitalWrite(PIN_TM_DIN, HIGH);
}

void tmSendByte(uint8_t b) {
  for (uint8_t i = 0; i < 8; i++) {
    digitalWrite(PIN_TM_CLK, LOW);
    digitalWrite(PIN_TM_DIN, (b & 1) ? HIGH : LOW);
    digitalWrite(PIN_TM_CLK, HIGH);
    b >>= 1;
  }
}

void tmCommand(uint8_t cmd) {
  tmStart();
  tmSendByte(cmd);
  tmStop();
}

void tmUpdate() {
  tmCommand(0x40);                 // data command: auto address increment
  tmStart();
  tmSendByte(0xC0);                // start at address 0 (GRID1 / D1)
  for (uint8_t i = 0; i < 16; i++) tmSendByte(dispBuf[i]);
  tmStop();
  tmCommand(0x88 | (brightness & 0x07));   // display ON + brightness
}

// --------------------------- Display helpers ------------------------
uint8_t gridForPos(uint8_t pos) {
  return REVERSE_DIGITS ? (NUM_DIGITS - 1 - pos) : pos;
}

void clearBuf() {
  for (uint8_t i = 0; i < 16; i++) dispBuf[i] = 0;
}

void setAll(uint8_t pattern) {
  clearBuf();
  for (uint8_t g = 0; g < NUM_DIGITS; g++) dispBuf[g] = pattern;
}

// Left-to-right text; a '.' lights the decimal point of the previous digit.
void showText(const char* s) {
  clearBuf();
  int8_t pos = -1;
  for (; *s; s++) {
    if (*s == '.') {
      if (pos >= 0 && pos < NUM_DIGITS) dispBuf[gridForPos(pos)] |= SEG_DP;
      continue;
    }
    pos++;
    if (pos >= NUM_DIGITS) break;
    dispBuf[gridForPos(pos)] = fontFor(*s);
  }
  tmUpdate();
}

// ----------------------------- Display test -------------------------
void displayTest() {
  Serial.println(F("\n=== DISPLAY TEST ==="));

  Serial.println(F("1) All segments + decimal points ON (2 s)."));
  Serial.println(F("   Look for any digit/segment that is dark or dim."));
  setAll(0xFF);
  tmUpdate();
  delay(2000);

  Serial.println(F("2) Segment walk: one segment at a time on ALL digits."));
  for (uint8_t s = 0; s < 8; s++) {
    Serial.print(F("   segment "));
    Serial.print(SEG_NAMES[s]);
    Serial.print(F("  (TM1640 SEG"));
    Serial.print(s + 1);
    Serial.println(F(")"));
    setAll(1 << s);
    tmUpdate();
    delay(800);
  }

  Serial.println(F("3) Digit walk: '8.' on one digit at a time."));
  for (uint8_t g = 0; g < NUM_DIGITS; g++) {
    Serial.print(F("   digit D"));
    Serial.print(g + 1);
    Serial.print(F("  (TM1640 GRID"));
    Serial.print(g + 1);
    Serial.println(F(")"));
    clearBuf();
    dispBuf[g] = 0xFF;
    tmUpdate();
    delay(450);
  }

  Serial.println(F("4) 0..9 on every digit."));
  for (char ch = '0'; ch <= '9'; ch++) {
    setAll(fontFor(ch));
    tmUpdate();
    delay(450);
  }

  Serial.println(F("5) Brightness ramp 0..7 (all segments on)."));
  uint8_t keep = brightness;
  setAll(0xFF);
  for (uint8_t b = 0; b < 8; b++) {
    brightness = b;
    tmUpdate();
    delay(400);
  }
  brightness = keep;

  Serial.println(F("6) Text check: HELLO then 1234567890.12"));
  showText("HELLO");
  delay(1200);
  showText("1234567890.12");
  delay(1500);

  Serial.println(F("=== DISPLAY TEST DONE ===\n"));
}

// ------------------------------ Keyboard ----------------------------
// Returns a 24-bit mask; bit index = row * 4 + col.
uint32_t scanMatrix() {
  uint32_t mask = 0;
  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    digitalWrite(ROW_PINS[r], LOW);
    pinMode(ROW_PINS[r], OUTPUT);
    delayMicroseconds(10);
    for (uint8_t c = 0; c < NUM_COLS; c++) {
      if (digitalRead(COL_PINS[c]) == LOW) mask |= (1UL << (r * NUM_COLS + c));
    }
    pinMode(ROW_PINS[r], INPUT);          // back to high-Z
  }
  return mask;
}

// Checks a group of lines (all rows or all columns): stuck to GND, and
// shorted to each other (drive one low, the others must stay high).
bool checkLineGroup(const uint8_t* pins, uint8_t n, char tag) {
  bool ok = true;
  for (uint8_t i = 0; i < n; i++) pinMode(pins[i], INPUT_PULLUP);
  delay(2);

  for (uint8_t i = 0; i < n; i++) {
    if (digitalRead(pins[i]) == LOW) {
      Serial.print(F("  PROBLEM: "));
      Serial.print(tag);
      Serial.print(i + 1);
      Serial.println(F(" reads LOW with pull-up -> shorted to GND?"));
      ok = false;
    }
  }

  for (uint8_t d = 0; d < n; d++) {
    pinMode(pins[d], OUTPUT);
    digitalWrite(pins[d], LOW);
    delayMicroseconds(50);
    for (uint8_t o = 0; o < n; o++) {
      if (o == d) continue;
      if (digitalRead(pins[o]) == LOW) {
        Serial.print(F("  PROBLEM: "));
        Serial.print(tag);
        Serial.print(d + 1);
        Serial.print(F(" pulls "));
        Serial.print(tag);
        Serial.print(o + 1);
        Serial.println(F(" LOW -> bridge between lines (or keys held down)"));
        ok = false;
      }
    }
    pinMode(pins[d], INPUT_PULLUP);
  }
  return ok;
}

void restoreKeyPins() {
  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    digitalWrite(ROW_PINS[r], LOW);       // PORT bit low -> plain high-Z input
    pinMode(ROW_PINS[r], INPUT);
  }
  for (uint8_t c = 0; c < NUM_COLS; c++) pinMode(COL_PINS[c], INPUT_PULLUP);
}

void keyboardSelfCheck() {
  Serial.println(F("=== KEYBOARD WIRING CHECK ==="));
  bool ok = true;
  restoreKeyPins();
  delay(2);
  ok &= checkLineGroup(ROW_PINS, NUM_ROWS, 'R');
  restoreKeyPins();
  ok &= checkLineGroup(COL_PINS, NUM_COLS, 'C');
  restoreKeyPins();
  delay(2);

  uint32_t m = scanMatrix();
  if (m) {
    ok = false;
    Serial.println(F("  PROBLEM: key(s) read as pressed at power-up (stuck/shorted):"));
    for (uint8_t i = 0; i < NUM_ROWS * NUM_COLS; i++) {
      if (m & (1UL << i)) {
        Serial.print(F("    R"));
        Serial.print(i / NUM_COLS + 1);
        Serial.print(F(" C"));
        Serial.print(i % NUM_COLS + 1);
        Serial.print(F("  label "));
        Serial.println(KEY_LABELS[i / NUM_COLS][i % NUM_COLS]);
      }
    }
  }
  Serial.println(ok ? F("  Wiring check OK.\n") : F("  Wiring check found problems (see above).\n"));
}

// ------------------------------ Battery -----------------------------
float readBatteryVolts() {
  analogRead(ADC_BATT_CHANNEL);                 // dummy read, lets the mux settle
  uint32_t sum = 0;
  for (uint8_t i = 0; i < 16; i++) sum += analogRead(ADC_BATT_CHANNEL);
  float adc = sum / 16.0;
  float vPin = adc * ADC_VREF / 1023.0;
  float vNode = vPin * (BATT_DIV_TOP + BATT_DIV_BOTTOM) / BATT_DIV_BOTTOM;
  return vNode + DIODE_DROP;
}

void showBattery() {
  float v = readBatteryVolts();
  Serial.print(F("  Battery approx. "));
  Serial.print(v, 2);
  Serial.println(F(" V (after D1 drop correction)"));
  int hundredths = (int)(v * 100.0 + 0.5);
  char buf[16];
  snprintf(buf, sizeof(buf), "b %d.%02d", hundredths / 100, hundredths % 100);
  showText(buf);
}

// ------------------------------ Key test ----------------------------
uint8_t popcount32(uint32_t v) {
  uint8_t n = 0;
  while (v) { n += v & 1; v >>= 1; }
  return n;
}

void showKeyInfo(uint8_t r, uint8_t c) {
  uint8_t tested = popcount32(testedMask);
  char buf[20];
  if (tested == NUM_ROWS * NUM_COLS) {
    snprintf(buf, sizeof(buf), "PASS  ALL 24");
  } else {
    snprintf(buf, sizeof(buf), "r%d c%d  %2d-24", r + 1, c + 1, tested);
  }
  showText(buf);
}

void onNewKeys(uint32_t newly, uint32_t held) {
  if (popcount32(held) >= 3) {
    Serial.println(F("  (3+ keys held: ghosting is possible, there are no diodes)"));
  }
  for (uint8_t i = 0; i < NUM_ROWS * NUM_COLS; i++) {
    if (!(newly & (1UL << i))) continue;
    uint8_t r = i / NUM_COLS;
    uint8_t c = i % NUM_COLS;
    bool firstTime = !(testedMask & (1UL << i));
    testedMask |= (1UL << i);

    Serial.print(F("KEY  SW"));
    Serial.print(2 + r + 6 * c);          // SW numbering from the schematic
    Serial.print(F("   R"));
    Serial.print(r + 1);
    Serial.print(F(" C"));
    Serial.print(c + 1);
    Serial.print(F("   label: "));
    Serial.print(KEY_LABELS[r][c]);
    Serial.print(F("   tested "));
    Serial.print(popcount32(testedMask));
    Serial.print(F("/24"));
    Serial.println(firstTime ? F("") : F("  (again)"));

    if (r == BATT_KEY_ROW && c == BATT_KEY_COL) showBattery();
    else showKeyInfo(r, c);

    if (popcount32(testedMask) == NUM_ROWS * NUM_COLS && firstTime) {
      Serial.println(F("*** ALL 24 KEYS TESTED OK ***"));
    }
  }
}

// ------------------------------ Serial ------------------------------
void printHelp() {
  Serial.println(F("Commands: d=display test  r=reset key coverage  +/-=brightness"));
  Serial.println(F("          v=battery voltage  ?=help"));
}

void handleSerial() {
  while (Serial.available()) {
    char ch = Serial.read();
    switch (ch) {
      case 'd': case 'D':
        displayTest();
        showText("rEAdy");
        break;
      case 'r': case 'R':
        testedMask = 0;
        Serial.println(F("Key coverage reset."));
        showText("rEAdy");
        break;
      case '+':
        if (brightness < 7) brightness++;
        Serial.print(F("Brightness ")); Serial.println(brightness);
        tmUpdate();
        break;
      case '-':
        if (brightness > 0) brightness--;
        Serial.print(F("Brightness ")); Serial.println(brightness);
        tmUpdate();
        break;
      case 'v': case 'V':
        showBattery();
        break;
      case '?':
        printHelp();
        break;
      default:
        break;
    }
  }
}

// ------------------------------ Arduino -----------------------------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_TM_CLK, OUTPUT);
  pinMode(PIN_TM_DIN, OUTPUT);
  digitalWrite(PIN_TM_CLK, HIGH);
  digitalWrite(PIN_TM_DIN, HIGH);
  restoreKeyPins();

  delay(500);                                  // let TM1640 / 5V rail settle
  Serial.println(F("\n*** VINTAGE LED CALCULATOR - BOARD TEST ***"));
  printHelp();
  Serial.print(F("Battery check: "));
  Serial.print(readBatteryVolts(), 2);
  Serial.println(F(" V\n"));

  clearBuf();
  tmUpdate();

  keyboardSelfCheck();
  displayTest();

  showText("rEAdy");
  Serial.println(F("=== KEY TEST: press every key; all 24 must be seen ==="));
  Serial.println(F("Display shows: row/col, then keys-tested-so-far - 24\n"));
}

void loop() {
  handleSerial();

  uint32_t m = scanMatrix();
  if (m != stableMask) {
    delay(15);                                 // debounce
    if (scanMatrix() == m) {
      uint32_t newly = m & ~stableMask;
      stableMask = m;
      if (newly) onNewKeys(newly, m);
    }
  }
}
