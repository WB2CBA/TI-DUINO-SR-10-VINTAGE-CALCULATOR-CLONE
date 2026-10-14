/*
  =====================================================================
  SR-10 STYLE SCIENTIFIC CALCULATOR
  ATmega328P-AU + TM1640 + 12-digit HP LED display + 6x4 key matrix
  Arduino IDE, Board: "Arduino Uno" (16 MHz crystal)
  =====================================================================

  Keys (as on the schematic):
      1/x   X2    SQRT   C
      CD    +-    EE     /
      7     8     9      X
      4     5     6      -
      1     2     3      +
      0     .     =      BATT

  Behaviour
    - Chain calculation like the original SR-10 (no operator priority):
      2 + 3 X 4 =  gives 20.
    - 1/x, X2, SQRT work on the number on the display, immediately.
    - +-  changes the sign (of the exponent after EE).
    - EE  starts the exponent: 1.5 EE 3 = 1500, 2 EE +- 4 = 0.0002.
    - C   clears everything (also clears "Error").
    - CD  clears the number being entered; pending operation is kept.
    - BATT shows battery voltage and charge level (see Battery below).
    - Error (division by zero, SQRT of a negative number, result beyond
      +-9.99999999999E99) shows "Error"; press C.

  Battery
    BATT (shift + BATT on the full version) shows the voltage and the charge
    level in percent for 2.5 seconds, e.g. "b  8.74   72".  0 % is 6.7 V and
    100 % is 10.0 V (BATT_MIN_V / BATT_MAX_V).  At or below 6.6 V (BATT_LOW_V)
    the percent is replaced by "Lo".  A battery that stays at or below 6.6 V
    for 5 seconds also gives an automatic "Lo bAt" message (2 seconds, then
    every 30 seconds); press any key to bring the display back.

  Arithmetic
    Real decimal arithmetic with 13 internal digits (the display shows 11,
    the 2 extra "guard" digits make 1/3 X 3 = 1 and 0.1 + 0.2 = 0.3).
    Exponent range 10^-99 ... 10^99.

  Display
    11 digits in normal mode.  Numbers >= 10^11 or < 0.001 switch to
    scientific notation: 8-digit mantissa on the left, exponent at the
    right, e.g.  "-1.2345678 -05".

  Serial Monitor (115200 baud) mirrors the display text for debugging.
  Slide switch ON (9V battery) while uploading and using.
*/

#include <Arduino.h>

// Number type: defined before any function so the Arduino IDE's automatic
// prototypes can use it.  Value = sign * d[0].d[1]...d[12] * 10^e
#define NDIG 13                 // internal mantissa digits
#define BIGL 32                 // work-array length for add/subtract

struct Num {
  bool    neg;
  int16_t e;
  uint8_t d[NDIG];
};

// ----------------------------- Pin map ------------------------------
const uint8_t PIN_TM_CLK = 8;    // PB0 -> TM1640 SCLK
const uint8_t PIN_TM_DIN = 9;    // PB1 -> TM1640 DIN

const uint8_t NUM_ROWS = 6;
const uint8_t NUM_COLS = 4;
const uint8_t ROW_PINS[NUM_ROWS] = {2, 3, 4, 5, 6, 7};     // R1..R6
const uint8_t COL_PINS[NUM_COLS] = {A0, A1, A2, A3};       // C1..C4

// Key codes: i=1/x  q=X2  s=SQRT  C=C  D=CD  n=+-  E=EE  B=BATT
const char KEYMAP[NUM_ROWS][NUM_COLS] = {
  {'i', 'q', 's', 'C'},
  {'D', 'n', 'E', '/'},
  {'7', '8', '9', '*'},
  {'4', '5', '6', '-'},
  {'1', '2', '3', '+'},
  {'0', '.', '=', 'B'}
};

const uint8_t NUM_DIGITS = 12;
const bool    REVERSE_DIGITS = false;   // true if D1 is the right-most digit
const uint8_t BRIGHTNESS = 7;           // 0..7 (7 chosen in brightness test)
const bool    DEBUG_SERIAL = true;      // mirror display text to Serial Monitor

