#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <mcp_can.h>

#define CMD_SET_CONFIG   0x10
#define CMD_GET_ENERGY   0x11
#define CMD_GIVE_ENERGY  0x12
#define CMD_QUERY_STATUS 0x11
#define CMD_STOP_EFFECT  0x12
#define CMD_PING         0xF0

#define REPORT_STARTED   0xA0
#define REPORT_FINISHED  0xA1
#define REPORT_BUSY      0xA2
#define REPORT_INVALID   0xA3
#define REPORT_STOPPED   0xA4
#define REPORT_PONG      0xAF

LiquidCrystal_I2C lcd(0x27, 16, 2);

const uint8_t BTN_UP_PIN    = 2;
const uint8_t BTN_DOWN_PIN  = 3;
const uint8_t BTN_SELECT_PIN= 4;
const uint8_t BTN_BACK_PIN  = 5;

const uint8_t btnPins[4] = {BTN_UP_PIN, BTN_DOWN_PIN, BTN_SELECT_PIN, BTN_BACK_PIN};
uint8_t btnPrev[4];
unsigned long btnTime[4];

const uint8_t CAN_CS_PIN  = 17;
const uint8_t CAN_INT_PIN = 22;
MCP_CAN CAN(CAN_CS_PIN);

const uint8_t allowedGpio[] = {2, 3, 4, 5};
const uint8_t allowedCount = sizeof(allowedGpio) / sizeof(allowedGpio[0]);

const char* menuItems[] = {
  "Proverka CAN",
  "Poluchit energ.",
  "Otdat energiyu",
  "Ozhidat otvet",
  "Otpravit komand.",
  "Nastroit nody"
};
const uint8_t MENU_COUNT = sizeof(menuItems)/sizeof(menuItems[0]);
uint8_t currentMenu = 0;
uint8_t lastMenu = 0xFF;
String lastLine1 = "";
String lastLine2 = "";

bool inConfigEditor = false;
bool inCmdSender   = false;

struct NodeConfig {
  uint8_t gpio;
  uint16_t leds;
  uint16_t duration;
  uint8_t effectType;
  uint8_t windowSize;
  uint8_t stepDelay;
};
NodeConfig editCfg;
uint8_t editNodeId = 1;
uint8_t configStep = 0;

uint8_t cmdNodeId = 1;
uint8_t cmdType   = CMD_GET_ENERGY;
uint8_t cmdStep   = 0;

bool gpioAllowed(uint8_t pin) {
  for (uint8_t i = 0; i < allowedCount; i++) {
    if (allowedGpio[i] == pin) return true;
  }
  return false;
}

uint8_t nextAllowedGpio(uint8_t current, int8_t dir) {
  int idx = 0;
  for (uint8_t i = 0; i < allowedCount; i++) {
    if (allowedGpio[i] == current) { idx = i; break; }
  }
  idx += dir;
  if (idx < 0) idx = allowedCount - 1;
  if (idx >= (int)allowedCount) idx = 0;
  return allowedGpio[idx];
}

bool buttonPressed(uint8_t idx) {
  uint8_t pin = btnPins[idx];
  bool cur = digitalRead(pin);
  unsigned long now = millis();
  bool pressed = false;
  if (btnPrev[idx] != cur) {
    btnPrev[idx] = cur;
    if (cur == LOW && now - btnTime[idx] > 50) {
      pressed = true;
      btnTime[idx] = now;
    }
  }
  return pressed;
}

void updateLCD(const String &l1, const String &l2) {
  if (l1 != lastLine1 || l2 != lastLine2) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(l1);
    lcd.setCursor(0, 1);
    lcd.print(l2);
    lastLine1 = l1;
    lastLine2 = l2;
  }
}

void drawMenu() {
  updateLCD(menuItems[currentMenu], "UP/DN SEL");
}

void sendBroadcastCommand(uint8_t cmd) {
  updateLCD("Otpravka...", "");
  Serial.print("[ИНФО] [CAN] Broadcast cmd ");
  Serial.println(cmd, HEX);
  CAN.sendMsgBuf(0x100, 0, 1, &cmd);
}

void sendConfig(uint8_t nodeId, const NodeConfig &cfg) {
  uint16_t addr = 0x110 + nodeId;
  uint8_t payload[10] = {
    CMD_SET_CONFIG,
    cfg.gpio,
    lowByte(cfg.leds), highByte(cfg.leds),
    lowByte(cfg.duration), highByte(cfg.duration),
    cfg.effectType,
    cfg.windowSize,
    cfg.stepDelay,
    0x00
  };
  updateLCD("Otpravka cfg", "");
  Serial.print("[ИНФО] Отправка конфигурации -> 0x"); Serial.print(addr, HEX); Serial.print(" [");
  for (uint8_t i=0;i<10;i++) { Serial.print(payload[i], HEX); Serial.print(' '); }
  Serial.println("]");
  CAN.sendMsgBuf(addr, 0, 10, payload);
}

