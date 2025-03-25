#include <M5Unified.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLERemoteCharacteristic.h>

// --- BLE UUID Definitions ---
#define SERVICE_UUID           "ce062b2f-e42b-4239-b951-f9d4b4abe0ff"
#define CHARACTERISTIC_UUID    "46f27243-ac2d-4b01-b909-4b5711a23a8d"

// --- Game Constants ---
#define MAX_ROUNDS 5
#define NUM_BARRELS 3

// --- Role Definitions ---
enum Role { ROLE_UNDEFINED, ROLE_SHOOTER, ROLE_DODGER };
Role deviceRole = ROLE_UNDEFINED;

// --- Shooter Game States ---
enum ShooterState { SHOOTER_WAIT_DODGER, SHOOTER_WAIT_INPUT, SHOOTER_SHOW_RESULT, SHOOTER_GAME_OVER };
ShooterState shooterState = SHOOTER_WAIT_DODGER;

// --- Dodger Game States ---
enum DodgerState { DODGER_WAIT_INPUT, DODGER_WAIT_SHOT, DODGER_SHOW_RESULT, DODGER_GAME_OVER };
DodgerState dodgerState = DODGER_WAIT_INPUT;

// --- Global Game Variables ---
int roundNumber = 1;
bool gameOver = false;
bool roundResultSafe = false; // true if dodger successfully hides this round

// Choices for current round (values 1, 2, or 3)
int dodgerChoice = 0;
int shooterChoice = 0;

// --- BLE Communication Flags ---
volatile bool deviceConnected = false;      // For BLE Server
volatile bool dodgerInputReceived = false;    // When shooter receives dodger input via BLE
volatile bool notificationReceived = false;   // For BLE Client notifications
int receivedShooterChoice = 0;                  // Received shooter barrel (as int)

// --- BLE Objects for Shooter (Server) ---
BLEServer* pServer = nullptr;
BLEService* pService = nullptr;
BLECharacteristic* pCharacteristic = nullptr;

// --- BLE Objects for Dodger (Client) ---
BLERemoteCharacteristic* pRemoteCharacteristic = nullptr;
BLEClient* pClient = nullptr;

// --- UI Layout Constants (Assuming a 320x240 Screen) ---
const int screenWidth = 320;
const int screenHeight = 240;

// Role selection button dimensions
const int roleButtonY = 80;
const int roleButtonWidth = screenWidth / 2; // 160
const int roleButtonHeight = 80;

// Barrel button dimensions and positions
const int buttonWidth = 80;
const int buttonHeight = 50;
const int buttonSpacing = 20;
const int buttonY = 180;
const int button1X = 40;
const int button2X = button1X + buttonWidth + buttonSpacing;  // 40+80+20 = 140
const int button3X = button2X + buttonWidth + buttonSpacing;  // 140+80+20 = 240

// --- Touch Debounce ---
unsigned long lastTouchTime = 0;
const unsigned long touchDebounce = 300; // milliseconds

// --- Helper: Check if a point lies in a rectangle ---
static bool pointInRect(int px, int py, int rx, int ry, int rw, int rh) {
  return (px >= rx && px <= rx + rw && py >= ry && py <= ry + rh);
}

// --- Forward Declarations ---
void drawRoleSelectionScreen();
void drawGameScreen();
void drawGameOverScreen();
void resetGame();
void setupBLE_Server();
void setupBLE_Client();

// --- BLE Server Callback Classes ---
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("BLE: Client connected.");
  }
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("BLE: Client disconnected.");
    BLEDevice::startAdvertising();
  }
};

class MyCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    std::string rxValue = pCharacteristic->getValue();
    if (rxValue.length() > 0) {
      dodgerChoice = atoi(rxValue.c_str());
      dodgerInputReceived = true;
      Serial.print("BLE: Received dodger choice: ");
      Serial.println(dodgerChoice);
    }
  }
};