// Battery measurement (BATTSENSE on ADC6, R17 220k / R18 100k)
const uint8_t ADC_BATT_CHANNEL = 6;
const float   BATT_DIV_TOP = 220.0;
const float   BATT_DIV_BOTTOM = 100.0;
const float   ADC_VREF = 5.0;
const float   DIODE_DROP = 0.35;
const float   BATT_MIN_V = 6.7;         // 0 %   (regulator is close to dropout)
const float   BATT_MAX_V = 10.0;        // 100 %
const float   BATT_LOW_V = 6.6;         // "Lo bAt" warning at or below this

// --------------------------- Segment bits ---------------------------
const uint8_t SEG_A  = 0x01;
const uint8_t SEG_B  = 0x02;
const uint8_t SEG_C  = 0x04;
const uint8_t SEG_D  = 0x08;
const uint8_t SEG_E  = 0x10;
const uint8_t SEG_F  = 0x20;
const uint8_t SEG_G  = 0x40;
const uint8_t SEG_DP = 0x80;

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
    case 'A': return SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G;
    case 'L': return SEG_D | SEG_E | SEG_F;
    case 't': return SEG_D | SEG_E | SEG_F | SEG_G;
    case 'b': return SEG_C | SEG_D | SEG_E | SEG_F | SEG_G;
    case 'E': case 'e': return SEG_A | SEG_D | SEG_E | SEG_F | SEG_G;
    case 'o': return SEG_C | SEG_D | SEG_E | SEG_G;
    case 'r': return SEG_E | SEG_G;
    case '-': return SEG_G;
    default:  return 0;               // space and anything unknown
  }
}

// ----------------------- TM1640 bit-banged driver -------------------
uint8_t dispBuf[16];

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
  tmCommand(0x40);                          // auto address increment
  tmStart();
  tmSendByte(0xC0);                         // start at GRID1 / D1
  for (uint8_t i = 0; i < 16; i++) tmSendByte(dispBuf[i]);
  tmStop();
  tmCommand(0x88 | (BRIGHTNESS & 0x07));    // display ON + brightness
}

uint8_t gridForPos(uint8_t pos) {
  return REVERSE_DIGITS ? (NUM_DIGITS - 1 - pos) : pos;
}

// Left-to-right text; a '.' lights the decimal point of the previous digit.
void showText(const char* s) {
  for (uint8_t i = 0; i < 16; i++) dispBuf[i] = 0;
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

// =====================================================================
//                       DECIMAL (BCD) ARITHMETIC
// =====================================================================
// A number is  sign * d[0].d[1]d[2]...d[12] * 10^e   with d[0] != 0
// (zero has d[0] == 0).  All operations round half-up to 13 digits.

// (struct Num and NDIG are defined at the top of the file)

bool mathErr = false;          // set on overflow / divide by zero / sqrt(-x)

void numClear(Num &n) {
  n.neg = false;
  n.e = 0;
  for (uint8_t i = 0; i < NDIG; i++) n.d[i] = 0;
}

bool numIsZero(const Num &n) { return n.d[0] == 0; }

void numOne(Num &n) {
  numClear(n);
  n.d[0] = 1;
}

// Build a normalised, rounded number from a digit array:
//   value = a[0].a[1]a[2]... * 10^exp
void numFromDigits(Num &r, const uint8_t *a, uint8_t len, int16_t exp, bool neg) {
  uint8_t s = 0;
  while (s < len && a[s] == 0) s++;
  if (s == len) { numClear(r); return; }
  exp -= s;

  uint8_t t[NDIG + 1];
  for (uint8_t i = 0; i <= NDIG; i++) {
    uint8_t idx = s + i;
    t[i] = (idx < len) ? a[idx] : 0;
  }
  if (t[NDIG] >= 5) {                         // round half up
    int8_t i = NDIG - 1;
    while (i >= 0) {
      if (t[i] == 9) { t[i] = 0; i--; }
      else { t[i]++; break; }
    }
    if (i < 0) { t[0] = 1; exp++; }           // 9.99..9 -> 10.00..0
  }
  if (exp > 99)  { mathErr = true; numClear(r); return; }
  if (exp < -99) { numClear(r); return; }     // underflow -> 0
  r.neg = neg;
  r.e = exp;
  for (uint8_t i = 0; i < NDIG; i++) r.d[i] = t[i];
}

int8_t cmpDigits(const uint8_t *a, const uint8_t *b, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    if (a[i] != b[i]) return (a[i] > b[i]) ? 1 : -1;
  }
  return 0;
}

