/*
 * ESP32 DAC Synthesizer - Rotary Menu Edition
 * 
 * Hardware Setup:
 * - 12 GPIO pins for keyboard (each with 10kΩ pull-DOWN resistor to GND, touched with 3.3V stylus)
 * - 1 Rotary encoder (CLK, DT, SW) for all control
 * - 1 Passive buzzer on GPIO25 (DAC1)
 * - 1 7-segment display (common cathode)
 * 
 * UI Flow:
 * - Rest mode: 3 horizontal lines, dial adjusts volume
 * - Click: Enter menu (Waveform → Octave → Choppiness)
 * - Turn dial: Scroll through menu screens
 * - Click on a screen: Enter that setting, turn dial to adjust
 * - Click again: Save and return to rest mode
 */

#include <driver/dac.h>

// ============== PIN DEFINITIONS ==============

// Keyboard pins (12 keys - one octave)
const int KEY_PINS[12] = {13, 12, 14, 27, 26, 33, 32, 15, 2, 4, 16, 17};
// Note names: C, C#, D, D#, E, F, F#, G, G#, A, A#, B

// Rotary encoder pins
const int ENCODER_CLK = 18;
const int ENCODER_DT = 19;
const int ENCODER_SW = 21;

// 7-segment display pins (a, b, c, d, e, f, g)
// UPDATE THESE TO MATCH YOUR WIRING!
const int SEG_PINS[7] = {34, 35, 36, 39, 25, 26, 27};

// DAC output
#define DAC_PIN DAC_CHANNEL_1  // GPIO25

// ============== CONFIGURATION ==============

#define SAMPLES 256
#define RATCHET_BASE_HZ 8  // Base frequency for ratchet effect (adjustable)

// Chromatic scale frequencies for octave 4 (middle octave)
const float NOTE_FREQS_OCT4[12] = {
  261.63,  // C4
  277.18,  // C#4
  293.66,  // D4
  311.13,  // D#4
  329.63,  // E4
  349.23,  // F4
  369.99,  // F#4
  392.00,  // G4
  415.30,  // G#4
  440.00,  // A4
  466.16,  // A#4
  493.88   // B4
};

// ============== WAVEFORM DATA ==============

enum WaveType {
  SQUARE = 0,
  TRIANGLE = 1,
  SINE = 2
};

uint8_t squareWave[SAMPLES];
uint8_t triangleWave[SAMPLES];
uint8_t sineWave[SAMPLES];

// ============== UI STATE ==============

enum UIMode {
  REST,           // Normal playing mode - show volume
  MENU_BROWSE,    // Browsing menu screens (waveform/octave/choppiness)
  SETTING_ADJUST  // Adjusting a specific setting
};

enum MenuScreen {
  MENU_WAVEFORM = 0,
  MENU_OCTAVE = 1,
  MENU_CHOPPINESS = 2
};

UIMode currentMode = REST;
MenuScreen currentMenu = MENU_WAVEFORM;

// ============== SYNTH STATE ==============

volatile uint8_t *currentWaveform = sineWave;
volatile int currentSample = 0;
volatile int currentFreq = 0;
volatile uint8_t volume = 128;  // 0-255, start at 50%

WaveType selectedWaveform = SINE;
bool isHighOctave = false;
int choppiness = 1;  // 1, 2, 4, or 8
int currentNote = -1;

// Ratchet effect variables
unsigned long lastRatchetToggle = 0;
bool ratchetOn = true;

// Encoder state
int lastEncoderCLK = HIGH;
int encoderValue = 50;  // Current encoder position (0-100 for volume, or menu index)
int lastButtonState = HIGH;
unsigned long lastButtonPress = 0;
const unsigned long buttonDebounce = 200;

// Timer
hw_timer_t *timer = NULL;

// ============== 7-SEGMENT PATTERNS ==============

// Rest mode: 3 horizontal lines (top, middle, bottom)
const byte PATTERN_REST = 0b01001001;  // segments a, d, g