// --- BLE Client Notification Callback ---
static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic,
                           uint8_t* pData, size_t length, bool isNotify) {
  if (length > 0) {
    receivedShooterChoice = atoi((const char*)pData);
    notificationReceived = true;
    Serial.print("BLE: Notification received, shooter choice: ");
    Serial.println(receivedShooterChoice);
  }
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  // Set display rotation for proper orientation (adjust as needed).
  M5.Display.setRotation(0);
  M5.Display.fillScreen(BLACK);
  Serial.begin(115200);
  Serial.println("Setup: Starting system...");

  // Initialize touch.
  M5.Touch.begin(&M5.Display);

  // Draw role selection screen.
  drawRoleSelectionScreen();
  Serial.println("Setup: Role selection screen displayed. Touch left for Shooter, right for Dodger.");

  // Wait for user to select role using touch events.
  while (deviceRole == ROLE_UNDEFINED) {
    M5.update();
    if (M5.Touch.getCount() > 0) {
      if (millis() - lastTouchTime < touchDebounce) {
        continue;  // Allowed here because we're in a while loop.
      }
      lastTouchTime = millis();
      auto pos = M5.Touch.getDetail(0);  // Get first touch detail
      int tx = pos.x;
      int ty = pos.y;
      Serial.print("Role selection touch: x=");
      Serial.print(tx);
      Serial.print(", y=");
      Serial.println(ty);
      if (pointInRect(tx, ty, 0, roleButtonY, roleButtonWidth, roleButtonHeight)) {
        deviceRole = ROLE_SHOOTER;
        Serial.println("Role selected: SHOOTER");
      } else if (pointInRect(tx, ty, roleButtonWidth, roleButtonY, roleButtonWidth, roleButtonHeight)) {
        deviceRole = ROLE_DODGER;
        Serial.println("Role selected: DODGER");
      }
    }
  }
  
  // Clear screen and show selected role.
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(2);
  if (deviceRole == ROLE_SHOOTER) {
    M5.Display.drawCentreString("Shooter Mode", screenWidth / 2, 20, 2);
    setupBLE_Server();
    shooterState = SHOOTER_WAIT_DODGER;
  } else {
    M5.Display.drawCentreString("Dodger Mode", screenWidth / 2, 20, 2);
    setupBLE_Client();
    dodgerState = DODGER_WAIT_INPUT;
  }
  
  delay(1000);
  resetGame();
  Serial.println("Setup complete. Entering main loop.");
}

