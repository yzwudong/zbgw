#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <FS.h>
#include <LittleFS.h>
#include <ETH.h>
#include <CCTools.h>
#include <esp_task_wdt.h>
#include <CronAlarms.h>
// #include <Husarnet.h> //not available now

#include "config.h"
#include "web.h"
#include "log.h"
#include "etc.h"
#include "const/zones.h"
// #include "const/hw.h"
#include "zb.h"

#include <WireGuard-ESP32.h>
static WireGuard wg;

extern BrdConfigStruct brdConfigs[BOARD_CFG_CNT];
extern EthConfig ethConfigs[ETH_CFG_CNT];
extern ZbConfig zbConfigs[ZB_CFG_CNT];
extern MistConfig mistConfigs[MIST_CFG_CNT];

extern struct ThisConfigStruct hwConfig;

extern struct SystemConfigStruct systemCfg;
extern struct NetworkConfigStruct networkCfg;
extern struct VpnConfigStruct vpnCfg;
extern struct MqttConfigStruct mqttCfg;

extern struct SysVarsStruct vars;

extern LEDControl ledControl;

extern CCTools CCTool;

const char *coordMode = "coordMode"; // coordMode node name ?? not name but text field with mode
// const char *prevCoordMode = "prevCoordMode"; // prevCoordMode node name ?? not name but text field with mode

const char *configFileSystem = "/config/system.json";
const char *configFileWifi = "/config/configWifi.json";
const char *configFileEther = "/config/configEther.json";
const char *configFileGeneral = "/config/configGeneral.json";
const char *configFileSecurity = "/config/configSecurity.json";
const char *configFileSerial = "/config/configSerial.json";
const char *configFileMqtt = "/config/configMqtt.json";
const char *configFileWg = "/config/configWg.json";
const char *configFileHw = "/configHw.json";

#include "mbedtls/md.h"

String sha1(String payloadStr)
{
  const char *payload = payloadStr.c_str();

  int size = 20;

  byte shaResult[size];

  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA1;

  const size_t payloadLength = strlen(payload);

  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char *)payload, payloadLength);
  mbedtls_md_finish(&ctx, shaResult);
  mbedtls_md_free(&ctx);

  String hashStr = "";

  for (uint16_t i = 0; i < size; i++)
  {
    String hex = String(shaResult[i], HEX);
    if (hex.length() < 2)
    {
      hex = "0" + hex;
    }
    hashStr += hex;
  }

  return hashStr;
}

#include <OneWire.h>
#include <DallasTemperature.h>

OneWire *oneWire = nullptr;
DallasTemperature *sensor = nullptr;

int check1wire()
{
  int pin = -1;
  vars.oneWireIs = false;
  if (hwConfig.eth.mdcPin != 33 && hwConfig.eth.mdiPin != 33 && hwConfig.eth.pwrPin != 33)
  {
    if (hwConfig.zb.rxPin != 33 && hwConfig.zb.txPin != 33 && hwConfig.zb.bslPin != 33 && hwConfig.zb.rstPin != 33)
    {
      if (hwConfig.mist.btnPin != 33 && hwConfig.mist.uartSelPin != 33 && hwConfig.mist.ledModePin != 33 && hwConfig.mist.ledPwrPin != 33)
      {
        pin = 33;
        vars.oneWireIs = true;
      }
    }
  }
  return pin;
}

void setup1wire(int pin)
{
  if (oneWire != nullptr)
  {
    delete oneWire;
  }
  if (sensor != nullptr)
  {
    delete sensor;
  }

  oneWire = new OneWire(pin);
  sensor = new DallasTemperature(oneWire);

  sensor->begin();
  get1wire();
}

float get1wire()
{
  if (sensor == nullptr)
  {
    LOGW("1w not init");
    return -127.0;
  }
  if (millis() - vars.last1wAsk > 5000)
  {
    sensor->requestTemperatures();
    vars.temp1w = sensor->getTempCByIndex(0);
    LOGD("Temp is %f", vars.temp1w);
    vars.last1wAsk = millis();
  }
  if (vars.temp1w == -127)
  {
    vars.oneWireIs = false;
  }
  return vars.temp1w;
}