void numAdd(Num &r, const Num &a, const Num &b) {
  if (numIsZero(a)) { Num t = b; r = t; return; }
  if (numIsZero(b)) { Num t = a; r = t; return; }

  Num x = a, y = b;                           // x = larger magnitude
  if (b.e > a.e || (b.e == a.e && cmpDigits(b.d, a.d, NDIG) > 0)) { x = b; y = a; }

  int16_t shift = x.e - y.e;
  if (shift >= NDIG + 2) { r = x; return; }   // y is negligible

  uint8_t A[BIGL], B[BIGL];
  for (uint8_t i = 0; i < BIGL; i++) { A[i] = 0; B[i] = 0; }
  for (uint8_t i = 0; i < NDIG; i++) {
    A[1 + i] = x.d[i];
    B[1 + shift + i] = y.d[i];
  }
  const uint8_t len = 2 * NDIG + 2;

  if (x.neg == y.neg) {
    uint8_t c = 0;
    for (int8_t i = len - 1; i >= 0; i--) {
      uint8_t v = A[i] + B[i] + c;
      A[i] = v % 10;
      c = v / 10;
    }
  } else {
    int8_t br = 0;
    for (int8_t i = len - 1; i >= 0; i--) {
      int8_t v = (int8_t)A[i] - (int8_t)B[i] - br;
      if (v < 0) { v += 10; br = 1; } else br = 0;
      A[i] = v;
    }
  }
  numFromDigits(r, A, len, x.e + 1, x.neg);
}

void numSub(Num &r, const Num &a, const Num &b) {
  Num nb = b;
  if (!numIsZero(nb)) nb.neg = !nb.neg;
  numAdd(r, a, nb);
}

void numMul(Num &r, const Num &a, const Num &b) {
  if (numIsZero(a) || numIsZero(b)) { numClear(r); return; }
  uint16_t col[2 * NDIG - 1];
  for (uint8_t k = 0; k < 2 * NDIG - 1; k++) col[k] = 0;
  for (uint8_t i = 0; i < NDIG; i++)
    for (uint8_t j = 0; j < NDIG; j++)
      col[i + j] += (uint16_t)a.d[i] * b.d[j];

  uint8_t R[2 * NDIG];
  uint16_t carry = 0;
  for (int8_t k = 2 * NDIG - 2; k >= 0; k--) {
    uint16_t v = col[k] + carry;
    R[k + 1] = v % 10;
    carry = v / 10;
  }
  R[0] = carry;                               // weight 10^1
  numFromDigits(r, R, 2 * NDIG, a.e + b.e + 1, a.neg != b.neg);
}

void numDiv(Num &r, const Num &a, const Num &b) {
  if (numIsZero(b)) { mathErr = true; return; }
  if (numIsZero(a)) { numClear(r); return; }

  const uint8_t M = NDIG + 2;                 // quotient digits
  uint8_t rem[NDIG + 1], bb[NDIG + 1], q[M];
  rem[0] = 0; bb[0] = 0;
  for (uint8_t i = 0; i < NDIG; i++) { rem[1 + i] = a.d[i]; bb[1 + i] = b.d[i]; }

  for (uint8_t k = 0; k < M; k++) {
    uint8_t count = 0;
    while (cmpDigits(rem, bb, NDIG + 1) >= 0) {
      int8_t br = 0;
      for (int8_t i = NDIG; i >= 0; i--) {
        int8_t v = (int8_t)rem[i] - (int8_t)bb[i] - br;
        if (v < 0) { v += 10; br = 1; } else br = 0;
        rem[i] = v;
      }
      count++;
    }
    q[k] = count;
    for (uint8_t i = 0; i < NDIG; i++) rem[i] = rem[i + 1];   // rem *= 10
    rem[NDIG] = 0;
  }
  numFromDigits(r, q, M, a.e - b.e, a.neg != b.neg);
}