void showMessage(const String &l1, const String &l2, unsigned long t=1500) {
  updateLCD(l1, l2);
  delay(t);
  drawMenu();
  lastMenu = currentMenu;
}

void handleCanReceive() {
  if (CAN_MSGAVAIL == CAN.checkReceive()) {
    unsigned long rxId;
    uint8_t ext;
    uint8_t len;
    uint8_t buf[8];
    CAN.readMsgBuf(&rxId, &ext, &len, buf);

    Serial.println("[ИНФО] Получено CAN-сообщение");
    Serial.print("[ИНФО] RX 0x"); Serial.print(rxId, HEX);
    Serial.print(" len="); Serial.print(len);
    Serial.print(" data=");
    for (uint8_t i=0;i<len;i++) { Serial.print(buf[i], HEX); Serial.print(' '); }
    Serial.println();

    updateLCD(String("Rx 0x") + String(rxId, HEX), String("B0:") + String(buf[0], HEX));
    delay(2000);
    drawMenu();
    lastMenu = currentMenu;
  }
}

void handleCmdSender() {
  switch(cmdStep) {
    case 0:
      updateLCD(String("Node ID: ") + String(cmdNodeId, HEX), "UP/DN SEL");
      if (buttonPressed(0)) { cmdNodeId++; if (cmdNodeId>15) cmdNodeId=1; }
      if (buttonPressed(1)) { if (cmdNodeId<=1) cmdNodeId=15; else cmdNodeId--; }
      if (buttonPressed(2)) { cmdStep=1; }
      if (buttonPressed(3)) { inCmdSender=false; drawMenu(); }
      break;
    case 1:
      updateLCD(String("Cmd:") + (cmdType==CMD_GET_ENERGY?"GET":"GIVE"), "UP/DN SEL");
      if (buttonPressed(0) || buttonPressed(1)) {
        cmdType = (cmdType==CMD_GET_ENERGY)?CMD_GIVE_ENERGY:CMD_GET_ENERGY;
      }
      if (buttonPressed(2)) { cmdStep=2; }
      if (buttonPressed(3)) { cmdStep=0; }
      break;
    case 2:
      updateLCD("Send?", "SEL=Yes BACK=No");
      if (buttonPressed(2)) {
        sendBroadcastCommand(cmdType);
        inCmdSender=false;
        showMessage("Cmd sent","Node "+String(cmdNodeId));
      }
      if (buttonPressed(3)) { inCmdSender=false; drawMenu(); }
      break;
  }
}