void getReadableTime(String &readableTime, unsigned long beginTime)
{
  unsigned long currentMillis;
  unsigned long seconds;
  unsigned long minutes;
  unsigned long hours;
  unsigned long days;
  currentMillis = millis() - beginTime;
  seconds = currentMillis / 1000;
  minutes = seconds / 60;
  hours = minutes / 60;
  days = hours / 24;
  currentMillis %= 1000;
  seconds %= 60;
  minutes %= 60;
  hours %= 24;

  readableTime = String(days) + " d ";

  if (hours < 10)
  {
    readableTime += "0";
  }
  readableTime += String(hours) + ":";

  if (minutes < 10)
  {
    readableTime += "0";
  }
  readableTime += String(minutes) + ":";

  if (seconds < 10)
  {
    readableTime += "0";
  }
  readableTime += String(seconds) + "";
}

float readTemperature(bool clear)
{
  if (clear)
  {
    return (temprature_sens_read() - 32) / 1.8;
  }
  else
  {
    return (temprature_sens_read() - 32) / 1.8 - systemCfg.tempOffset;
  }
}

float getCPUtemp(bool clear)
{
  float CPUtemp = 0.0;
  if (WiFi.getMode() == WIFI_MODE_NULL || WiFi.getMode() == WIFI_OFF)
  {
    WiFi.mode(WIFI_STA); // enable wifi to enable temp sensor
    CPUtemp = readTemperature(clear);
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF); // disable wifi
  }
  else
  {
    CPUtemp = readTemperature(clear);
  }
  return CPUtemp;
}

void zigbeeRouterRejoin()
{
  printLogMsg("Router rejoin begin");
  CCTool.routerRejoin();
  printLogMsg("Router in join mode!");
}

void zigbeeEnableBSL()
{
  printLogMsg("ZB enable BSL");
  CCTool.enterBSL();
  printLogMsg("Now you can flash CC2652!");
  if (systemCfg.workMode == WORK_MODE_USB)
  {
    Serial.updateBaudRate(500000);
    Serial2.updateBaudRate(500000);
  }
}

void zigbeeRestart()
{
  printLogMsg("ZB RST begin");
  CCTool.restart();
  printLogMsg("ZB restart was done");
  if (systemCfg.workMode == WORK_MODE_USB)
  {
    Serial.updateBaudRate(systemCfg.serialSpeed);
    Serial2.updateBaudRate(systemCfg.serialSpeed);
  }
}

void usbModeSet(usbMode mode)
{
  // if (vars.hwUartSelIs)
  //{
  // String modeStr = (mode == ZIGBEE) ? "ZIGBEE" : "ESP";
  bool pinValue = (mode == ZIGBEE) ? HIGH : LOW;
  // String msg = "Switched USB to " + modeStr + "";
  // printLogMsg(msg);

  if (mode == ZIGBEE)
  {
    Serial.updateBaudRate(systemCfg.serialSpeed);
  }
  else
  {
    Serial.updateBaudRate(115200);
  }
 digitalWrite(hwConfig.mist.uartSelPin, pinValue);
  if (pinValue)
  {
    ledControl.modeLED.mode = LED_ON;
  }
  else
  {
    ledControl.modeLED.mode = LED_OFF;
  }
  //}
  // else
  //{
  //  LOGD("NO vars.hwUartSelIs");
  //}
}

void getDeviceID(char *arr)
{
  uint64_t mac = ESP.getEfuseMac(); // Retrieve the MAC address
  uint8_t a = 0;                    // Initialize variables to store the results
  uint8_t b = 0;

  // Apply XOR operation to each byte of the MAC address to obtain unique values for a and b
  a ^= (mac >> (8 * 0)) & 0xFF;
  a ^= (mac >> (8 * 1)) & 0xFF;
  a ^= (mac >> (8 * 2)) & 0xFF;
  a ^= (mac >> (8 * 3)) & 0xFF;
  b ^= (mac >> (8 * 4)) & 0xFF;
  b ^= (mac >> (8 * 5)) & 0xFF;

  char buf[MAX_DEV_ID_LONG];

  // Format a and b into buf as hexadecimal values, ensuring two-digit representation for each
  sprintf(buf, "%02x%02x", a, b);

  // Convert each character in buf to upper case
  for (uint8_t cnt = 0; buf[cnt] != '\0'; cnt++)
  {
    buf[cnt] = toupper(buf[cnt]);
  }

  // Form the final string including the board name and the processed MAC address

  // sprintf(arr, "%s-%s", hwConfig.board, buf);

  String devicePref = "XZG"; // hwConfig.board
  snprintf(arr, MAX_DEV_ID_LONG, "%s-%s", devicePref, buf);
}