// ---- square root: digit-by-digit, exact for perfect squares ----------
#define RL 20

void arrMulSmall(uint8_t *dst, const uint8_t *src, uint8_t m) {
  uint16_t c = 0;
  for (int8_t i = RL - 1; i >= 0; i--) {
    uint16_t v = (uint16_t)src[i] * m + c;
    dst[i] = v % 10;
    c = v / 10;
  }
}

void arrAddSmall(uint8_t *a, uint8_t v) {
  uint8_t c = v;
  for (int8_t i = RL - 1; i >= 0 && c; i--) {
    uint8_t s = a[i] + c;
    a[i] = s % 10;
    c = s / 10;
  }
}

void arrSub(uint8_t *a, const uint8_t *b) {
  int8_t br = 0;
  for (int8_t i = RL - 1; i >= 0; i--) {
    int8_t v = (int8_t)a[i] - (int8_t)b[i] - br;
    if (v < 0) { v += 10; br = 1; } else br = 0;
    a[i] = v;
  }
}

void numSqrt(Num &r, const Num &a) {
  if (numIsZero(a)) { numClear(r); return; }
  if (a.neg) { mathErr = true; return; }

  const uint8_t PAIRS = 14;                   // 14 root digits
  uint8_t P[2 * PAIRS];
  for (uint8_t i = 0; i < 2 * PAIRS; i++) P[i] = 0;
  bool odd = (a.e & 1);
  uint8_t off = odd ? 0 : 1;
  for (uint8_t i = 0; i < NDIG; i++) P[off + i] = a.d[i];
  int16_t e2 = (a.e - (odd ? 1 : 0)) / 2;

  uint8_t rem[RL], root[RL], y[RL], t[RL];
  for (uint8_t i = 0; i < RL; i++) { rem[i] = 0; root[i] = 0; }

  for (uint8_t k = 0; k < PAIRS; k++) {
    for (uint8_t i = 0; i < RL - 2; i++) rem[i] = rem[i + 2];   // rem *= 100
    rem[RL - 2] = P[2 * k];
    rem[RL - 1] = P[2 * k + 1];

    arrMulSmall(y, root, 20);
    uint8_t d = 0;
    while (d < 9) {
      for (uint8_t i = 0; i < RL; i++) t[i] = y[i];
      arrAddSmall(t, 2 * d + 1);              // (20r+d+1)(d+1) - (20r+d)d
      if (cmpDigits(rem, t, RL) >= 0) { arrSub(rem, t); d++; }
      else break;
    }
    for (uint8_t i = 0; i < RL - 1; i++) root[i] = root[i + 1];
    root[RL - 1] = d;
  }
  numFromDigits(r, root + (RL - PAIRS), PAIRS, e2, false);
}

// =====================================================================
//                       DISPLAY TEXT FORMATTING
// =====================================================================
char shownText[32];

// Right-align body (digits, '-', '.') into 12 digit positions.
void rightAlign(char *out, const char *body) {
  uint8_t k = 0;
  for (const char *p = body; *p; p++) if (*p != '.') k++;
  uint8_t pad = (k < NUM_DIGITS) ? (NUM_DIGITS - k) : 0;
  uint8_t o = 0;
  while (pad--) out[o++] = ' ';
  for (const char *p = body; *p; p++) out[o++] = *p;
  out[o] = 0;
}