void loop() {
  M5.update();
  
  // --- Shooter Mode Logic ---
  if (deviceRole == ROLE_SHOOTER) {
    if (shooterState == SHOOTER_WAIT_DODGER) {
      // Waiting for dodger's barrel selection via BLE.
      drawGameScreen(); // displays "Waiting for dodger..."
      if (dodgerInputReceived) {
        shooterState = SHOOTER_WAIT_INPUT;
        dodgerInputReceived = false;
        Serial.println("Shooter: Dodger input received; now waiting for shooter input.");
      }
    }
    else if (shooterState == SHOOTER_WAIT_INPUT) {
      drawGameScreen(); // displays "Select barrel to shoot"
      if (M5.Touch.getCount() > 0) {
        if (millis() - lastTouchTime >= touchDebounce) {
          lastTouchTime = millis();
          auto pos = M5.Touch.getDetail(0);
          int tx = pos.x, ty = pos.y;
          Serial.print("Shooter button touch: x=");
          Serial.print(tx);
          Serial.print(", y=");
          Serial.println(ty);
          if (pointInRect(tx, ty, button1X, buttonY, buttonWidth, buttonHeight)) {
            shooterChoice = 1;
          } else if (pointInRect(tx, ty, button2X, buttonY, buttonWidth, buttonHeight)) {
            shooterChoice = 2;
          } else if (pointInRect(tx, ty, button3X, buttonY, buttonWidth, buttonHeight)) {
            shooterChoice = 3;
          }
          if (shooterChoice >= 1 && shooterChoice <= 3) {
            Serial.print("Shooter selected barrel: ");
            Serial.println(shooterChoice);
            if (shooterChoice != dodgerChoice) {
              roundResultSafe = false;
              gameOver = true;
              Serial.println("Result: Dodger HIT!");
            } else {
              roundResultSafe = true;
              Serial.println("Result: Round Safe.");
            }
            if (deviceConnected) {
              char buf[4];
              sprintf(buf, "%d", shooterChoice);
              pCharacteristic->setValue(buf);
              pCharacteristic->notify();
              Serial.print("BLE: Notified dodger with shooter choice: ");
              Serial.println(shooterChoice);
            } else {
              Serial.println("BLE Warning: No device connected!");
            }
            shooterState = SHOOTER_SHOW_RESULT;
          }
        }
      }
    }
    else if (shooterState == SHOOTER_SHOW_RESULT) {
      drawGameScreen();
      delay(1500);
      if (gameOver || (roundNumber >= MAX_ROUNDS && roundResultSafe)) {
        shooterState = SHOOTER_GAME_OVER;
        Serial.println("Shooter: Game over.");
      } else {
        roundNumber++;
        shooterState = SHOOTER_WAIT_DODGER;
        Serial.print("Shooter: Advancing to round ");
        Serial.println(roundNumber);
      }
    }
    else if (shooterState == SHOOTER_GAME_OVER) {
      drawGameOverScreen();
      if (M5.Touch.getCount() > 0) {
        if (millis() - lastTouchTime >= touchDebounce) {
          lastTouchTime = millis();
          auto pos = M5.Touch.getDetail(0);
          int tx = pos.x, ty = pos.y;
          Serial.print("Shooter restart touch: x=");
          Serial.print(tx);
          Serial.print(", y=");
          Serial.println(ty);
          if (pointInRect(tx, ty, screenWidth / 2 - 60, 120, 120, 40)) {
            Serial.println("Shooter: Restart pressed.");
            resetGame();
            shooterState = SHOOTER_WAIT_DODGER;
          }
        }
      }
    }
  }
  // --- Dodger Mode Logic ---
  else if (deviceRole == ROLE_DODGER) {
    if (dodgerState == DODGER_WAIT_INPUT) {
      drawGameScreen();
      if (M5.Touch.getCount() > 0) {
        if (millis() - lastTouchTime >= touchDebounce) {
          lastTouchTime = millis();
          auto pos = M5.Touch.getDetail(0);
          int tx = pos.x, ty = pos.y;
          Serial.print("Dodger button touch: x=");
          Serial.print(tx);
          Serial.print(", y=");
          Serial.println(ty);
          if (pointInRect(tx, ty, button1X, buttonY, buttonWidth, buttonHeight)) {
            dodgerChoice = 1;
          } else if (pointInRect(tx, ty, button2X, buttonY, buttonWidth, buttonHeight)) {
            dodgerChoice = 2;
          } else if (pointInRect(tx, ty, button3X, buttonY, buttonWidth, buttonHeight)) {
            dodgerChoice = 3;
          }
          if (dodgerChoice >= 1 && dodgerChoice <= 3) {
            Serial.print("Dodger selected barrel: ");
            Serial.println(dodgerChoice);
            if (pRemoteCharacteristic != nullptr) {
              char buf[4];
              sprintf(buf, "%d", dodgerChoice);
              pRemoteCharacteristic->writeValue(std::string(buf));
              Serial.print("BLE: Sent dodger choice: ");
              Serial.println(dodgerChoice);
            } else {
              Serial.println("BLE Warning: Remote characteristic not found!");
            }
            dodgerState = DODGER_WAIT_SHOT;
          }
        }
      }
    }
    else if (dodgerState == DODGER_WAIT_SHOT) {
      drawGameScreen();
      if (notificationReceived) {
        notificationReceived = false;
        Serial.print("Dodger: Received shooter choice: ");
        Serial.println(receivedShooterChoice);
        if (receivedShooterChoice != dodgerChoice) {
          roundResultSafe = false;
          gameOver = true;
          Serial.println("Dodger: You were hit!");
        } else {
          roundResultSafe = true;
          Serial.println("Dodger: Round safe.");
        }
        dodgerState = DODGER_SHOW_RESULT;
      }
    }
    else if (dodgerState == DODGER_SHOW_RESULT) {
      drawGameScreen();
      delay(1500);
      if (gameOver || (roundNumber >= MAX_ROUNDS && roundResultSafe)) {
        dodgerState = DODGER_GAME_OVER;
        Serial.println("Dodger: Game over.");
      } else {
        roundNumber++;
        dodgerState = DODGER_WAIT_INPUT;
        Serial.print("Dodger: Advancing to round ");
        Serial.println(roundNumber);
      }
    }
    else if (dodgerState == DODGER_GAME_OVER) {
      drawGameOverScreen();
      if (M5.Touch.getCount() > 0) {
        if (millis() - lastTouchTime >= touchDebounce) {
          lastTouchTime = millis();
          auto pos = M5.Touch.getDetail(0);
          int tx = pos.x, ty = pos.y;
          Serial.print("Dodger restart touch: x=");
          Serial.print(tx);
          Serial.print(", y=");
          Serial.println(ty);
          if (pointInRect(tx, ty, screenWidth / 2 - 60, 120, 120, 40)) {
            Serial.println("Dodger: Restart pressed.");
            resetGame();
            dodgerState = DODGER_WAIT_INPUT;
          }
        }
      }
    }
  }
}