void writeDefaultConfig(const char *path, DynamicJsonDocument &doc)
{
  LOGD("Write defaults to %s", path);
  serializeJsonPretty(doc, Serial);
  File configFile = LittleFS.open(path, FILE_WRITE);
  if (!configFile)
  {
    LOGD("Failed Write");
    if (LittleFS.mkdir(path))
    {
      LOGD("Config dir created");
      delay(500);
      ESP.restart();
    }
    else
    {
      LOGD("mkdir failed");
    }
    // return false;
  }
  else
  {
    serializeJsonPretty(doc, configFile);
  }
  configFile.close();
}

void factoryReset()
{

  LOGD("start");

  ledControl.powerLED.mode = LED_FLASH_3Hz;
  ledControl.modeLED.mode = LED_FLASH_3Hz;

  for (uint8_t i = 0; i < TIMEOUT_FACTORY_RESET; i++)
  {
    LOGD("%d, sec", TIMEOUT_FACTORY_RESET - i);
    delay(1000);
  }

  LittleFS.format();
  if (!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED, "/lfs2", 10)) // change to format anyway
  {
    LOGD("Error with LITTLEFS");
  }

  LittleFS.remove(configFileSerial);
  LittleFS.remove(configFileSecurity);
  LittleFS.remove(configFileGeneral);
  LittleFS.remove(configFileEther);
  LittleFS.remove(configFileWifi);
  LittleFS.remove(configFileSystem);
  LittleFS.remove(configFileWg);
  LittleFS.remove(configFileHw);
  LittleFS.remove(configFileMqtt);
  LOGD("FS Done");
  eraseNVS();
  LOGD("NVS Done");

  ledControl.powerLED.mode = LED_OFF;
  ledControl.modeLED.mode = LED_OFF;
  delay(500);
  ESP.restart();
}

void setClock(void *pvParameters)
{
  checkDNS();
  configTime(0, 0, systemCfg.ntpServ1, systemCfg.ntpServ2);

  const time_t targetTime = 946684800; // 946684800 - is 01.01.2000 in timestamp

  LOGD("Waiting for NTP time sync");
  unsigned long startTryingTime = millis();

  time_t nowSecs = time(nullptr);

  // over 01.01.2000 or longer than 5 minutes
  while ((nowSecs < targetTime) && ((millis() - startTryingTime) < 300000))
  {
    delay(500);
    yield();
    nowSecs = time(nullptr);
  }

  struct tm timeinfo;
  if (localtime_r(&nowSecs, &timeinfo))
  {
    // LOGD("Current GMT time: %s", String(asctime(&timeinfo)).c_str());

    char *zoneToFind = const_cast<char *>(NTP_TIME_ZONE);
    if (systemCfg.timeZone)
    {
      zoneToFind = systemCfg.timeZone;
    }
    const char *gmtOffset = getGmtOffsetForZone(zoneToFind);

    String timezone = "EET-2EEST,M3.5.0/3,M10.5.0/4";

    if (gmtOffset != nullptr)
    {
      LOGD("GMT Offset for %s is %s", zoneToFind, gmtOffset);
      timezone = gmtOffset;
      setTimezone(timezone);
    }
    else
    {
      LOGD("GMT Offset for %s not found.", zoneToFind);
    }

    setupCron();
  }
  else
  {
    LOGD("Failed to get time from NTP server.");
  }
  vTaskDelete(NULL);
}

void setLedsDisable(bool all)
{
  if (vars.hwLedPwrIs || vars.hwLedUsbIs)
  {
    LOGD("setLedsDisable", "%s", String(all));
    if (all)
    {
      ledControl.powerLED.active = false;
      ledControl.modeLED.active = false;
    }
    else
    {
      ledControl.powerLED.active = !systemCfg.disableLedPwr;
      ledControl.modeLED.active = !systemCfg.disableLedUSB;
    }
  }
}