// Scientific layout: [sign][8 mantissa digits][exp sign][2 exp digits]
void buildSci(char *out, bool mneg, const char *mant, bool eneg, uint8_t eabs) {
  uint8_t o = 0, used = 0;
  out[o++] = mneg ? '-' : ' ';
  for (const char *m = mant; *m; m++) {
    out[o++] = *m;
    if (*m != '.') used++;
  }
  while (used < 8) { out[o++] = ' '; used++; }
  out[o++] = eneg ? '-' : ' ';
  out[o++] = '0' + eabs / 10;
  out[o++] = '0' + eabs % 10;
  out[o] = 0;
}

// Small numbers (1e-10 ... 1e-4): show them plainly (0.000125) when their
// 8 significant digits still fit after the leading zeros, else scientific.
bool smallFitsFixed(const Num &x, int16_t e) {
  uint8_t t[8];
  for (uint8_t i = 0; i < 8; i++) t[i] = x.d[i];
  if (x.d[8] >= 5) {
    int8_t i = 7;
    while (i >= 0) {
      if (t[i] == 9) { t[i] = 0; i--; }
      else { t[i]++; break; }
    }
    if (i < 0) { t[0] = 1; e++; }
  }
  uint8_t k = 8;
  while (k > 1 && t[k - 1] == 0) k--;
  return (int16_t)k <= 11 + e;
}

bool useSci(const Num &x, int16_t e) {
  if (e >= 11) return true;
  if (e >= -3) return false;
  if (e >= -10) return !smallFitsFixed(x, e);
  return true;
}

// Text for a calculated number (11 significant digits, or scientific).
void formatNum(const Num &x, char *out) {
  if (numIsZero(x)) { rightAlign(out, "0"); return; }

  uint8_t t[NDIG];
  int16_t eEst = x.e, eR = x.e;
  uint8_t s = 11;
  bool sci = false;
  for (uint8_t pass = 0; pass < 2; pass++) {
    bool sciMode = useSci(x, eEst);
    s = sciMode ? 8 : (eEst >= 0 ? 11 : (uint8_t)(11 + eEst));
    for (uint8_t i = 0; i < NDIG; i++) t[i] = x.d[i];
    eR = x.e;
    if (t[s] >= 5) {                          // round to s digits
      int8_t i = s - 1;
      while (i >= 0) {
        if (t[i] == 9) { t[i] = 0; i--; }
        else { t[i]++; break; }
      }
      if (i < 0) { t[0] = 1; eR++; }
    }
    for (uint8_t i = s; i < NDIG; i++) t[i] = 0;
    sci = sciMode;
    if (eR == eEst) break;
    eEst = eR;
  }

  if (sci) {
    char mant[12];
    uint8_t m = 0, last = 7;
    while (last > 0 && t[last] == 0) last--;
    mant[m++] = '0' + t[0];
    mant[m++] = '.';
    for (uint8_t i = 1; i <= last; i++) mant[m++] = '0' + t[i];
    mant[m] = 0;
    buildSci(out, x.neg, mant, eR < 0, (uint8_t)(eR < 0 ? -eR : eR));
    return;
  }

  char body[32];
  uint8_t b = 0, digits = 0;
  if (x.neg) body[b++] = '-';
  uint8_t last = s - 1;
  while (last > 0 && t[last] == 0) last--;
  if (eR >= 0) {
    for (uint8_t i = 0; i <= (uint8_t)eR && digits < 11; i++) { body[b++] = '0' + t[i]; digits++; }
    if (last > (uint8_t)eR) {
      body[b++] = '.';
      for (uint8_t i = eR + 1; i <= last && digits < 11; i++) { body[b++] = '0' + t[i]; digits++; }
    }
  } else {
    body[b++] = '0';
    body[b++] = '.';
    digits = 1;
    for (int16_t z = 0; z < -eR - 1 && digits < 11; z++) { body[b++] = '0'; digits++; }
    for (uint8_t i = 0; i <= last && digits < 11; i++) { body[b++] = '0' + t[i]; digits++; }
  }
  body[b] = 0;
  rightAlign(out, body);
}