// --- UI Drawing Functions ---
void drawRoleSelectionScreen() {
  M5.Display.setRotation(1);  // Landscape mode.
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(2);
  // Left half: Shooter button.
  M5.Display.fillRect(0, roleButtonY, roleButtonWidth, roleButtonHeight, BLUE);
  M5.Display.drawRect(0, roleButtonY, roleButtonWidth, roleButtonHeight, TFT_WHITE);
  M5.Display.drawCentreString("Shooter", roleButtonWidth / 2, roleButtonY + 25, 2);
  // Right half: Dodger button.
  M5.Display.fillRect(roleButtonWidth, roleButtonY, roleButtonWidth, roleButtonHeight, GREEN);
  M5.Display.drawRect(roleButtonWidth, roleButtonY, roleButtonWidth, roleButtonHeight, TFT_WHITE);
  M5.Display.drawCentreString("Dodger", roleButtonWidth + roleButtonWidth / 2, roleButtonY + 25, 2);
  Serial.println("UI: Role selection screen drawn.");
}

void drawGameScreen() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(2);
  
  String roundStr = "Round: " + String(roundNumber) + " / " + String(MAX_ROUNDS);
  M5.Display.drawCentreString(roundStr, screenWidth / 2, 10, 2);
  
  if (deviceRole == ROLE_SHOOTER) {
    if (shooterState == SHOOTER_WAIT_DODGER) {
      M5.Display.drawCentreString("Waiting for dodger...", screenWidth / 2, 50, 2);
    } else if (shooterState == SHOOTER_WAIT_INPUT) {
      M5.Display.drawCentreString("Select barrel to shoot", screenWidth / 2, 50, 2);
    } else if (shooterState == SHOOTER_SHOW_RESULT) {
      if (roundResultSafe)
        M5.Display.drawCentreString("Round Safe", screenWidth / 2, 50, 2);
      else
        M5.Display.drawCentreString("Dodger Hit!", screenWidth / 2, 50, 2);
    }
  } else { // Dodger mode.
    if (dodgerState == DODGER_WAIT_INPUT) {
      M5.Display.drawCentreString("Select barrel to hide", screenWidth / 2, 50, 2);
    } else if (dodgerState == DODGER_WAIT_SHOT) {
      M5.Display.drawCentreString("Waiting for shot...", screenWidth / 2, 50, 2);
    } else if (dodgerState == DODGER_SHOW_RESULT) {
      if (roundResultSafe)
        M5.Display.drawCentreString("Safe!", screenWidth / 2, 50, 2);
      else
        M5.Display.drawCentreString("You Were Hit!", screenWidth / 2, 50, 2);
    }
  }
  
  // Draw barrel selection buttons.
  M5.Display.fillRect(button1X, buttonY, buttonWidth, buttonHeight, DARKGREY);
  M5.Display.drawRect(button1X, buttonY, buttonWidth, buttonHeight, TFT_WHITE);
  M5.Display.drawCentreString("Barrel1", button1X + buttonWidth / 2, buttonY + 15, 2);
  
  M5.Display.fillRect(button2X, buttonY, buttonWidth, buttonHeight, DARKGREY);
  M5.Display.drawRect(button2X, buttonY, buttonWidth, buttonHeight, TFT_WHITE);
  M5.Display.drawCentreString("Barrel2", button2X + buttonWidth / 2, buttonY + 15, 2);
  
  M5.Display.fillRect(button3X, buttonY, buttonWidth, buttonHeight, DARKGREY);
  M5.Display.drawRect(button3X, buttonY, buttonWidth, buttonHeight, TFT_WHITE);
  M5.Display.drawCentreString("Barrel3", button3X + buttonWidth / 2, buttonY + 15, 2);
}