void nmActivate()
{
  LOGD("start");
  setLedsDisable(true);
}

void nmDeactivate()
{
  LOGD("end");
  setLedsDisable();
}

bool checkDNS(bool setup)
{
  const char *wifiKey = "WiFi";
  const char *ethKey = "ETH";
  const char *savedKey = "Saved";
  const char *restoredKey = "Restored";
  const char *dnsTagKey = "[DNS]";
  char buffer[100];

  if (networkCfg.wifiEnable)
  {
    IPAddress currentWifiDNS = WiFi.dnsIP();
    if (currentWifiDNS != vars.savedWifiDNS)
    {
      char dnsStrW[16];
      snprintf(dnsStrW, sizeof(dnsStrW), "%u.%u.%u.%u", currentWifiDNS[0], currentWifiDNS[1], currentWifiDNS[2], currentWifiDNS[3]);

      int lastDot = -1;
      for (int i = 0; dnsStrW[i] != '\0'; i++)
      {
        if (dnsStrW[i] == '.')
        {
          lastDot = i;
        }
      }

      int fourthPartW = atoi(dnsStrW + lastDot + 1);

      if (setup && fourthPartW != 0)
      {
        vars.savedWifiDNS = currentWifiDNS;
        snprintf(buffer, sizeof(buffer), "%s %s %s - %s", dnsTagKey, savedKey, wifiKey, dnsStrW);
        printLogMsg(buffer);
      }
      else
      {
        if (vars.savedWifiDNS)
        {
          WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), vars.savedWifiDNS);
          snprintf(buffer, sizeof(buffer), "%s %s %s - %s", dnsTagKey, restoredKey, wifiKey, vars.savedWifiDNS.toString().c_str());
          printLogMsg(buffer);
        }
      }
    }
  }

  if (networkCfg.ethEnable)
  {
    IPAddress currentEthDNS = ETH.dnsIP();
    if (currentEthDNS != vars.savedEthDNS)
    {
      char dnsStrE[16];
      snprintf(dnsStrE, sizeof(dnsStrE), "%u.%u.%u.%u", currentEthDNS[0], currentEthDNS[1], currentEthDNS[2], currentEthDNS[3]);

      int lastDot = -1;
      for (int i = 0; dnsStrE[i] != '\0'; i++)
      {
        if (dnsStrE[i] == '.')
        {
          lastDot = i;
        }
      }

      int fourthPartE = atoi(dnsStrE + lastDot + 1);

      if (setup && fourthPartE != 0)
      {
        vars.savedEthDNS = currentEthDNS;
        snprintf(buffer, sizeof(buffer), "%s %s %s - %s", dnsTagKey, savedKey, ethKey, dnsStrE);
        printLogMsg(buffer);
      }
      else
      {
        if (vars.savedEthDNS)
        {
          ETH.config(ETH.localIP(), ETH.gatewayIP(), ETH.subnetMask(), vars.savedEthDNS);
          snprintf(buffer, sizeof(buffer), "%s %s %s - %s", dnsTagKey, restoredKey, ethKey, vars.savedEthDNS.toString().c_str());
          printLogMsg(buffer);
        }
      }
    }
  }
  return true;
}

/*void reCheckDNS()
{
  checkDNS();
}*/

