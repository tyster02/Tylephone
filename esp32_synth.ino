/*
 * ESP32 DAC Synthesizer
 * 
 * Hardware Setup:
 * - 12 GPIO pins for keyboard (each with 10kΩ pull-DOWN resistor to GND, touched with 3.3V stylus)
 * - 1 Rotary encoder for volume control
 * - 1 Passive buzzer on GPIO25 (DAC1)
 * - 1 2-way switch for octave selection
 * - 3 buttons for waveform selection (square, triangle, sine)
 * - 3 LEDs to indicate selected waveform
 * - 1 7-segment display (common cathode assumed)
 */

#include <driver/dac.h>

// ============== PIN DEFINITIONS ==============

// Keyboard pins (12 keys - one octave)
const int KEY_PINS[12] = {13, 12, 14, 27, 26, 25, 33, 32, 15, 2, 0, 4};
// Note names for reference: C, C#, D, D#, E, F, F#, G, G#, A, A#, B

// Rotary encoder pins
const int ENCODER_CLK = 18;
const int ENCODER_DT = 19;

// Octave switch
const int OCTAVE_SWITCH = 21;

// Waveform selection buttons
const int BTN_SQUARE = 22;
const int BTN_TRIANGLE = 23;
const int BTN_SINE = 5;

// Waveform indicator LEDs
const int LED_SQUARE = 16;
const int LED_TRIANGLE = 17;
const int LED_SINE = 18;

// 7-segment display pins (a, b, c, d, e, f, g)
const int SEG_PINS[7] = {34, 35, 36, 39, 34, 35, 36};  // UPDATE THESE!

// DAC output
#define DAC_PIN DAC_CHANNEL_1  // GPIO25

// ============== CONFIGURATION ==============

#define SAMPLES 256
#define BASE_OCTAVE 4  // C4 = middle C

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

// ============== STATE VARIABLES ==============

volatile uint8_t *currentWaveform = sineWave;
volatile int currentSample = 0;
volatile int currentFreq = 0;
volatile uint8_t volume = 128;  // 0-255, start at 50%

WaveType selectedWaveform = SINE;
bool isHighOctave = false;
int currentNote = -1;  // -1 = no note playing

// Encoder state
int lastEncoderCLK = HIGH;
int encoderPos = 50;  // Volume 0-100

// Timer
hw_timer_t *timer = NULL;

// Button debounce
unsigned long lastDebounceTime[3] = {0, 0, 0};
const unsigned long debounceDelay = 50;

// 7-segment patterns
// Segments: a, b, c, d, e, f, g (top, top-right, bottom-right, bottom, bottom-left, top-left, middle)
const byte VOLUME_PATTERNS[9] = {
  0b00000001,  // Top segment only (a)
  0b00000011,  // Top + top-right (a, b)
  0b00000111,  // Top, top-right, bottom-right (a, b, c)
  0b00001111,  // Add bottom (a, b, c, d)
  0b00011111,  // Add bottom-left (a, b, c, d, e)
  0b00111111,  // Add top-left (a, b, c, d, e, f)
  0b01111111,  // All segments
  0b01111111,  // All segments (repeated for smooth mapping)
  0b01111111   // All segments
};

// Waveform display patterns (only top 4 segments used)
const byte WAVEFORM_PATTERNS[3] = {
  0b00100101,  // Square: left, top, right segments (a, b, f) - looks like squared top
  0b00100001,  // Triangle: left and top (a, f) - corner shape
  0b00100111   // Sine: left, top, right, top-right (a, b, c, f) - rounded top
};

// ============== TIMER INTERRUPT ==============