// =====================================================================
//                         CALCULATOR LOGIC
// =====================================================================
Num  acc, dispVal;
char pendingOp = 0;            // '+', '-', '*', '/' or 0
bool entering = false;         // a number is being typed
bool haveOperand = false;      // typed number / function result waiting
bool errorState = false;

// number being typed
uint8_t ed[12];
uint8_t en = 0;                // digits typed (leading zeros are not stored)
bool    hasDP = false;
uint8_t dpPos = 0;             // digits before the decimal point
bool    entryNeg = false;
bool    eeActive = false;
bool    expNeg = false;
uint8_t expVal = 0;

unsigned long msgUntil = 0;

void resetEntry() {
  en = 0; hasDP = false; dpPos = 0; entryNeg = false;
  eeActive = false; expNeg = false; expVal = 0;
}

void startEntry() {
  resetEntry();
  entering = true;
  haveOperand = true;
}

Num entryToNum() {
  Num r;
  numClear(r);
  uint8_t z = 0;
  while (z < en && ed[z] == 0) z++;
  if (z == en) return r;                      // zero
  int16_t e = (int16_t)(hasDP ? dpPos : en) - 1 - z;
  e += expNeg ? -(int16_t)expVal : (int16_t)expVal;
  uint8_t tmp[NDIG];
  for (uint8_t i = 0; i < NDIG; i++) tmp[i] = (z + i < en) ? ed[z + i] : 0;
  numFromDigits(r, tmp, NDIG, e, entryNeg);
  return r;
}

void mantText(char *mant) {
  uint8_t m = 0;
  if (en == 0) mant[m++] = '0';
  for (uint8_t i = 0; i < en; i++) {
    mant[m++] = '0' + ed[i];
    if (hasDP && dpPos == i + 1) mant[m++] = '.';
  }
  mant[m] = 0;
}

void entryText(char *out) {
  char mant[24];
  mantText(mant);
  if (eeActive) { buildSci(out, entryNeg, mant, expNeg, expVal); return; }
  char body[28];
  uint8_t b = 0;
  if (entryNeg) body[b++] = '-';
  for (const char *p = mant; *p; p++) body[b++] = *p;
  body[b] = 0;
  rightAlign(out, body);
}

void refresh() {
  if (errorState)    rightAlign(shownText, "Error");
  else if (entering) entryText(shownText);
  else               formatNum(dispVal, shownText);
  showText(shownText);
  if (DEBUG_SERIAL) Serial.println(shownText);
}

void doError() {
  errorState = true;
  mathErr = false;
  entering = false;
  pendingOp = 0;
  haveOperand = false;
  refresh();
}

Num currentValue() { return entering ? entryToNum() : dispVal; }

// Fold the typed number (or function result) into the accumulator.
bool commitOperand() {
  if (haveOperand) {
    Num v = currentValue();
    if (mathErr) { doError(); return false; }
    if (pendingOp) {
      Num r;
      numClear(r);
      switch (pendingOp) {
        case '+': numAdd(r, acc, v); break;
        case '-': numSub(r, acc, v); break;
        case '*': numMul(r, acc, v); break;
        case '/': numDiv(r, acc, v); break;
      }
      if (mathErr) { doError(); return false; }
      acc = r;
    } else {
      acc = v;
    }
    dispVal = acc;
  } else if (!pendingOp) {
    acc = dispVal;
  }
  entering = false;
  haveOperand = false;
  return true;
}

void onDigit(uint8_t d) {
  if (errorState) return;
  if (!entering) startEntry();
  if (eeActive) {
    expVal = (expVal % 10) * 10 + d;          // keep last two digits
  } else if (en < 11) {
    if (!(en == 0 && d == 0 && !hasDP)) ed[en++] = d;   // skip leading zeros
  }
  refresh();
}

void onDot() {
  if (errorState || eeActive) return;
  if (!entering) startEntry();
  if (hasDP) return;
  if (en == 0) ed[en++] = 0;
  hasDP = true;
  dpPos = en;
  refresh();
}