void setupCron()
{
  // Cron.create(const_cast<char *>("30 */1 * * * *"), reCheckDNS, false);

  // const String time = systemCfg.updCheckTime;
  static char formattedTime[16];
  int seconds, hours, minutes;

  String wday = systemCfg.updCheckDay;

  seconds = random(1, 59);

  // char timeArray[6];
  // String(systemCfg.updCheckTime).toCharArray(timeArray, sizeof(timeArray));

  sscanf(systemCfg.updCheckTime, "%d:%d", &hours, &minutes);

  snprintf(formattedTime, sizeof(formattedTime), "%d %d %d * * %s", seconds, minutes, hours, wday);

  // LOGD("UPD cron %s", String(formattedTime));
  printLogMsg("[UPD_CHK] cron " + String(formattedTime));

  Cron.create(const_cast<char *>(formattedTime), checkUpdateAvail, false); // 0 0 */6 * * *

  if (systemCfg.nmEnable)
  {

    time_t nowSecs = time(nullptr);
    struct tm timeinfo;
    localtime_r(&nowSecs, &timeinfo);
    int currentTimeInMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;

    int sTargetHour, sTargetMinute, startTimeInMinutes;
    int eTargetHour, eTargetMinute, endTimeInMinutes;

    sscanf(systemCfg.nmStart, "%d:%d", &sTargetHour, &sTargetMinute);
    startTimeInMinutes = sTargetHour * 60 + sTargetMinute;

    sscanf(systemCfg.nmEnd, "%d:%d", &eTargetHour, &eTargetMinute);
    endTimeInMinutes = eTargetHour * 60 + eTargetMinute;

    if (startTimeInMinutes <= endTimeInMinutes)
    {
      if (currentTimeInMinutes >= startTimeInMinutes && currentTimeInMinutes < endTimeInMinutes)
      {
        nmActivate();
      }
      else
      {
        nmDeactivate();
      }
    }
    else
    {
      if (currentTimeInMinutes >= startTimeInMinutes || currentTimeInMinutes < endTimeInMinutes)
      {
        nmActivate();
      }
      else
      {
        nmDeactivate();
      }
    }

    char startCron[30];
    char endCron[30];
    strcpy(startCron, convertTimeToCron(String(systemCfg.nmStart)));
    strcpy(endCron, convertTimeToCron(String(systemCfg.nmEnd)));
    LOGD("cron", "NM start %s", startCron);
    LOGD("cron", "NM end %s", endCron);

    Cron.create(const_cast<char *>(startCron), nmActivate, false);
    Cron.create(const_cast<char *>(endCron), nmDeactivate, false);
  }
}

void setTimezone(String timezone)
{
  // LOGD("Setting Timezone");
  setenv("TZ", timezone.c_str(), 1); //  Now adjust the TZ.  Clock settings are adjusted to show the new local time
  tzset();
  time_t nowSecs = time(nullptr);
  struct tm timeinfo;
  localtime_r(&nowSecs, &timeinfo);

  String timeNow = asctime(&timeinfo);
  timeNow.remove(timeNow.length() - 1);
  printLogMsg("[Time] " + timeNow);
}

const char *getGmtOffsetForZone(const char *zone)
{
  for (int i = 0; i < timeZoneCount; i++)
  {
    if (strcmp(zone, timeZones[i].zone) == 0)
    {
      // Zone found, return GMT Offset
      return timeZones[i].gmtOffset;
    }
  }
  // Zone not found
  return nullptr;
}

String getTime()
{
  time_t nowSecs = time(nullptr);
  struct tm timeinfo;
  localtime_r(&nowSecs, &timeinfo);

  String timeNow = asctime(&timeinfo);
  timeNow.remove(timeNow.length() - 1);
  return timeNow;
}

char *convertTimeToCron(const String &time)
{
  static char formattedTime[16];
  int hours, minutes;

  char timeArray[6];
  time.toCharArray(timeArray, sizeof(timeArray));

  sscanf(timeArray, "%d:%d", &hours, &minutes);

  snprintf(formattedTime, sizeof(formattedTime), "0 %d %d * * *", minutes, hours);

  return formattedTime;
}