// Volume patterns - fill circle clockwise (0-8 segments)
const byte VOLUME_PATTERNS[9] = {
  0b00000000,  // Empty (0%)
  0b00000001,  // a (top)
  0b00000011,  // a, b (top, top-right)
  0b00000111,  // a, b, c (add bottom-right)
  0b00001111,  // a, b, c, d (add bottom)
  0b00011111,  // a, b, c, d, e (add bottom-left)
  0b00111111,  // a, b, c, d, e, f (add top-left)
  0b00111111,  // Full circle (no middle segment)
  0b00111111   // Full (100%)
};

// Menu screen patterns
const byte PATTERN_MENU_WAVEFORM = 0b01110110;  // Looks like "H" (symmetric shape)
const byte PATTERN_MENU_OCTAVE = 0b00111111;    // "O" shape
const byte PATTERN_MENU_CHOPPINESS = 0b00000110; // Two vertical lines (b, c)

// Waveform option patterns
const byte PATTERN_WAVE_SINE = 0b00111111;      // Circle "O"
const byte PATTERN_WAVE_SQUARE = 0b00100111;    // Square top (a, b, c, f)
const byte PATTERN_WAVE_TRIANGLE = 0b00100001;  // Corner (a, f)

// Octave option patterns
const byte PATTERN_OCTAVE_LOW = 0b00001000;     // Bottom line (d)
const byte PATTERN_OCTAVE_HIGH = 0b00000001;    // Top line (a)

// Choppiness patterns (fill corners: 1, 2, 4, 8)
const byte PATTERN_CHOP_1 = 0b00010000;  // Bottom-left (e)
const byte PATTERN_CHOP_2 = 0b00011000;  // Bottom-left + bottom-right (e, c)
const byte PATTERN_CHOP_4 = 0b00111000;  // Add top-right (e, c, b)
const byte PATTERN_CHOP_8 = 0b00111000 | 0b00100000;  // All four corners (e, c, b, f)

// ============== TIMER INTERRUPT ==============

void IRAM_ATTR onTimer() {
  // Handle ratchet effect
  if (choppiness > 1) {
    unsigned long now = millis();
    int ratchetPeriod = 1000 / (RATCHET_BASE_HZ * (choppiness / 2));
    
    if (now - lastRatchetToggle >= ratchetPeriod) {
      ratchetOn = !ratchetOn;
      lastRatchetToggle = now;
    }
  } else {
    ratchetOn = true;  // No ratchet effect
  }
  
  if (currentFreq > 0 && ratchetOn) {
    uint8_t scaledValue = (currentWaveform[currentSample] * volume) / 255;
    dac_output_voltage(DAC_PIN, scaledValue);
    currentSample = (currentSample + 1) % SAMPLES;
  } else {
    dac_output_voltage(DAC_PIN, 0);  // Silence
  }
}

// ============== SETUP ==============

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32 DAC Synthesizer - Menu Edition ===");
  
  // Initialize keyboard pins
  for (int i = 0; i < 12; i++) {
    pinMode(KEY_PINS[i], INPUT);
  }
  
  // Initialize encoder
  pinMode(ENCODER_CLK, INPUT);
  pinMode(ENCODER_DT, INPUT);
  pinMode(ENCODER_SW, INPUT_PULLUP);
  
  // Initialize 7-segment display
  for (int i = 0; i < 7; i++) {
    pinMode(SEG_PINS[i], OUTPUT);
  }
  
  // Initialize DAC
  dac_output_enable(DAC_PIN);
  
  // Generate waveforms
  generateWaveforms();
  
  // Set up timer
  timer = timerBegin(0, 80, true);
  timerAttachInterrupt(timer, &onTimer, true);
  timerAlarmWrite(timer, 100, true);
  timerAlarmEnable(timer);
  
  // Show rest mode
  updateDisplay();
  
  Serial.println("Synthesizer ready! Turn dial for volume, click to enter menu.");
}

// ============== MAIN LOOP ==============

void loop() {
  // Read encoder rotation
  readEncoder();
  
  // Read encoder button
  readEncoderButton();
  
  // Scan keyboard
  scanKeyboard();
  
  delay(1);
}

