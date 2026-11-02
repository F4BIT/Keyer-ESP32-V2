
/*
  ESP32-S3 CW KEYER - v2.2

  Auteur : F4BIT Stéphane
  Date   : 2026-11
  Licence: GNU General Public License v3.0
  
  Carte : ESP32-S3-DevKitC-1-N16R8
  Arduino-ESP32 : version 2.x
  
  Fonctions :
  - Paddle double contact DIT / DAH
  - Manipulation Iambic A/B (alternance simplifiee)
  - Reglage vitesse par potentiometre : 5 a 50 WPM
  - Emission de texte depuis le PC par USB serie
  - Sidetone
  - Sortie CW par optocoupleur PC817
  - Sortie PTT optionnelle par PC817
*/

// =====================================================
// CONFIGURATION
// =====================================================

#define USE_EXTERNAL_PTT false
#define DEFAULT_PADDLE_REVERSE false

// Brochage ESP32-S3
#define PIN_DIT       4
#define PIN_DAH       5
#define PIN_CW        6
#define PIN_PTT       7
#define PIN_TONE      8
#define PIN_SPEED_POT 9
#define PIN_MODE      11

// Reglages
int wpm = 20;
const int MIN_WPM = 5;
const int MAX_WPM = 50;

const int SIDETONE_HZ = 650;
const int PTT_LEAD_MS = 40;
const int PTT_TAIL_MS = 100;

bool iambicB = true;
bool paddleReverse = DEFAULT_PADDLE_REVERSE;
bool sidetoneEnabled = true;

// =====================================================
// VARIABLES INTERNES
// =====================================================

unsigned long ditMs = 60;
unsigned long lastPotRead = 0;
unsigned long lastModeButton = 0;
unsigned long lastPTTActivity = 0;

int lastPotWpm = -1;
int potWpm = 20;

bool cwIsDown = false;
bool pttIsDown = false;

// Gestion du paddle
bool ditMemory = false;
bool dahMemory = false;
bool lastElementWasDit = false;

bool squeezeSeen = false;
bool squeezeReleased = false;

int paddleState = 0;
// 0 = attente
// 1 = element en cours
// 2 = silence entre elements

bool currentElementDit = true;
unsigned long paddleDeadline = 0;

// Gestion du texte USB
String serialLine = "";
String textBuffer = "";
int textPosition = 0;

String textPattern = "";
int textElementPosition = 0;

bool textActive = false;
int textState = 0;
// 0 = attente
// 1 = element CW
// 2 = silence

unsigned long textDeadline = 0;

// =====================================================
// TABLE MORSE
// =====================================================

const char* morseFor(char c) {
  switch (c) {
    case 'A': return ".-";
    case 'B': return "-...";
    case 'C': return "-.-.";
    case 'D': return "-..";
    case 'E': return ".";
    case 'F': return "..-.";
    case 'G': return "--.";
    case 'H': return "....";
    case 'I': return "..";
    case 'J': return ".---";
    case 'K': return "-.-";
    case 'L': return ".-..";
    case 'M': return "--";
    case 'N': return "-.";
    case 'O': return "---";
    case 'P': return ".--.";
    case 'Q': return "--.-";
    case 'R': return ".-.";
    case 'S': return "...";
    case 'T': return "-";
    case 'U': return "..-";
    case 'V': return "...-";
    case 'W': return ".--";
    case 'X': return "-..-";
    case 'Y': return "-.--";
    case 'Z': return "--..";

    case '0': return "-----";
    case '1': return ".----";
    case '2': return "..---";
    case '3': return "...--";
    case '4': return "....-";
    case '5': return ".....";
    case '6': return "-....";
    case '7': return "--...";
    case '8': return "---..";
    case '9': return "----.";

    case '.': return ".-.-.-";
    case ',': return "--..--";
    case '?': return "..--..";
    case '/': return "-..-.";
    case '=': return "-...-";
    case '+': return ".-.-.";
    case '-': return "-....-";
    case '\'': return ".----.";
    case '(': return "-.--.";
    case ')': return "-.--.-";

    default: return "";
  }
}

// =====================================================
// TEMPO
// =====================================================

void updateTiming() {
  // Duree d'un point selon la norme PARIS
  ditMs = 1200UL / wpm;
}

// =====================================================
// SORTIES
// =====================================================

void setCW(bool state) {
  cwIsDown = state;
  digitalWrite(PIN_CW, state ? HIGH : LOW);

  if (sidetoneEnabled) {
    if (state) {
      ledcWriteTone(0, SIDETONE_HZ);
    } else {
      ledcWriteTone(0, 0);
    }
  }
}

void setPTT(bool state) {
  if (!USE_EXTERNAL_PTT) return;

  pttIsDown = state;
  digitalWrite(PIN_PTT, state ? HIGH : LOW);
}

// =====================================================
// POTENTIOMETRE DE VITESSE
// =====================================================