ThisConfigStruct *findBrdConfig(int searchId = 0)
{

  bool ethOk = false;
  bool btnOk = false;
  bool zbOk = false;

  static ThisConfigStruct bestConfig;
  bestConfig.eth = {.addr = -1, .pwrPin = -1, .mdcPin = -1, .mdiPin = -1, .phyType = ETH_PHY_LAN8720, .clkMode = ETH_CLOCK_GPIO17_OUT}; // .pwrAltPin = -1};
  bestConfig.zb = {.txPin = -1, .rxPin = -1, .rstPin = -1, .bslPin = -1};
  memset(&bestConfig.mist, -1, sizeof(bestConfig.mist));
  strlcpy(bestConfig.board, "Unknown", sizeof(bestConfig.board));

  for (int brdIdx = searchId; brdIdx < BOARD_CFG_CNT; ++brdIdx)
  {
    int ethIdx = brdConfigs[brdIdx].ethConfigIndex;
    int zbIdx = brdConfigs[brdIdx].zbConfigIndex;
    int mistIdx = brdConfigs[brdIdx].mistConfigIndex;

    LOGI("Try brd: %d - %s", brdIdx, brdConfigs[brdIdx].board);

    if (ethIdx == -1)
    {
      ethOk = true;
      LOGD("NO ethernet OK: %d", ethIdx);
    }
    else if (ETH.begin(ethConfigs[ethIdx].addr, ethConfigs[ethIdx].pwrPin, ethConfigs[ethIdx].mdcPin, ethConfigs[ethIdx].mdiPin, ethConfigs[ethIdx].phyType, ethConfigs[ethIdx].clkMode)) // ethConfigs[ethIdx].pwrAltPin))
    {
      ethOk = true;
      bestConfig.eth = ethConfigs[ethIdx];
      LOGD("Ethernet config OK: %d", ethIdx);
    }

    if (mistConfigs[mistIdx].btnPin > 0)
    {
      pinMode(mistConfigs[mistIdx].btnPin, INPUT);
      int press = 0;
      for (int y = 0; y < 10; y++)
      {
        int state = digitalRead(mistConfigs[mistIdx].btnPin);
        if (state != mistConfigs[mistIdx].btnPlr)
        {
          press++;
        }
        delay(100);
      }
      btnOk = (press <= 5);

      if (!btnOk)
      {
        LOGD("Button pin ERROR");
        ethOk = false;
      }
      else
      {
        LOGD("Button pin OK");
      }
    }
    else
    {
      btnOk = true;
    }

    if (btnOk)
    {
      LOGD("Trying Zigbee: %d", zbIdx);
      esp_task_wdt_reset();
      Serial2.begin(systemCfg.serialSpeed, SERIAL_8N1, zbConfigs[zbIdx].rxPin, zbConfigs[zbIdx].txPin);

      int BSL_PIN_MODE = 0;
      if (CCTool.begin(zbConfigs[zbIdx].rstPin, zbConfigs[zbIdx].bslPin, BSL_PIN_MODE))
      {
        if (CCTool.detectChipInfo())
        {
          zbOk = true;
          LOGD("Zigbee config OK: %d", zbIdx);

          bestConfig.zb = zbConfigs[zbIdx];
          bestConfig.mist = mistConfigs[mistIdx];

          bool multiCfg = false;
          int brdNewId = -1;
          for (int brdNewIdx = 0; brdNewIdx < BOARD_CFG_CNT; brdNewIdx++)
          {
            LOGD("%d %d", brdIdx, brdNewIdx);
            if (brdIdx != brdNewIdx && brdConfigs[brdNewIdx].ethConfigIndex == ethIdx && brdConfigs[brdNewIdx].zbConfigIndex == zbIdx && brdConfigs[brdNewIdx].mistConfigIndex == mistIdx)
            {
              multiCfg = true;
              brdNewId = brdNewIdx;
              break;
            }
          }
          const char *nameBrd;
          if (!multiCfg)
          {
            nameBrd = brdConfigs[brdIdx].board;
            strlcpy(bestConfig.board, nameBrd, sizeof(bestConfig.board));
          }
          else
          {
            String nameBrdStr = ("Multi_" + String(brdNewId));
            nameBrd = nameBrdStr.c_str();
            // LOGW("%s", nameBrdStr);
            strlcpy(bestConfig.board, nameBrd, sizeof(bestConfig.board));
          }

          return &bestConfig;
        }
        else
        {
          zbOk = false;
          LOGD("Zigbee config ERROR");
        }
      }
    }

    LOGI("Config error with: %s", brdConfigs[brdIdx].board);
    DynamicJsonDocument config(300);
    config["searchId"] = brdIdx + 1;
    writeDefaultConfig(configFileHw, config);

    LOGD("Restarting...");
    delay(500);
    ESP.restart();

    return nullptr;
  }

  if (bestConfig.eth.addr != -1)
  {
    LOGI("Returning best partial config");
    return &bestConfig;
  }
  else
  {
    LOGI("No valid config found, returning default");
    return &bestConfig;
  }
}

