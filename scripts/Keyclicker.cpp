// scripts/Keyclicker.cpp

// --- INCLUDE ---
#include "Scripts.h"
#include "../MacroPadLogic.h"
#include "USBHIDKeyboard.h"

// --- CONSTANT ---
const int KEYCLICKER_DELAY_STEP_SIZE = 1;
const int KEYCLICKER_MAX_KEYS = 10;

// --- DATA ---
struct KeyclickerData {
  char keyClickerKeys[KEYCLICKER_MAX_KEYS];
  int keyCount = 0;
  bool isListening = false;
  bool wasListening = false; // TODO might not need
  unsigned long lastClickTime = 0;
  int minDelay = 202;
  int maxDelay = 305;
  unsigned long lastButton1Debounce = 0;
  unsigned long lastButton2Debounce = 0;
  unsigned long lastButton3Debounce = 0;
  unsigned long lastButton4Debounce = 0;
};

// --- GLOBAL ---
extern bool killswitch;
extern USBHIDKeyboard Keyboard;
extern int buttonStates[];
extern const int NORMAL_DEBOUNCE_TIME;

// --- HELPER ---
static void handleInactiveState(KeyclickerData* data, Script* thisScript, unsigned long currentTime);
static void handleActiveState(KeyclickerData* data, Script* thisScript, unsigned long currentTime);
static void updateScriptDisplay(KeyclickerData* data, Script* thisScript);

// --- FUNCTION DEFINITION ---
void setupKeyclicker(Script* thisScript) {
  thisScript->data = new KeyclickerData;

  // Cast the void* data pointer back to our specific data struct.
  KeyclickerData* data = static_cast<KeyclickerData*>(thisScript->data);
  if (!data) return; // Exit if data is not initialized.
  snprintf(thisScript->msg[0], MAX_MSG_LINE_LENGTH, "Min Delay: %dms", data->minDelay);
  snprintf(thisScript->msg[1], MAX_MSG_LINE_LENGTH, "Max Delay: %dms", data->maxDelay);
}

void keyclicker(Script* thisScript) {
  // Cast the void* data pointer back to our specific data struct.
  KeyclickerData* data = static_cast<KeyclickerData*>(thisScript->data);
  if (!data) return; // Exit if data is not initialized.

  unsigned long currentTime = millis();

  if (killswitch || !thisScript->enabled) {
    handleInactiveState(data, thisScript, currentTime);
  } else {
    handleActiveState(data, thisScript, currentTime);
  }

  updateScriptDisplay(data, thisScript);
}

static void handleInactiveState(KeyclickerData* data, Script* thisScript, unsigned long currentTime) {
    keyboard.releaseAll();
  if (!thisScript->selected) return;

  // --- Handle Button 1 ---
  if (buttonStates[0] == LOW && (currentTime - data->lastButton1Debounce > NORMAL_DEBOUNCE_TIME)) {
    data->minDelay = data->minDelay - KEYCLICKER_DELAY_STEP_SIZE;
    data->lastButton1Debounce = currentTime;
  }

  // --- Handle Button 2 ---
  if (buttonStates[1] == LOW && (currentTime - data->lastButton2Debounce > NORMAL_DEBOUNCE_TIME)) {
    data->minDelay = data->minDelay + KEYCLICKER_DELAY_STEP_SIZE;
    data->lastButton2Debounce = currentTime;
  }

  // --- Handle Button 3 ---
  if (buttonStates[2] == LOW && (currentTime - data->lastButton3Debounce > NORMAL_DEBOUNCE_TIME)) {
    data->maxDelay = data->maxDelay - KEYCLICKER_DELAY_STEP_SIZE;
    data->lastButton3Debounce = currentTime;
  }

  // --- Handle Button 4 ---
  if (buttonStates[3] == LOW && (currentTime - data->lastButton4Debounce > NORMAL_DEBOUNCE_TIME)) {
    data->maxDelay = data->maxDelay + KEYCLICKER_DELAY_STEP_SIZE;
    data->lastButton4Debounce = currentTime;
  }

  // Safety checks to prevent invalid delay ranges.
  if (data->minDelay < KEYCLICKER_DELAY_STEP_SIZE) data->minDelay = KEYCLICKER_DELAY_STEP_SIZE;
  if (data->maxDelay < data->minDelay) data->maxDelay = data->minDelay;
  if (data->minDelay > data->maxDelay) data->minDelay = data->maxDelay;
}