void IRAM_ATTR onTimer() {
  if (currentFreq > 0) {  // Only output if a note is playing
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
  Serial.println("\n=== ESP32 DAC Synthesizer ===");
  
  // Initialize keyboard pins (INPUT with external pull-down resistors)
  // Each pin should have a 10kΩ resistor connecting it to GND
  for (int i = 0; i < 12; i++) {
    pinMode(KEY_PINS[i], INPUT);
  }
  
  // Initialize encoder pins
  pinMode(ENCODER_CLK, INPUT);
  pinMode(ENCODER_DT, INPUT);
  
  // Initialize octave switch
  pinMode(OCTAVE_SWITCH, INPUT_PULLUP);
  
  // Initialize waveform buttons
  pinMode(BTN_SQUARE, INPUT_PULLUP);
  pinMode(BTN_TRIANGLE, INPUT_PULLUP);
  pinMode(BTN_SINE, INPUT_PULLUP);
  
  // Initialize waveform LEDs
  pinMode(LED_SQUARE, OUTPUT);
  pinMode(LED_TRIANGLE, OUTPUT);
  pinMode(LED_SINE, OUTPUT);
  
  // Initialize 7-segment display pins
  for (int i = 0; i < 7; i++) {
    pinMode(SEG_PINS[i], OUTPUT);
  }
  
  // Initialize DAC
  dac_output_enable(DAC_PIN);
  
  // Generate waveforms
  generateWaveforms();
  
  // Set up timer
  timer = timerBegin(0, 80, true);  // Timer 0, prescaler 80 (1 MHz)
  timerAttachInterrupt(timer, &onTimer, true);
  timerAlarmWrite(timer, 100, true);  // Will be updated when note plays
  timerAlarmEnable(timer);
  
  // Set initial waveform
  setWaveform(SINE);
  
  // Update display
  updateDisplay(false);  // Show volume initially
  
  Serial.println("Synthesizer ready!");
}

// ============== MAIN LOOP ==============

void loop() {
  // Read octave switch
  isHighOctave = (digitalRead(OCTAVE_SWITCH) == LOW);  // LOW = pressed = high octave
  
  // Read rotary encoder for volume
  readEncoder();
  
  // Read waveform selection buttons
  readWaveformButtons();
  
  // Scan keyboard
  scanKeyboard();
  
  // Small delay to prevent overwhelming the processor
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
  
  // Square wave (50% duty cycle)
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
  
  // Adjust for octave (high octave = double frequency)
  float freq = isHighOctave ? baseFreq * 2.0 : baseFreq;
  
  setFrequency(freq);
  currentNote = noteIndex;
}

void stopNote() {
  currentFreq = 0;
  currentNote = -1;
  dac_output_voltage(DAC_PIN, 0);
}

// ============== KEYBOARD SCANNING ==============

void scanKeyboard() {
  bool anyKeyPressed = false;
  
  // Scan all keys (pins are pulled down, so HIGH = pressed when 3.3V stylus touches)
  for (int i = 0; i < 12; i++) {
    if (digitalRead(KEY_PINS[i]) == HIGH) {  // 3.3V stylus touching = HIGH
      if (currentNote != i) {  // Only change if different note
        playNote(i);
      }
      anyKeyPressed = true;
      break;  // Only play one note at a time (monophonic synth)
    }
  }
  
  // If no keys pressed, stop playing
  if (!anyKeyPressed && currentNote != -1) {
    stopNote();
  }
}

// ============== ROTARY ENCODER ==============

void readEncoder() {
  int currentCLK = digitalRead(ENCODER_CLK);
  
  if (currentCLK != lastEncoderCLK && currentCLK == LOW) {
    // Encoder was rotated
    if (digitalRead(ENCODER_DT) == HIGH) {
      // Clockwise
      encoderPos++;
      if (encoderPos > 100) encoderPos = 100;
    } else {
      // Counter-clockwise
      encoderPos--;
      if (encoderPos < 0) encoderPos = 0;
    }
    
    // Update volume (map 0-100 to 0-255)
    volume = (encoderPos * 255) / 100;
    
    // Update display to show volume
    updateDisplay(false);
    
    Serial.println("Volume: " + String(encoderPos) + "%");
  }
  
  lastEncoderCLK = currentCLK;
}

// ============== WAVEFORM SELECTION ==============

void readWaveformButtons() {
  unsigned long currentTime = millis();
  
  // Check each button with debouncing
  if (digitalRead(BTN_SQUARE) == LOW && currentTime - lastDebounceTime[0] > debounceDelay) {
    setWaveform(SQUARE);
    lastDebounceTime[0] = currentTime;
    updateDisplay(true);  // Briefly show waveform
    delay(500);
    updateDisplay(false);  // Return to volume display
  }
  
  if (digitalRead(BTN_TRIANGLE) == LOW && currentTime - lastDebounceTime[1] > debounceDelay) {
    setWaveform(TRIANGLE);
    lastDebounceTime[1] = currentTime;
    updateDisplay(true);
    delay(500);
    updateDisplay(false);
  }
  
  if (digitalRead(BTN_SINE) == LOW && currentTime - lastDebounceTime[2] > debounceDelay) {
    setWaveform(SINE);
    lastDebounceTime[2] = currentTime;
    updateDisplay(true);
    delay(500);
    updateDisplay(false);
  }
}

void setWaveform(WaveType type) {
  selectedWaveform = type;
  
  // Update waveform pointer
  switch (type) {
    case SQUARE:
      currentWaveform = squareWave;
      break;
    case TRIANGLE:
      currentWaveform = triangleWave;
      break;
    case SINE:
      currentWaveform = sineWave;
      break;
  }
  
  // Update LEDs
  digitalWrite(LED_SQUARE, (type == SQUARE) ? HIGH : LOW);
  digitalWrite(LED_TRIANGLE, (type == TRIANGLE) ? HIGH : LOW);
  digitalWrite(LED_SINE, (type == SINE) ? HIGH : LOW);
  
  Serial.print("Waveform: ");
  Serial.println(type == SQUARE ? "Square" : (type == TRIANGLE ? "Triangle" : "Sine"));
}

// ============== 7-SEGMENT DISPLAY ==============

void updateDisplay(bool showWaveform) {
  byte pattern;
  
  if (showWaveform) {
    // Show waveform shape
    pattern = WAVEFORM_PATTERNS[selectedWaveform];
  } else {
    // Show volume as circular progress (0-100 maps to 0-8 segments)
    int segmentIndex = (encoderPos * 8) / 100;
    if (segmentIndex > 8) segmentIndex = 8;
    pattern = VOLUME_PATTERNS[segmentIndex];
  }
  
  // Output to 7-segment display
  for (int i = 0; i < 7; i++) {
    digitalWrite(SEG_PINS[i], (pattern & (1 << i)) ? HIGH : LOW);
  }
}