void wgBegin()
{
  checkDNS();
  if (!wg.is_initialized())
  {

    const char *wg_preshared_key = nullptr;
    if (vpnCfg.wgPreSharedKey[0] != '\0')
    {
      wg_preshared_key = vpnCfg.wgPreSharedKey;
      LOGD("vpnCfg.wgPreSharedKey is used");
    }

    if (!wg.begin(
            vpnCfg.wgLocalIP,
            vpnCfg.wgLocalSubnet,
            vpnCfg.wgLocalPort,
            vpnCfg.wgLocalGateway,
            vpnCfg.wgLocalPrivKey,
            vpnCfg.wgEndAddr,
            vpnCfg.wgEndPubKey,
            vpnCfg.wgEndPort,
            vpnCfg.wgAllowedIP,
            vpnCfg.wgAllowedMask,
            vpnCfg.wgMakeDefault,
            wg_preshared_key))
    {
      printLogMsg(String("Failed to initialize WG"));
      vars.vpnWgInit = false;
    }
    else
    {
      printLogMsg(String("WG was initialized"));
      /*LOGD("%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s",
           String(vpnCfg.wgLocalIP.toString().c_str()),
           String(vpnCfg.wgLocalSubnet.toString().c_str()),
           String(vpnCfg.wgLocalPort),
           String(vpnCfg.wgLocalGateway.toString().c_str()),
           String(vpnCfg.wgLocalPrivKey).c_str(),
           String(vpnCfg.wgEndAddr).c_str(),
           String(vpnCfg.wgEndPubKey).c_str(),
           String(vpnCfg.wgEndPort),
           String(vpnCfg.wgAllowedIP.toString().c_str()),
           String(vpnCfg.wgAllowedMask.toString().c_str()),
           String(vpnCfg.wgMakeDefault),
           String(wg_preshared_key).c_str());*/
      vars.vpnWgInit = true;
    }
  }
}

void wgLoop()
{

  // IPAddress ip = ;

  int checkPeriod = 5; // to do vpnCfg.checkTime;
  ip_addr_t lwip_ip;

  lwip_ip.u_addr.ip4.addr = static_cast<uint32_t>(vpnCfg.wgLocalIP);

  if (wg.is_initialized())
  {
    if (vars.vpnWgCheckTime == 0)
    {
      vars.vpnWgCheckTime = millis() + 1000 * checkPeriod;
    }
    else
    {
      if (vars.vpnWgCheckTime <= millis())
      {
        uint16_t wgLocalPort = vpnCfg.wgLocalPort;
        vars.vpnWgCheckTime = millis() + 1000 * checkPeriod;
        if (wg.is_peer_up(&lwip_ip, &wgLocalPort))
        {
          vars.vpnWgPeerIp = (lwip_ip.u_addr.ip4.addr);
          if (!vars.vpnWgConnect)
          {
            LOGD("Peer with IP %s connect", vars.vpnWgPeerIp.toString().c_str());
          }
          vars.vpnWgConnect = true;
        }
        else
        {
          if (vars.vpnWgConnect)
          {
            LOGD("Peer disconnect");
          }
          vars.vpnWgPeerIp.clear();
          vars.vpnWgConnect = false;
        }
      }
    }
  }
  else
  {
    vars.vpnWgInit = false;
  }
}

/* //not available now
void hnBegin()
{
  Husarnet.selfHostedSetup(vpnCfg.hnDashUrl);
  Husarnet.join(vpnCfg.hnJoinCode, vpnCfg.hnHostName);
  Husarnet.start();
}
*/