void drawGameOverScreen() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(2);
  String result;
  if (deviceRole == ROLE_SHOOTER) {
    result = (!roundResultSafe) ? "You Win!" : "You Lose!";
  } else {
    result = (!roundResultSafe) ? "You Lose!" : "You Win!";
  }
  M5.Display.drawCentreString("Game Over", screenWidth / 2, 50, 2);
  M5.Display.drawCentreString(result, screenWidth / 2, 80, 2);
  M5.Display.fillRect(screenWidth / 2 - 60, 120, 120, 40, BLUE);
  M5.Display.drawRect(screenWidth / 2 - 60, 120, 120, 40, TFT_WHITE);
  M5.Display.drawCentreString("Restart", screenWidth / 2, 130, 2);
  Serial.println("UI: Game over screen drawn.");
}

void resetGame() {
  roundNumber = 1;
  gameOver = false;
  roundResultSafe = false;
  dodgerChoice = 0;
  shooterChoice = 0;
  dodgerInputReceived = false;
  notificationReceived = false;
  receivedShooterChoice = 0;
  M5.Display.fillScreen(BLACK);
  Serial.println("Game reset.");
}

// --- BLE Setup Functions ---
void setupBLE_Server() {
  BLEDevice::init("M5Core2_Shooter");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ |
                      BLECharacteristic::PROPERTY_WRITE |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  pCharacteristic->setCallbacks(new MyCharacteristicCallbacks());
  pCharacteristic->setValue("0");
  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("BLE Server: Advertising started.");
}

void setupBLE_Client() {
  BLEDevice::init("");
  pClient = BLEDevice::createClient();
  Serial.println("BLE Client: Created. Scanning for server...");
  
  BLEScan* pBLEScan = BLEDevice::getScan();
  BLEAdvertisedDevice* myDevice = nullptr;
  while (true) {
    BLEScanResults foundDevices = pBLEScan->start(5);
    Serial.print("BLE Client: Found devices: ");
    Serial.println(foundDevices.getCount());
    for (int i = 0; i < foundDevices.getCount(); i++) {
      BLEAdvertisedDevice device = foundDevices.getDevice(i);
      if (device.haveServiceUUID() && device.isAdvertisingService(BLEUUID(SERVICE_UUID))) {
        myDevice = new BLEAdvertisedDevice(device);
        break;
      }
    }
    if (myDevice != nullptr) break;
    Serial.println("BLE Client: Server not found, rescanning...");
  }
  
  Serial.print("BLE Client: Connecting to ");
  Serial.println(myDevice->getAddress().toString().c_str());
  pClient->connect(myDevice);
  Serial.println("BLE Client: Connected to server.");
  
  BLERemoteService* pRemoteService = pClient->getService(BLEUUID(SERVICE_UUID));
  if (pRemoteService == nullptr) {
    Serial.println("BLE Client Error: Failed to find service.");
    return;
  }
  
  pRemoteCharacteristic = pRemoteService->getCharacteristic(BLEUUID(CHARACTERISTIC_UUID));
  if (pRemoteCharacteristic == nullptr) {
    Serial.println("BLE Client Error: Failed to find characteristic.");
    return;
  }
  
  if (pRemoteCharacteristic->canNotify())
    pRemoteCharacteristic->registerForNotify(notifyCallback);
  
  Serial.println("BLE Client: Setup complete.");
}