// ============== WAVEFORM GENERATION ==============

void generateWaveforms() {
  // Sine wave
  for (int i = 0; i < SAMPLES; i++) {
    sineWave[i] = (uint8_t)(127.5 + 127.5 * sin(2.0 * PI * i / SAMPLES));
  }
  
  // Triangle wave
  for (int i = 0; i < SAMPLES; i++) {
    if (i < SAMPLES / 2) {
      triangleWave[i] = (uint8_t)(i * 512 / SAMPLES);
    } else {
      triangleWave[i] = (uint8_t)(255 - ((i - SAMPLES / 2) * 512 / SAMPLES));
    }
  }
  
  // Square wave
  for (int i = 0; i < SAMPLES; i++) {
    squareWave[i] = (i < SAMPLES / 2) ? 255 : 0;
  }
}

// ============== FREQUENCY CONTROL ==============

void setFrequency(float freq) {
  if (freq <= 0) {
    currentFreq = 0;
    return;
  }
  
  currentFreq = (int)freq;
  timerAlarmDisable(timer);
  timerAlarmWrite(timer, 1000000 / (currentFreq * SAMPLES), true);
  timerAlarmEnable(timer);
}

void playNote(int noteIndex) {
  if (noteIndex < 0 || noteIndex >= 12) {
    stopNote();
    return;
  }
  
  float baseFreq = NOTE_FREQS_OCT4[noteIndex];
  float freq = isHighOctave ? baseFreq * 2.0 : baseFreq;
  
  setFrequency(freq);
  currentNote = noteIndex;
  lastRatchetToggle = millis();  // Reset ratchet timing
  ratchetOn = true;
}

void stopNote() {
  currentFreq = 0;
  currentNote = -1;
  dac_output_voltage(DAC_PIN, 0);
}

// ============== KEYBOARD SCANNING ==============

void scanKeyboard() {
  bool anyKeyPressed = false;
  
  for (int i = 0; i < 12; i++) {
    if (digitalRead(KEY_PINS[i]) == HIGH) {
      if (currentNote != i) {
        playNote(i);
      }
      anyKeyPressed = true;
      break;
    }
  }
  
  if (!anyKeyPressed && currentNote != -1) {
    stopNote();
  }
}

// ============== ROTARY ENCODER ==============

void readEncoder() {
  int currentCLK = digitalRead(ENCODER_CLK);
  
  if (currentCLK != lastEncoderCLK && currentCLK == LOW) {
    int direction = (digitalRead(ENCODER_DT) == HIGH) ? 1 : -1;
    
    switch (currentMode) {
      case REST:
        // Adjust volume
        encoderValue += direction * 5;  // Change by 5% increments
        encoderValue = constrain(encoderValue, 0, 100);
        volume = (encoderValue * 255) / 100;
        Serial.println("Volume: " + String(encoderValue) + "%");
        break;
        
      case MENU_BROWSE:
        // Cycle through menu screens
        if (direction > 0) {
          currentMenu = (MenuScreen)((currentMenu + 1) % 3);
        } else {
          currentMenu = (MenuScreen)((currentMenu + 2) % 3);  // +2 is same as -1 mod 3
        }
        Serial.print("Menu: ");
        Serial.println(currentMenu == MENU_WAVEFORM ? "Waveform" : 
                      (currentMenu == MENU_OCTAVE ? "Octave" : "Choppiness"));
        break;
        
      case SETTING_ADJUST:
        adjustCurrentSetting(direction);
        break;
    }
    
    updateDisplay();
  }
  
  lastEncoderCLK = currentCLK;
}

void readEncoderButton() {
  int buttonState = digitalRead(ENCODER_SW);
  
  if (buttonState == LOW && lastButtonState == HIGH && 
      millis() - lastButtonPress > buttonDebounce) {
    
    handleButtonPress();
    lastButtonPress = millis();
  }
  
  lastButtonState = buttonState;
}