void handleConfigEditor() {
  switch(configStep) {
    case 0:
      updateLCD(String("ID: ") + String(editNodeId, HEX), "UP/DN SEL");
      if (buttonPressed(0)) { editNodeId++; if (editNodeId>15) editNodeId=1; }
      if (buttonPressed(1)) { if (editNodeId<=1) editNodeId=15; else editNodeId--; }
      if (buttonPressed(2)) { configStep=1; }
      if (buttonPressed(3)) { inConfigEditor=false; drawMenu(); }
      break;
    case 1:
      updateLCD(String("GPIO: ") + String(editCfg.gpio), "UP/DN SEL");
      if (buttonPressed(0)) editCfg.gpio = nextAllowedGpio(editCfg.gpio, 1);
      if (buttonPressed(1)) editCfg.gpio = nextAllowedGpio(editCfg.gpio, -1);
      if (buttonPressed(2)) configStep=2;
      if (buttonPressed(3)) configStep=0;
      break;
    case 2:
      updateLCD(String("LEDs: ") + String(editCfg.leds), "UP/DN SEL");
      if (buttonPressed(0)) { editCfg.leds+=10; if(editCfg.leds>1000) editCfg.leds=10; }
      if (buttonPressed(1)) { if(editCfg.leds<=10) editCfg.leds=1000; else editCfg.leds-=10; }
      if (buttonPressed(2)) configStep=3;
      if (buttonPressed(3)) configStep=1;
      break;
    case 3:
      updateLCD(String("Dur: ") + String(editCfg.duration), "UP/DN SEL");
      if (buttonPressed(0)) { editCfg.duration+=500; if(editCfg.duration>10000) editCfg.duration=500; }
      if (buttonPressed(1)) { if(editCfg.duration<=500) editCfg.duration=10000; else editCfg.duration-=500; }
      if (buttonPressed(2)) configStep=4;
      if (buttonPressed(3)) configStep=2;
      break;
    case 4:
      updateLCD(String("Effect: ") + String(editCfg.effectType), "UP/DN SEL");
      if (buttonPressed(0)) { editCfg.effectType++; if(editCfg.effectType>10) editCfg.effectType=1; }
      if (buttonPressed(1)) { if(editCfg.effectType<=1) editCfg.effectType=10; else editCfg.effectType--; }
      if (buttonPressed(2)) configStep=5;
      if (buttonPressed(3)) configStep=3;
      break;
    case 5:
      updateLCD(String("Window: ") + String(editCfg.windowSize), "UP/DN SEL");
      if (buttonPressed(0)) { editCfg.windowSize++; if(editCfg.windowSize>150) editCfg.windowSize=1; }
      if (buttonPressed(1)) { if(editCfg.windowSize<=1) editCfg.windowSize=150; else editCfg.windowSize--; }
      if (buttonPressed(2)) configStep=6;
      if (buttonPressed(3)) configStep=4;
      break;
    case 6:
      updateLCD(String("Delay: ") + String(editCfg.stepDelay), "UP/DN SEL");
      if (buttonPressed(0)) { editCfg.stepDelay+=5; if(editCfg.stepDelay>255) editCfg.stepDelay=5; }
      if (buttonPressed(1)) { if(editCfg.stepDelay<=5) editCfg.stepDelay=255; else editCfg.stepDelay-=5; }
      if (buttonPressed(2)) configStep=7;
      if (buttonPressed(3)) configStep=5;
      break;
    case 7:
      updateLCD("Send cfg?", "SEL=Yes BACK=No");
      if (buttonPressed(2)) {
        sendConfig(editNodeId, editCfg);
        inConfigEditor=false;
        showMessage("Cfg sent","Node "+String(editNodeId));
      }
      if (buttonPressed(3)) { inConfigEditor=false; drawMenu(); }
      break;
  }
}

void initButtons() {
  for (uint8_t i = 0; i < 4; i++) {
    pinMode(btnPins[i], INPUT_PULLUP);
    btnPrev[i] = digitalRead(btnPins[i]);
  }
}

void initLCD() {
  Wire.setSDA(0);
  Wire.setSCL(1);
  Wire.begin();
  lcd.begin(16, 2);
  lcd.backlight();
}

void initCAN() {
  SPI.begin();
  updateLCD("Init CAN...", "");
  Serial.println("Init CAN...");
  while (CAN_OK != CAN.begin(MCP_ANY, CAN_500KBPS, MCP_8MHZ)) {
    Serial.println("Oshibka CAN");
    updateLCD("Oshibka CAN", "");
    delay(500);
  }
  CAN.setMode(MCP_NORMAL);
  pinMode(CAN_INT_PIN, INPUT);
  Serial.println("CAN inizializirovan");
  updateLCD("CAN init", "");
  delay(1000);
}

void setup() {
  Serial.begin(115000);
  initButtons();
  initLCD();
  initCAN();
  drawMenu();
  lastMenu = currentMenu;
  editCfg = {2, 10, 500, 1, 1, 5};
}

void loop() {
  handleCanReceive();
  if (inConfigEditor) { handleConfigEditor(); return; }
  if (inCmdSender)    { handleCmdSender(); return; }
  if (buttonPressed(0)) currentMenu = (currentMenu==0) ? MENU_COUNT-1 : currentMenu-1;
  if (buttonPressed(1)) currentMenu = (currentMenu+1)%MENU_COUNT;
  if (currentMenu != lastMenu) {
    drawMenu();
    lastMenu = currentMenu;
  }

  static bool commandSent = false;

  if (buttonPressed(2) && !commandSent) {
    commandSent = true;
    switch(currentMenu) {
      case 0: sendBroadcastCommand(CMD_PING); showMessage("Sent","PING"); break;
      case 1: sendBroadcastCommand(CMD_GET_ENERGY); showMessage("Sent","GET"); break;
      case 2: sendBroadcastCommand(CMD_GIVE_ENERGY); showMessage("Sent","GIVE"); break;
      case 3:
        updateLCD("Ozhidanie...", "BACK-exit");
        while(true) {
          handleCanReceive();
          if (buttonPressed(3)) break;
        }
        drawMenu(); lastMenu = currentMenu; break;
      case 4: inCmdSender=true; cmdStep=0; break;
      case 5: inConfigEditor=true; configStep=0; break;
    }
  }
  if (!buttonPressed(2)) {
    commandSent = false;
  }
}