void onSign() {
  if (errorState) return;
  if (eeActive)      expNeg = !expNeg;
  else if (entering) entryNeg = !entryNeg;
  else {
    if (!numIsZero(dispVal)) dispVal.neg = !dispVal.neg;
    haveOperand = true;
  }
  refresh();
}

void onEE() {
  if (errorState || eeActive) return;
  if (!entering) startEntry();
  if (en > 8) return;                         // mantissa must fit 8 digits
  if (en == 0) { ed[en++] = 1; hasDP = false; }
  eeActive = true;
  expVal = 0;
  expNeg = false;
  refresh();
}

void onClearAll() {
  numClear(acc);
  numClear(dispVal);
  pendingOp = 0;
  entering = false;
  haveOperand = false;
  errorState = false;
  mathErr = false;
  resetEntry();
  refresh();
}

void onClearDisplay() {
  if (errorState) return;
  startEntry();                               // empty entry = 0
  refresh();
}

void onOperator(char op) {
  if (errorState) return;
  if (!commitOperand()) return;
  pendingOp = op;
  refresh();
}

void onEquals() {
  if (errorState) return;
  if (!commitOperand()) return;
  pendingOp = 0;
  refresh();
}

void onFunction(char f) {
  if (errorState) return;
  Num v = currentValue();
  if (mathErr) { doError(); return; }
  Num r;
  numClear(r);
  if (f == 'i')      { Num one; numOne(one); numDiv(r, one, v); }
  else if (f == 'q') { numMul(r, v, v); }
  else               { numSqrt(r, v); }
  if (mathErr) { doError(); return; }
  dispVal = r;
  entering = false;
  haveOperand = true;
  refresh();
}

// ------------------------------ Battery -----------------------------
float readBatteryVolts() {
  analogRead(ADC_BATT_CHANNEL);
  uint32_t sum = 0;
  for (uint8_t i = 0; i < 16; i++) sum += analogRead(ADC_BATT_CHANNEL);
  float vPin = (sum / 16.0) * ADC_VREF / 1023.0;
  return vPin * (BATT_DIV_TOP + BATT_DIV_BOTTOM) / BATT_DIV_BOTTOM + DIODE_DROP;
}

// Show a short text for a while, then the calculator display returns.
void showMessage(const char *text, unsigned long ms) {
  showText(text);
  if (DEBUG_SERIAL) Serial.println(text);
  msgUntil = millis() + ms;
  if (msgUntil == 0) msgUntil = 1;
}

// Battery level in percent: BATT_MIN_V = 0 %, BATT_MAX_V = 100 %
int batteryPercent(float v) {
  float p = (v - BATT_MIN_V) * 100.0 / (BATT_MAX_V - BATT_MIN_V);
  if (p < 0.0) p = 0.0;
  if (p > 100.0) p = 100.0;
  return (int)(p + 0.5);
}

// Shows e.g. "b  8.74   72" = voltage on the left, percent on the right.
// At or below BATT_LOW_V the percent is replaced by "Lo".
void showBattery() {
  float v = readBatteryVolts();
  int hundredths = (int)(v * 100.0 + 0.5);
  char buf[20];
  uint8_t n = 0;
  buf[n++] = 'b';
  buf[n++] = ' ';
  buf[n++] = (hundredths >= 1000) ? (char)('0' + (hundredths / 1000) % 10) : ' ';
  buf[n++] = (char)('0' + (hundredths / 100) % 10);
  buf[n++] = '.';
  buf[n++] = (char)('0' + (hundredths / 10) % 10);
  buf[n++] = (char)('0' + hundredths % 10);
  buf[n++] = ' ';
  buf[n++] = ' ';
  buf[n++] = ' ';
  if (v <= BATT_LOW_V) {
    buf[n++] = ' ';
    buf[n++] = 'L';
    buf[n++] = 'o';
  } else {
    int p = batteryPercent(v);
    buf[n++] = (p >= 100) ? '1' : ' ';
    buf[n++] = (p >= 10) ? (char)('0' + (p / 10) % 10) : ' ';
    buf[n++] = (char)('0' + p % 10);
  }
  buf[n] = 0;
  showMessage(buf, 2500UL);
}