void handleButtonPress() {
  switch (currentMode) {
    case REST:
      // Enter menu
      currentMode = MENU_BROWSE;
      currentMenu = MENU_WAVEFORM;
      Serial.println("Entered menu");
      break;
      
    case MENU_BROWSE:
      // Enter setting adjustment
      currentMode = SETTING_ADJUST;
      Serial.println("Adjusting setting...");
      break;
      
    case SETTING_ADJUST:
      // Save and return to rest
      currentMode = REST;
      encoderValue = (volume * 100) / 255;  // Reset encoder to current volume
      Serial.println("Saved! Back to rest mode");
      break;
  }
  
  updateDisplay();
}

void adjustCurrentSetting(int direction) {
  switch (currentMenu) {
    case MENU_WAVEFORM:
      // Cycle through waveforms
      if (direction > 0) {
        selectedWaveform = (WaveType)((selectedWaveform + 1) % 3);
      } else {
        selectedWaveform = (WaveType)((selectedWaveform + 2) % 3);
      }
      
      // Update waveform pointer
      switch (selectedWaveform) {
        case SINE:
          currentWaveform = sineWave;
          Serial.println("Waveform: Sine");
          break;
        case SQUARE:
          currentWaveform = squareWave;
          Serial.println("Waveform: Square");
          break;
        case TRIANGLE:
          currentWaveform = triangleWave;
          Serial.println("Waveform: Triangle");
          break;
      }
      break;
      
    case MENU_OCTAVE:
      // Toggle octave
      isHighOctave = !isHighOctave;
      Serial.println(isHighOctave ? "Octave: High" : "Octave: Low");
      
      // Update current note if playing
      if (currentNote != -1) {
        playNote(currentNote);
      }
      break;
      
    case MENU_CHOPPINESS:
      // Cycle through choppiness: 1 → 2 → 4 → 8 → 1
      if (direction > 0) {
        choppiness = (choppiness >= 8) ? 1 : choppiness * 2;
      } else {
        choppiness = (choppiness <= 1) ? 8 : choppiness / 2;
      }
      Serial.println("Choppiness: " + String(choppiness));
      break;
  }
}

// ============== 7-SEGMENT DISPLAY ==============

void updateDisplay() {
  byte pattern = 0;
  
  switch (currentMode) {
    case REST:
      // Show volume as filling circle
      int volumeLevel = (encoderValue * 8) / 100;
      if (volumeLevel > 8) volumeLevel = 8;
      pattern = VOLUME_PATTERNS[volumeLevel];
      break;
      
    case MENU_BROWSE:
      // Show which menu screen we're on
      switch (currentMenu) {
        case MENU_WAVEFORM:
          pattern = PATTERN_MENU_WAVEFORM;
          break;
        case MENU_OCTAVE:
          pattern = PATTERN_MENU_OCTAVE;
          break;
        case MENU_CHOPPINESS:
          pattern = PATTERN_MENU_CHOPPINESS;
          break;
      }
      break;
      
    case SETTING_ADJUST:
      // Show current setting value
      switch (currentMenu) {
        case MENU_WAVEFORM:
          switch (selectedWaveform) {
            case SINE:
              pattern = PATTERN_WAVE_SINE;
              break;
            case SQUARE:
              pattern = PATTERN_WAVE_SQUARE;
              break;
            case TRIANGLE:
              pattern = PATTERN_WAVE_TRIANGLE;
              break;
          }
          break;
          
        case MENU_OCTAVE:
          pattern = isHighOctave ? PATTERN_OCTAVE_HIGH : PATTERN_OCTAVE_LOW;
          break;
          
        case MENU_CHOPPINESS:
          switch (choppiness) {
            case 1:
              pattern = PATTERN_CHOP_1;
              break;
            case 2:
              pattern = PATTERN_CHOP_2;
              break;
            case 4:
              pattern = PATTERN_CHOP_4;
              break;
            case 8:
              pattern = PATTERN_CHOP_8;
              break;
          }
          break;
      }
      break;
  }
  
  // Output to 7-segment display
  for (int i = 0; i < 7; i++) {
    digitalWrite(SEG_PINS[i], (pattern & (1 << i)) ? HIGH : LOW);
  }
}
