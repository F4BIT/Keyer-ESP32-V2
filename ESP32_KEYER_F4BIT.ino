/*
  ESP32-S3 CW KEYER - v2.3

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

// Winkeyer 2.0 (jeu de commandes K1EL compatible K3NG)
// Le port USB serie fonctionne a 1200 bauds, debit standard Winkeyer.
bool winkeyHostOpen = false;
bool winkeyPaused = false;
bool winkeyKeyImmediate = false;
uint8_t winkeyPending = 0;
uint8_t winkeyParamsNeeded = 0;
uint8_t winkeyParams[16];
uint8_t winkeyParamIndex = 0;
uint8_t winkeyAdminPending = 0;
uint8_t winkeyWeight = 50;
uint8_t winkeyPttLead = 4;
uint8_t winkeyPttTail = 10;
uint8_t winkeyMode = 0;

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
    lastPotWpm = potWpm;
    if (!winkeyHostOpen) {
      wpm = potWpm;
      updateTiming();
      Serial.print("Vitesse : ");
      Serial.print(wpm);
      Serial.println(" WPM");
    }
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

      if (!winkeyHostOpen) {
        Serial.print("Mode Iambic : ");
        Serial.println(iambicB ? "B" : "A");
      }
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
    if (!winkeyHostOpen) Serial.println("Texte interrompu par le paddle.");
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

  if (!winkeyHostOpen) {
    Serial.print("Emission : ");
    Serial.println(textBuffer);
  }
}

void stopText() {
  textActive = false;
  textState = 0;
  textBuffer = "";
  textPosition = 0;

  setCW(false);
  setPTT(false);

  if (!winkeyHostOpen) Serial.println("Emission arretee.");
}

void textKeyer() {
  if (!textActive) return;

  unsigned long now = millis();

  if (textState == 0) {
    if (textPosition >= textBuffer.length()) {
      textActive = false;
      setCW(false);

      if (!winkeyHostOpen) Serial.println("Fin du texte.");
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

  // Commandes CLI K3NG (prefixe antislash)
  if (command == "\\?") {
    Serial.println("K3NG CLI: \\a Iambic A, \\b Iambic B, \\n inverse paddle, \\o sidetone, \\s statut, \\\\ stop");
    return;
  }
  if (command == "\\a") { iambicB = false; Serial.println("Iambic A"); return; }
  if (command == "\\b") { iambicB = true; Serial.println("Iambic B"); return; }
  if (command == "\\n") { paddleReverse = !paddleReverse; Serial.println(paddleReverse ? "Paddle reverse ON" : "Paddle reverse OFF"); return; }
  if (command == "\\o") { sidetoneEnabled = !sidetoneEnabled; Serial.println(sidetoneEnabled ? "Sidetone ON" : "Sidetone OFF"); return; }
  if (command == "\\s") {
    Serial.print("CW Speed WPM: "); Serial.println(wpm);
    Serial.print("Iambic: "); Serial.println(iambicB ? "B" : "A");
    Serial.print("Paddle reverse: "); Serial.println(paddleReverse ? "ON" : "OFF");
    Serial.print("Sidetone: "); Serial.println(sidetoneEnabled ? "ON" : "OFF");
    return;
  }
  if (command == "\\\\") { stopText(); return; }

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

// =====================================================
// EMULATION WINKY 2.0 - SOUS-ENSEMBLE POUR LOGICIELS PC
// =====================================================

void winkeySendStatus() {
  // WK status: bits 7..5 = 110, bit 2 = BUSY, bit 1 = BREAK-IN.
  uint8_t st = 0xC0;
  if (textActive || cwIsDown || winkeyKeyImmediate) st |= 0x04;
  if (digitalRead(PIN_DIT) == LOW || digitalRead(PIN_DAH) == LOW) st |= 0x02;
  Serial.write(st);
}

void winkeySetSpeed(uint8_t value) {
  if (value == 0) return; // 0 = vitesse au potentiometre
  wpm = constrain((int)value, MIN_WPM, MAX_WPM);
  updateTiming();
}

void winkeyExecuteAdmin(uint8_t value) {
  switch (value) {
    case 1: // Reset logique de l'interface
      winkeyPaused = false;
      stopText();
      winkeyHostOpen = false;
      break;
    case 2: // Host Open
      winkeyHostOpen = true;
      winkeyPaused = false;
      // Code de revision emule : Winkeyer 2.x
      Serial.write((uint8_t)0x20);
      break;
    case 3: // Host Close
      winkeyPaused = false;
      winkeyPending = 0;
      winkeyAdminPending = 0;
      stopText();
      winkeyHostOpen = false;
      break;
    case 4: // Echo test : echo du prochain octet
      winkeyAdminPending = 4;
      break;
    case 10: // WK1 mode
    case 11: // WK2 mode
      break;
    default:
      // Commandes admin non implementees : aucune reponse inventee.
      break;
  }
}

void winkeyExecuteCommand(uint8_t command, uint8_t *v) {
  switch (command) {
    case 0x01: // Sidetone control (WK1/WK2)
      sidetoneEnabled = ((v[0] & 0x0F) != 0);
      break;
    case 0x02: // Speed immediate
      winkeySetSpeed(v[0]);
      break;
    case 0x03: // Weighting
      winkeyWeight = constrain((int)v[0], 10, 90);
      break;
    case 0x04: // PTT lead / tail, pas de changement de cablage
      winkeyPttLead = v[0];
      winkeyPttTail = v[1];
      break;
    case 0x05: // Speed pot setup (3 octets)
      break;
    case 0x06: // Pause
      winkeyPaused = (v[0] != 0);
      break;
    case 0x07: // Get speed pot: commande sans parametre, reponse 10xxxxxx
      Serial.write((uint8_t)(0x80 | constrain(wpm - MIN_WPM, 0, 63)));
      break;
    case 0x08: // Backspace : retire un caractere encore en attente
      if (textBuffer.length() > textPosition) {
        textBuffer.remove(textBuffer.length() - 1);
      }
      break;
    case 0x09: // Pin configuration (sorties conservees selon notre cablage)
      break;
    case 0x0A: // Clear buffer
      stopText();
      winkeyPaused = false;
      winkeyKeyImmediate = false;
      break;
    case 0x0B: // Key immediate / tune
      winkeyKeyImmediate = (v[0] != 0);
      setCW(winkeyKeyImmediate);
      if (winkeyKeyImmediate && USE_EXTERNAL_PTT) setPTT(true);
      if (!winkeyKeyImmediate) setPTT(false);
      break;
    case 0x0D: // Farnsworth - non gere dans ce firmware
      break;
    case 0x0E: // Winkeyer mode: Iambic, reverse, serial echo
      winkeyMode = v[0];
      iambicB = ((v[0] & 0x30) == 0);
      paddleReverse = ((v[0] & 0x08) != 0);
      break;
    case 0x0F: // Load Defaults, 15 parametres
      winkeyMode = v[0];
      iambicB = ((v[0] & 0x30) == 0);
      paddleReverse = ((v[0] & 0x08) != 0);
      winkeySetSpeed(v[1]);
      sidetoneEnabled = ((v[2] & 0x0F) != 0);
      winkeyWeight = constrain((int)v[3], 10, 90);
      winkeyPttLead = v[4];
      winkeyPttTail = v[5];
      break;
    case 0x11: // Key compensation - non gere
      break;
    case 0x14: // Software paddle
      if (v[0] == 0) {
        ditMemory = false; dahMemory = false;
      } else {
        if (v[0] & 0x01) ditMemory = true;
        if (v[0] & 0x02) dahMemory = true;
      }
      break;
    case 0x15: // Request status
      winkeySendStatus();
      break;
    case 0x18: // PTT buffered (traitement immediat simplifie)
      if (USE_EXTERNAL_PTT) setPTT(v[0] != 0);
      break;
    case 0x19: // Key buffered : nn secondes
      winkeyKeyImmediate = (v[0] != 0);
      setCW(winkeyKeyImmediate);
      break;
    case 0x1C: // Buffered speed change, appliquee immediatement
      winkeySetSpeed(v[0]);
      break;
    default:
      break;
  }
}

uint8_t winkeyParameterCount(uint8_t command) {
  switch (command) {
    case 0x01: case 0x02: case 0x03: case 0x06:
    case 0x09: case 0x0B: case 0x0C: case 0x0D:
    case 0x0E: case 0x11: case 0x14: case 0x18:
    case 0x19: case 0x1C:
      return 1;
    case 0x04: return 2;
    case 0x05: return 3;
    case 0x0F: return 15;
    case 0x07: case 0x08: case 0x0A: case 0x15:
      return 0;
    default: return 0;
  }
}

void winkeyReceiveByte(uint8_t b) {
  if (winkeyAdminPending == 4) {
    Serial.write(b);
    winkeyAdminPending = 0;
    return;
  }
  if (winkeyAdminPending == 1) {
    winkeyAdminPending = 0;
    winkeyExecuteAdmin(b);
    return;
  }
  if (winkeyPending != 0) {
    winkeyParams[winkeyParamIndex++] = b;
    if (winkeyParamIndex >= winkeyParamsNeeded) {
      winkeyExecuteCommand(winkeyPending, winkeyParams);
      winkeyPending = 0;
      winkeyParamIndex = 0;
      winkeyParamsNeeded = 0;
    }
    return;
  }
  if (b == 0x00) {
    winkeyAdminPending = 1;
    return;
  }
  if (b < 0x20) {
    winkeyPending = b;
    winkeyParamsNeeded = winkeyParameterCount(b);
    winkeyParamIndex = 0;
    if (winkeyParamsNeeded == 0) {
      winkeyExecuteCommand(b, winkeyParams);
      winkeyPending = 0;
    }
    return;
  }
  if (b >= 0x20 && b <= 0x7E) {
    if (textBuffer.length() < 180) {
      textBuffer += (char)toupper((char)b);
      if (!textActive) {
        textPosition = 0;
        textElementPosition = 0;
        textPattern = "";
        textState = 0;
        textActive = true;
      }
    }
  }
}

void readSerialUSB() {
  while (Serial.available()) {
    uint8_t b = (uint8_t)Serial.read();

    if (winkeyHostOpen || winkeyAdminPending != 0 || winkeyPending != 0 ||
        b == 0x00 || b < 0x20 || b == 0x7F) {
      // Les octets de synchronisation Winkeyer (notamment 0x13) arrivent
      // avant Host Open : ils ne doivent pas polluer le terminal texte.
      serialLine = "";
      winkeyReceiveByte(b);
      continue;
    }

    // Mode terminal K3NG simplifie conserve lorsque l'hote Winkeyer est ferme.
    char c = (char)b;
    if (c == '\n' || c == '\r') {
      if (serialLine.length() > 0) {
        processSerialCommand(serialLine);
        serialLine = "";
      }
    } else if (serialLine.length() < 200) {
      serialLine += c;
    }
  }
}

// =====================================================
// INITIALISATION
// =====================================================

void setup() {
  // ESP32-S3 USB natif : dans l'IDE Arduino, activer
  // Tools > USB CDC On Boot = Enabled et USB Mode = Hardware CDC and JTAG.
  // Le debit 1200 est celui annonce au pilote Winkeyer ; sur CDC natif,
  // il s'agit d'un port USB virtuel et non d'une UART physique.
  Serial.begin(1200); // Winkeyer standard : 1200 bauds, 8 bits, sans parite

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

  // Ne rien envoyer au demarrage : les logiciels Winkeyer attendent
  // une reponse binaire uniquement apres la commande Host Open.
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

  if (!winkeyPaused && !winkeyKeyImmediate && paddleState == 0 &&
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