void ledTask(void *parameter)
{
  LEDSettings *led = (LEDSettings *)parameter;
  TickType_t lastWakeTime = xTaskGetTickCount();
  int previousMode = LED_OFF;

  while (1)
  {
    // LOGD("%d | led %s | m %d", millis(), led->name, led->mode);
    if (led->pin == -1)
      continue;
    if (!led->active)
    {
      digitalWrite(led->pin, LOW);
      vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(500));
      continue;
    }

    switch (led->mode)
    {
    case LED_OFF:
      previousMode = led->mode;
      digitalWrite(led->pin, LOW);
      vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(500));
      break;
    case LED_ON:
      previousMode = led->mode;
      digitalWrite(led->pin, HIGH);
      vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(500));
      break;
    case LED_TOGGLE:
      if (digitalRead(led->pin) == LOW)
      {
        digitalWrite(led->pin, HIGH);
        led->mode = LED_ON;
      }
      else
      {
        digitalWrite(led->pin, LOW);
        led->mode = LED_OFF;
      }
      vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(500));
      break;
    case LED_BLINK_5T:
      for (int j = 0; j < 5; j++)
      {
        digitalWrite(led->pin, HIGH);
        vTaskDelay(pdMS_TO_TICKS(500));
        digitalWrite(led->pin, LOW);
        vTaskDelay(pdMS_TO_TICKS(500));
      }
      led->mode = static_cast<LEDMode>(previousMode);
      break;
    case LED_BLINK_1T:
      digitalWrite(led->pin, HIGH);
      vTaskDelay(pdMS_TO_TICKS(500));
      digitalWrite(led->pin, LOW);
      vTaskDelay(pdMS_TO_TICKS(500));
      led->mode = static_cast<LEDMode>(previousMode);
      break;
    case LED_BLINK_1Hz:
    case LED_FLASH_1Hz:
      previousMode = led->mode;
      digitalWrite(led->pin, (led->mode == LED_BLINK_1Hz) ? HIGH : LOW);
      vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(800));
      digitalWrite(led->pin, (led->mode == LED_BLINK_1Hz) ? LOW : HIGH);
      vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(200));
      break;
    case LED_BLINK_3Hz:
    case LED_FLASH_3Hz:
      previousMode = led->mode;
      for (int j = 0; j < 3; j++)
      {
        digitalWrite(led->pin, (led->mode == LED_BLINK_3Hz) ? HIGH : LOW);
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(267));
        digitalWrite(led->pin, (led->mode == LED_BLINK_3Hz) ? LOW : HIGH);
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(100));
      }
      break;
    }
  }
}

int compareVersions(String v1, String v2)
{
  int v1_major = v1.substring(0, v1.indexOf('.')).toInt();
  int v2_major = v2.substring(0, v2.indexOf('.')).toInt();

  if (v1_major != v2_major)
  {
    return v1_major - v2_major;
  }

  String v1_suffix = v1.substring(v1.indexOf('.') + 1);
  String v2_suffix = v2.substring(v2.indexOf('.') + 1);

  if (v1_suffix.length() == 0)
    return -1;
  if (v2_suffix.length() == 0)
    return 1;

  return v1_suffix.compareTo(v2_suffix);
}

void checkUpdateAvail()
{
  if (!vars.apStarted)
  {
    const char *ESPkey = "ESP";
    const char *ZBkey = "ZB";
    const char *FoundKey = "Found ";
    const char *NewFwKey = " new fw: ";
    const char *TryKey = "try to install";
    String latestReleaseUrlEsp = fetchLatestEspFw();
    String latestReleaseUrlZb = fetchLatestZbFw();
    String latestVersionEsp = extractVersionFromURL(latestReleaseUrlEsp);
    String latestVersionZb = extractVersionFromURL(latestReleaseUrlZb);

    LOGD("%s %s", ESPkey, latestVersionEsp.c_str());
    LOGD("%s %s", ZBkey, latestVersionZb.c_str());

    if (latestVersionEsp.length() > 0 && compareVersions(latestVersionEsp, VERSION) > 0)
    {
      vars.updateEspAvail = true;
      printLogMsg(String(FoundKey) + String(ESPkey) + String(NewFwKey) + latestVersionEsp);
      if (systemCfg.updAutoInst)
      {
        printLogMsg(String(TryKey));
        getEspUpdate(latestReleaseUrlEsp);
      }
    }
    else
    {
      vars.updateEspAvail = false;
    }

    if (CCTool.chip.fwRev > 0 && latestVersionZb.length() > 0 && compareVersions(latestVersionZb, String(CCTool.chip.fwRev)) > 0)
    {
      vars.updateZbAvail = true;
      printLogMsg(String(FoundKey) + String(ZBkey) + String(NewFwKey) + latestVersionZb);
      if (systemCfg.updAutoInst)
      {
        printLogMsg(String(TryKey));
        flashZbUrl(latestReleaseUrlZb);
        ESP.restart();
      }
    }
    else
    {
      vars.updateZbAvail = false;
    }
  }
}