void readSpeedPot() {
  unsigned long now = millis();

  if (now - lastPotRead < 50) return;
  lastPotRead = now;

  int raw = analogRead(PIN_SPEED_POT);

  potWpm = map(raw, 0, 4095, MIN_WPM, MAX_WPM);
  potWpm = constrain(potWpm, MIN_WPM, MAX_WPM);

  if (potWpm != lastPotWpm) {
    wpm = potWpm;
    updateTiming();

    lastPotWpm = potWpm;

    Serial.print("Vitesse : ");
    Serial.print(wpm);
    Serial.println(" WPM");
  }
}

// =====================================================
// BOUTON IAMBIC A/B
// =====================================================

void readModeButton() {
  if (digitalRead(PIN_MODE) == LOW) {
    if (millis() - lastModeButton > 300) {
      iambicB = !iambicB;
      lastModeButton = millis();

      Serial.print("Mode Iambic : ");
      Serial.println(iambicB ? "B" : "A");
    }
  }
}

// =====================================================
// LECTURE PADDLE
// =====================================================

void readPaddles() {
  bool dit = digitalRead(PIN_DIT) == LOW;
  bool dah = digitalRead(PIN_DAH) == LOW;

  if (paddleReverse) {
    bool temp = dit;
    dit = dah;
    dah = temp;
  }

  if (dit) ditMemory = true;
  if (dah) dahMemory = true;

  if (dit && dah) {
    squeezeSeen = true;
  }

  if (!dit && !dah && squeezeSeen) {
    squeezeReleased = true;
  }
}

// =====================================================
// CHOIX DU PROCHAIN ELEMENT PADDLE
// =====================================================

bool choosePaddleElement() {
  if (!ditMemory && !dahMemory) return false;

  if (ditMemory && dahMemory) {
    currentElementDit = !lastElementWasDit;
  } else if (ditMemory) {
    currentElementDit = true;
  } else {
    currentElementDit = false;
  }

  if (currentElementDit) {
    ditMemory = false;
  } else {
    dahMemory = false;
  }

  lastElementWasDit = currentElementDit;
  return true;
}

// =====================================================
// MANIPULATION AU PADDLE
// =====================================================

void paddleKeyer() {
  unsigned long now = millis();

  readPaddles();

  // Un paddle utilise le keyer en priorite.
  bool paddlePressed =
    digitalRead(PIN_DIT) == LOW ||
    digitalRead(PIN_DAH) == LOW;

  if (paddlePressed && textActive) {
    textActive = false;
    textState = 0;
    textBuffer = "";
    textPosition = 0;
    Serial.println("Texte interrompu par le paddle.");
  }

  if (paddleState == 0) {
    if (choosePaddleElement()) {
      unsigned long duration =
        currentElementDit ? ditMs : ditMs * 3;

      setCW(true);
      paddleDeadline = now + duration;
      paddleState = 1;

      squeezeSeen = paddlePressed;
      squeezeReleased = false;

      if (USE_EXTERNAL_PTT && !pttIsDown) {
        setPTT(true);
      }

      lastPTTActivity = now;
    }
  }

  else if (paddleState == 1) {
    if ((long)(now - paddleDeadline) >= 0) {
      setCW(false);

      paddleDeadline = now + ditMs;
      paddleState = 2;

      // En mode B, memoriser un element supplementaire
      // si les deux palettes ont ete pressees puis relachees.
      if (iambicB && squeezeSeen && squeezeReleased) {
        if (currentElementDit) {
          dahMemory = true;
        } else {
          ditMemory = true;
        }
      }

      squeezeSeen = false;
      squeezeReleased = false;
    }
  }

  else if (paddleState == 2) {
    if ((long)(now - paddleDeadline) >= 0) {
      paddleState = 0;
    }
  }
}

// =====================================================
// EMISSION DU TEXTE USB
// =====================================================

void startText(String text) {
  text.toUpperCase();

  textBuffer = text;
  textPosition = 0;

  textElementPosition = 0;
  textPattern = "";

  textState = 0;
  textActive = true;

  Serial.print("Emission : ");
  Serial.println(textBuffer);
}

void stopText() {
  textActive = false;
  textState = 0;
  textBuffer = "";
  textPosition = 0;

  setCW(false);
  setPTT(false);

  Serial.println("Emission arretee.");
}