// Automatic low-battery warning.  The voltage is read once a second; when it
// has been at or below BATT_LOW_V for LOW_CONFIRM_SECONDS in a row (so a short
// dip under load does not count) "Lo bAt" is shown for 2 seconds, and shown
// again every LOW_REPEAT_MS while the battery stays low.  Any key brings the
// calculator display back.
const uint8_t       LOW_CONFIRM_SECONDS = 5;
const unsigned long LOW_REPEAT_MS = 30000UL;
unsigned long nextBattCheck = 0;
unsigned long nextLowWarn = 0;
uint8_t       lowCount = 0;

void checkBattery() {
  unsigned long now = millis();
  if ((long)(now - nextBattCheck) < 0) return;
  nextBattCheck = now + 1000UL;

  if (readBatteryVolts() <= BATT_LOW_V) {
    if (lowCount < 255) lowCount++;
  } else {
    lowCount = 0;
  }
  if (lowCount >= LOW_CONFIRM_SECONDS && (long)(now - nextLowWarn) >= 0) {
    nextLowWarn = now + LOW_REPEAT_MS;
    showMessage("   Lo bAt", 2000UL);
  }
}

// ------------------------------ Key input ---------------------------
void onKey(char k) {
  if (k != 'B' && msgUntil) { msgUntil = 0; refresh(); }   // key ends a message
  switch (k) {
    case '.': onDot(); break;
    case '+': case '-': case '*': case '/': onOperator(k); break;
    case '=': onEquals(); break;
    case 'i': case 'q': case 's': onFunction(k); break;
    case 'n': onSign(); break;
    case 'E': onEE(); break;
    case 'D': onClearDisplay(); break;
    case 'C': onClearAll(); break;
    case 'B': showBattery(); break;
    default:
      if (k >= '0' && k <= '9') onDigit(k - '0');
      break;
  }
}

uint32_t scanMatrix() {
  uint32_t mask = 0;
  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    digitalWrite(ROW_PINS[r], LOW);
    pinMode(ROW_PINS[r], OUTPUT);
    delayMicroseconds(10);
    for (uint8_t c = 0; c < NUM_COLS; c++) {
      if (digitalRead(COL_PINS[c]) == LOW) mask |= (1UL << (r * NUM_COLS + c));
    }
    pinMode(ROW_PINS[r], INPUT);              // back to high-Z
  }
  return mask;
}

// ------------------------------ Arduino -----------------------------
uint32_t stableMask = 0;

void setup() {
  Serial.begin(115200);

  pinMode(PIN_TM_CLK, OUTPUT);
  pinMode(PIN_TM_DIN, OUTPUT);
  digitalWrite(PIN_TM_CLK, HIGH);
  digitalWrite(PIN_TM_DIN, HIGH);
  for (uint8_t r = 0; r < NUM_ROWS; r++) {
    digitalWrite(ROW_PINS[r], LOW);
    pinMode(ROW_PINS[r], INPUT);
  }
  for (uint8_t c = 0; c < NUM_COLS; c++) pinMode(COL_PINS[c], INPUT_PULLUP);

  delay(500);
  Serial.println(F("\nSR-10 style calculator"));
  showText("  Sr-10");                        // start-up greeting
  delay(1200);

  numClear(acc);
  numClear(dispVal);
  refresh();
}

void loop() {
  checkBattery();
  uint32_t m = scanMatrix();
  if (m != stableMask) {
    delay(15);                                // debounce
    if (scanMatrix() == m) {
      uint32_t newly = m & ~stableMask;
      stableMask = m;
      for (uint8_t i = 0; i < NUM_ROWS * NUM_COLS; i++) {
        if (newly & (1UL << i)) onKey(KEYMAP[i / NUM_COLS][i % NUM_COLS]);
      }
    }
  }
  if (msgUntil && (long)(millis() - msgUntil) >= 0) {
    msgUntil = 0;
    refresh();
  }
}