static void handleActiveState(KeyclickerData* data, Script* thisScript, unsigned long currentTime) {
  // Check if we need to click.
  if (currentTime - data->lastClickTime > random(data->minDelay, data->maxDelay)) {
      if (data->keyCount > 0) {
          for (int i = 0; i < data->keyCount; i++) {
              Keyboard.press(data->keyClickerKeys[i]);
          }
          delay(100);
          for (int i = 0; i < data->keyCount; i++) {
              Keyboard.release(data->keyClickerKeys[i]);
          }
      }
    data->lastClickTime = currentTime;
  }

  if (!thisScript->selected) return;

  // --- Handle Button 1 ---
  if (buttonStates[0] == LOW && (currentTime - data->lastButton1Debounce > NORMAL_DEBOUNCE_TIME)) {
    data->minDelay = data->minDelay - KEYCLICKER_DELAY_STEP_SIZE;
    data->lastButton1Debounce = currentTime;
  }

  // --- Handle Button 2 ---
  if (buttonStates[1] == LOW && (currentTime - data->lastButton2Debounce > NORMAL_DEBOUNCE_TIME)) {
    data->minDelay = data->minDelay + KEYCLICKER_DELAY_STEP_SIZE;
    data->lastButton2Debounce = currentTime;
  }

  // --- Handle Button 3 ---
  if (buttonStates[2] == LOW && (currentTime - data->lastButton3Debounce > NORMAL_DEBOUNCE_TIME)) {
    data->maxDelay = data->maxDelay - KEYCLICKER_DELAY_STEP_SIZE;
    data->lastButton3Debounce = currentTime;
  }

  // --- Handle Button 4 ---
  if (buttonStates[3] == LOW && (currentTime - data->lastButton4Debounce > NORMAL_DEBOUNCE_TIME)) {
    data->maxDelay = data->maxDelay + KEYCLICKER_DELAY_STEP_SIZE;
    data->lastButton4Debounce = currentTime;
  }

  // Safety checks to prevent invalid delay ranges.
  if (data->minDelay < KEYCLICKER_DELAY_STEP_SIZE) data->minDelay = KEYCLICKER_DELAY_STEP_SIZE;
  if (data->maxDelay < data->minDelay) data->maxDelay = data->minDelay;
  if (data->minDelay > data->maxDelay) data->minDelay = data->maxDelay;
}

static void updateScriptDisplay(KeyclickerData* data, Script* thisScript) {
    char keyString[KEYCLICKER_MAX_KEYS + 1] = "";
    int count = (data->keyCount < KEYCLICKER_MAX_KEYS) ? data->keyCount : KEYCLICKER_MAX_KEYS;
    strncpy(keyString, data->keyClickerKeys, count);
    keyString[count] = '\0';
    snprintf(thisScript->msg[0], MAX_MSG_LINE_LENGTH, "Keys: %s", keyString);
  // snprintf(thisScript->msg[0], MAX_MSG_LINE_LENGTH, "Min Delay: %dms", data->minDelay);
  // snprintf(thisScript->msg[1], MAX_MSG_LINE_LENGTH, "Max Delay: %dms", data->maxDelay);
}

static void recordKey(KeyclickerData* data, char pressedKey) {
    if (keyCount < KEYCLICKER_MAX_KEYS) {
        data->keyClickerKeys[data->keyCount] = pressedKey;
        data->keyCount++;
    }
}