void textKeyer() {
  if (!textActive) return;

  unsigned long now = millis();

  if (textState == 0) {
    if (textPosition >= textBuffer.length()) {
      textActive = false;
      setCW(false);

      Serial.println("Fin du texte.");
      return;
    }

    char c = textBuffer.charAt(textPosition++);

    // Espace entre les mots
    if (c == ' ') {
      textDeadline = now + ditMs * 7;
      textState = 2;
      return;
    }

    textPattern = morseFor(c);
    textElementPosition = 0;

    // Caractere non gere : on passe au suivant
    if (textPattern.length() == 0) {
      textDeadline = now + ditMs * 3;
      textState = 2;
      return;
    }

    char element = textPattern.charAt(textElementPosition++);
    bool isDit = (element == '.');

    setCW(true);

    textDeadline = now + (isDit ? ditMs : ditMs * 3);
    textState = 1;

    return;
  }

  if (textState == 1) {
    if ((long)(now - textDeadline) >= 0) {
      setCW(false);

      bool lastElement =
        textElementPosition >= textPattern.length();

      textDeadline = now + (lastElement ? ditMs * 3 : ditMs);
      textState = 2;
    }

    return;
  }

  if (textState == 2) {
    if ((long)(now - textDeadline) >= 0) {
      textState = 0;
    }
  }
}

// =====================================================
// COMMANDES USB SERIE
// =====================================================

void processSerialCommand(String command) {
  command.trim();

  if (command.length() == 0) return;

  // Reglage manuel : /W20
  if (command.startsWith("/W")) {
    int value = command.substring(2).toInt();

    if (value >= MIN_WPM && value <= MAX_WPM) {
      wpm = value;
      updateTiming();

      Serial.print("Vitesse demandee : ");
      Serial.print(wpm);
      Serial.println(" WPM");

      Serial.println(
        "Le potentiometre reprendra la main a sa prochaine lecture."
      );
    } else {
      Serial.println("Vitesse autorisee : 5 a 50 WPM.");
    }

    return;
  }

  if (command == "/A") {
    iambicB = false;
    Serial.println("Mode Iambic A");
    return;
  }

  if (command == "/B") {
    iambicB = true;
    Serial.println("Mode Iambic B");
    return;
  }

  if (command == "/STOP") {
    stopText();
    return;
  }

  if (command == "/INFO") {
    Serial.print("Vitesse : ");
    Serial.println(wpm);

    Serial.print("Mode : ");
    Serial.println(iambicB ? "Iambic B" : "Iambic A");

    Serial.print("PTT externe : ");
    Serial.println(USE_EXTERNAL_PTT ? "ON" : "OFF");

    return;
  }

  // Toute autre ligne est interpretee comme du texte Morse.
  startText(command);
}

void readSerialUSB() {
  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      if (serialLine.length() > 0) {
        processSerialCommand(serialLine);
        serialLine = "";
      }
    } else {
      if (serialLine.length() < 200) {
        serialLine += c;
      }
    }
  }
}

// =====================================================
// INITIALISATION
// =====================================================

void setup() {
  Serial.begin(115200);

  pinMode(PIN_DIT, INPUT_PULLUP);
  pinMode(PIN_DAH, INPUT_PULLUP);

  pinMode(PIN_CW, OUTPUT);
  pinMode(PIN_PTT, OUTPUT);

  pinMode(PIN_MODE, INPUT_PULLUP);
  pinMode(PIN_SPEED_POT, INPUT);

  digitalWrite(PIN_CW, LOW);
  digitalWrite(PIN_PTT, LOW);

  // ADC 12 bits pour le potentiometre
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_SPEED_POT, ADC_11db);

  // Sidetone avec LEDC (Arduino-ESP32 2.x)
  // Canal 0, resolution 8 bits
  ledcSetup(0, SIDETONE_HZ, 8);
  ledcAttachPin(PIN_TONE, 0);
  ledcWriteTone(0, 0);

  updateTiming();

  Serial.println();
  Serial.println("==================================");
  Serial.println(" ESP32-S3 CW KEYER - K3NG simplifie");
  Serial.println("==================================");
  Serial.println("Paddle : pret");
  Serial.println("USB serie : 115200 bauds");
  Serial.println("Potentiometre : 5 a 50 WPM");
  Serial.println();
  Serial.println("Commandes :");
  Serial.println("  Texte + Entree : emission CW");
  Serial.println("  /W20            : vitesse 20 WPM");
  Serial.println("  /A              : Iambic A");
  Serial.println("  /B              : Iambic B");
  Serial.println("  /STOP           : arret");
  Serial.println("  /INFO           : informations");
  Serial.println();
}

// =====================================================
// BOUCLE PRINCIPALE
// =====================================================

void loop() {
  readSpeedPot();
  readModeButton();
  readSerialUSB();

  // Priorite au paddle
  paddleKeyer();

  if (paddleState == 0 &&
      digitalRead(PIN_DIT) == HIGH &&
      digitalRead(PIN_DAH) == HIGH) {
    textKeyer();
  }

  // Gestion PTT externe optionnelle
  if (USE_EXTERNAL_PTT) {
    bool transmitting = cwIsDown || textActive ||
                       paddleState != 0;

    if (!transmitting && pttIsDown) {
      if (millis() - lastPTTActivity >= PTT_TAIL_MS) {
        setPTT(false);
      }
    }
  }
}
