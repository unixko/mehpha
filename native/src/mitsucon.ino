/*
 * Copyright notice goes here
 */

#include <Arduino.h>
#ifdef ESP32
#include <WiFi.h>
#else
#include <ESP8266WiFi.h>
#endif
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <HeatPump.h>
#include <NeoPixelBus.h> // conflict rom_i2c_writeReg_Mask ?

#include "mitsucon.h"

#ifdef OTA
  #include <ESP8266mDNS.h>
  #include <ArduinoOTA.h>
#endif

char debug_message[256]; // be verbose, eh?
extern HardwareSerial Serial;
//extern HardwareSerial Serial1;

HardwareSerial *dbgSerial;

// wifi, mqtt and heatpump client instances
#ifdef MQTTS
WiFiClientSecure espClient;
#else
WiFiClient espClient;
#endif
PubSubClient mqtt_client(espClient);
HeatPump hp;
unsigned long lastTempSend;
unsigned long lastLEDShow;
uint16_t debugcount;

const char* controller_sw_version       = "20200117-1900"; // Software Version displayed in Home Assistant

// debug mode, when true, will send all packets received from the heatpump to topic heatpump_debug_topic
// this can also be set by sending "on" to heatpump_debug_set_topic
bool _debugMode = false;

// auto mode, when true, heatpump will try to follow desired temperature
// and the RGBLED will track too
bool _autoMode = true;

#ifdef HAVE_RGB_LED
/* Our WS12813 is on GPIO2, is in G-R-B order and should appreciate 800kHz */
#define RGB_LED_COUNT 1
NeoPixelBus<NeoGrbFeature, NeoEsp8266BitBang800KbpsMethod> rgbled(RGB_LED_COUNT, rgbLedPin);
RgbColor _cachedLed;
RgbColor _led;
uint8_t _scale=100; // assume daylight
const RgbColor _greenLed(0,48,0);
const RgbColor _orangeLed(48,32,0);
const RgbColor _redLed(48,0,0);
const RgbColor _blueLed(0,0,48);
const RgbColor _blackLed(0,0,0);
// 23,0,47 is a nice purple/mauve
#endif

#ifdef SERIAL_IS_DEBUG
#define dbgprintln(msg) if(dbgSerial) { dbgSerial->println(msg);}
#define dbgprintf(fmt, ...) if(dbgSerial) { dbgSerial->printf(fmt, ##__VA_ARGS__);}
#else
// compiler complains about redefining these. Does it not parse the #ifdef above?
#undef dbgprintln
#define dbgprintln(msg) 
#undef dbgprintf
#define dbgprintf(fmt, ...)
#endif


void update_led( bool mathRequired ) 
{
#ifdef HAVE_RGB_LED
  
  if( mathRequired ) {
    _cachedLed = _led;
    if (_scale != 100 ) {
      _cachedLed.R= (_led.R * _scale) / 100;
      _cachedLed.G= (_led.G * _scale) / 100;
      _cachedLed.B= (_led.B * _scale) / 100;
    }
  }
  // always update the value, either from the existing cached copy or the newly updated cached copy
  rgbled.SetPixelColor(0,_cachedLed);
#endif
}

/* This code was lifted wholesale from Tasmota
 * xdrv_02_mqtt.ino v6.3 circa April 2019, GPL v3
 */
uint16_t mqtt_retry_counter = 1;            // MQTT connection retry counter
uint8_t mqtt_initial_connection_state = 2;  // MQTT connection messages state
bool mqtt_connected = false;                // MQTT virtual connection status

#ifdef MQTTS
bool MqttCheckTls(void)
{
  bool result = false;

  //Debug(LOG_LEVEL_INFO, S_LOG_MQTT, PSTR(D_FINGERPRINT));

//#ifdef ARDUINO_ESP8266_RELEASE_2_4_1
  espClient = WiFiClientSecure();               // Wifi Secure Client reconnect issue 4497 (https://github.com/esp8266/Arduino/issues/4497)
//#endif

  if (!espClient.connect(mqtt_server, mqtt_port)) {
#ifdef SERIAL_IS_DEBUG
    snprintf_P(debug_message, sizeof(debug_message), PSTR("Connection Failed To %s:%d. Retry in %d seconds"),
      mqtt_server, mqtt_port, mqtt_retry_counter);
    dbgprintln(debug_message);
#endif
  } else {
    if (espClient.verify(mqtt_fingerprint, mqtt_server)) {
      dbgprintln("TLS VerificationSuccess");
      result = true;
    }
  }
  if (!result) { 
      dbgprintln("TLS VerificationFAILED");
  }
  espClient.stop();
  yield();
  return result;
}
/* End uplifted Tasmota code */
#else
bool MqttCheckTls(void) 
{
  return true;
}
#endif

void mqttConnect() {
  dbgprintln("\n  entrypoint mqttConnect()");

  // Loop until we're reconnected
  while (!mqtt_client.connected()) {
    // Attempt to connect
    dbgprintln("attempt mqtt connect");
    if (mqtt_client.connect(client_id, mqtt_username, mqtt_password, heatpump_lastwill_topic, 1, true, heatpump_lastwill_message)) {
      if(( mqtt_initial_connection_state==2 && MqttCheckTls()) || (mqtt_initial_connection_state != 2)) {
        mqtt_initial_connection_state=0;
        mqtt_client.subscribe(heatpump_mode_command_topic);
        mqtt_client.subscribe(heatpump_temperature_command_topic);
#ifdef HAVE_RGB_LED
        mqtt_client.subscribe(heatpump_led_command_topic);
#endif
        mqtt_client.subscribe(heatpump_fan_mode_command_topic);
        mqtt_client.subscribe(heatpump_swing_mode_command_topic);
        mqtt_client.publish(heatpump_lastwill_topic,heatpump_online_message);
      }
    } else {
      // Wait 5 seconds before retrying
      dbgprintln("mqtt fail and pause");
      delay(5000);
    }
  }
}

void mqttAutoDiscovery() {
  const String chip_id                 = String(ESP.getChipId());
  const String mqtt_discov_topic       = String(mqtt_discov_prefix) + "/climate/" + chip_id + "/config";
  
  const size_t bufferSizeDiscovery = JSON_OBJECT_SIZE(52);
  DynamicJsonDocument rootDiscovery(bufferSizeDiscovery);
  
  rootDiscovery["name"]                = ha_entity_id;
  rootDiscovery["uniq_id"]             = chip_id;
  rootDiscovery["~"]                   = heatpump_topic;
  rootDiscovery["min_temp"]            = min_temp;
  rootDiscovery["max_temp"]            = max_temp;
  rootDiscovery["temp_step"]           = temp_step;
  JsonArray modes                      = rootDiscovery.createNestedArray("modes");
    modes.add("heat_cool");
    modes.add("cool");
    modes.add("dry");
    modes.add("heat");
    modes.add("fan_only");
    modes.add("off");
  JsonArray fan_modes                  = rootDiscovery.createNestedArray("fan_modes");
    fan_modes.add("auto");
    fan_modes.add("quiet");
    fan_modes.add("1");
    fan_modes.add("2");
    fan_modes.add("3");
    fan_modes.add("4");
  JsonArray swing_modes                = rootDiscovery.createNestedArray("swing_modes");
    swing_modes.add("auto");
    swing_modes.add("1");
    swing_modes.add("2");
    swing_modes.add("3");
    swing_modes.add("4");
    swing_modes.add("5");
    swing_modes.add("swing");
  rootDiscovery["avty_t"]              = "~/tele/lwt";
  rootDiscovery["curr_temp_t"]         = "~/tele/temp";
  rootDiscovery["curr_temp_tpl"]       = "{{ value_json.roomTemperature }}";
  rootDiscovery["mode_cmd_t"]          = "~/cmnd/mode";
  rootDiscovery["mode_stat_t"]         = "~/tele/stat";
  rootDiscovery["mode_stat_tpl"]       = "{{ 'off' if value_json.power == 'OFF' else value_json.mode | lower | replace('auto', 'heat_cool') | replace('fan', 'fan_only') }}";
  rootDiscovery["temp_cmd_t"]          = "~/cmnd/temp";
  rootDiscovery["temp_stat_t"]         = "~/tele/stat";
  rootDiscovery["temp_stat_tpl"]       = "{{ value_json.temperature }}";
  rootDiscovery["fan_mode_cmd_t"]      = "~/cmnd/fan";
  rootDiscovery["fan_mode_stat_t"]     = "~/tele/stat";
  rootDiscovery["fan_mode_stat_tpl"]   = "{{ value_json.fan | lower }}";
  rootDiscovery["swing_mode_cmd_t"]    = "~/cmnd/vane";
  rootDiscovery["swing_mode_stat_t"]   = "~/tele/stat";
  rootDiscovery["swing_mode_stat_tpl"] = "{{ value_json.vane | lower }}";
  rootDiscovery["json_attr_t"]         = "~/tele/attr";
  JsonObject device                    = rootDiscovery.createNestedObject("device");
    device["name"]                     = ha_entity_id;
    JsonArray ids = device.createNestedArray("ids");
      ids.add(chip_id);
    device["mf"]                       = "MitsuCon";
    device["mdl"]                      = "Mitsubishi Heat Pump";
    device["sw"]                       = controller_sw_version;

  char bufferDiscovery[1280];
  serializeJson(rootDiscovery, bufferDiscovery);

  if (!mqtt_client.publish(mqtt_discov_topic.c_str(), bufferDiscovery, true)) {
    mqtt_client.publish(heatpump_debug_topic, "failed to publish DISCOV topic");
    dbgprintf("fail publish heatpump, length=%u buffer=%s\n", length, buffer);    
  }
}

void hpSettingsChanged() {
  const size_t bufferSize = JSON_OBJECT_SIZE(6);
  DynamicJsonDocument root(bufferSize);

  heatpumpSettings currentSettings;
#ifdef SERIAL_IS_DEBUG
  // okay, excessive now
  currentSettings.connected=false;
  currentSettings.power="OFF";
  currentSettings.temperature=0;
  currentSettings.fan="AUTO";
  currentSettings.mode="AUTO";
  currentSettings.vane="AUTO";
  currentSettings.wideVane="|";
#endif
  currentSettings = hp.getSettings();

  root["power"]       = currentSettings.power;
  root["mode"]        = currentSettings.mode;
  root["temperature"] = currentSettings.temperature;
  root["fan"]         = currentSettings.fan;
  root["vane"]        = currentSettings.vane;
  root["wideVane"]    = currentSettings.wideVane;
  //root["iSee"]        = currentSettings.iSee;

  char buffer[512];
  serializeJson(root, buffer);

  if (!mqtt_client.publish(heatpump_state_topic, buffer, true)) {
    mqtt_client.publish(heatpump_debug_topic, "failed to publish STATE topic");
    dbgprintf("fail publish heatpump, length=%u buffer=%s\n", length, buffer);
  }
}

void hpStatusChanged(heatpumpStatus currentStatus) {
  dbgprintln("  entrypoint hpStatusChanged()");

  // Publish Temperature alone
  const size_t bufferSizeInfo = JSON_OBJECT_SIZE(2); // 1 key + 1 value
  DynamicJsonDocument rootInfo(bufferSizeInfo);

  rootInfo["roomTemperature"] = currentStatus.roomTemperature;

  char bufferInfo[512];
  serializeJson(rootInfo, bufferInfo);

  if (!mqtt_client.publish(heatpump_temperature_topic, bufferInfo, true)) {
    mqtt_client.publish(heatpump_debug_topic, "failed to publish TEMP topic");
    dbgprintf("fail publish room temp, length=%u buffer=%s\n", length, bufferInfo);
  }

#ifdef HAVE_RGB_LED
  // Publish LED States
  const size_t bufferSizeLights = JSON_OBJECT_SIZE(12); // 6 keys + 6 values
  DynamicJsonDocument rootLights(bufferSizeLights);

  rootLights["wizzard"] = _autoMode;
  rootLights["leds"] = _scale;
  rootLights["ledr"] = _led.R;
  rootLights["ledg"] = _led.G;
  rootLights["ledb"] = _led.B;
  
  char bufferLights[512];
  serializeJson(rootLights, bufferLights);

  if (!mqtt_client.publish(heatpump_led_topic, bufferLights, true)) {
    mqtt_client.publish(heatpump_debug_topic, "failed to publish LED topic");
    dbgprintf("fail publish led, length=%u buffer=%s\n", length, bufferLights);
  }
#endif


  // Publish Operating Mode and Timers
  const size_t bufferSizeTimers = JSON_OBJECT_SIZE(12); // 6 keys + 6 values
  DynamicJsonDocument rootTimers(bufferSizeTimers);

  rootTimers["hvac_action"]      = currentStatus.operating;
  rootTimers["timer_set"]        = currentStatus.timers.mode;
  rootTimers["timer_on_mins"]    = currentStatus.timers.onMinutesSet;
  rootTimers["timer_on_remain"]  = currentStatus.timers.onMinutesRemaining;
  rootTimers["timer_off_mins"]   = currentStatus.timers.offMinutesSet;
  rootTimers["timer_off_remain"] = currentStatus.timers.offMinutesRemaining;

  char bufferTimers[512];
  serializeJson(rootTimers, bufferTimers);

  if (!mqtt_client.publish(heatpump_attribute_topic, bufferTimers, true)) {
    mqtt_client.publish(heatpump_debug_topic, "failed to publish ATTR topic");
    dbgprintf("fail publish attr, length=%u buffer=%s\n", length, bufferTimers);
  }

  //mqtt_client.publish(heatpump_availability_topic, "online", true); // wot, no LWT?
  //dbgprintln("  exitpoint hpStatusChanged()");  
}

void hpPacketDebug(byte* packet, unsigned int length, char* packetDirection) {
  if (_debugMode) {
    String message;
    for (unsigned int idx = 0; idx < length; idx++) {
      if (packet[idx] < 16) {
        message += "0"; // pad single hex digits with a 0
      }
      message += String(packet[idx], HEX) + " ";
    }

    const size_t bufferSize = JSON_OBJECT_SIZE(8);
    DynamicJsonDocument root(bufferSize);

    root[packetDirection] = message;

    char buffer[512];
    serializeJson(root, buffer);

    if (!mqtt_client.publish(heatpump_debug_topic, buffer)) {
      mqtt_client.publish(heatpump_debug_topic, "failed to publish DEBUG topic");
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  dbgprintln("\n  entrypoint mqttCallback()");
  bool didLed=false;
  bool didAction=false;

  // Copy payload into message buffer
  char message[length + 1];
  for (unsigned int i = 0; i < length; i++) {
    message[i] = (char)payload[i];
  }
  message[length] = '\0';

  const size_t bufferSize = JSON_OBJECT_SIZE(6); // 3 keys + 3 values
  DynamicJsonDocument root(bufferSize);

  heatpumpSettings currentSettings = hp.getSettings();

  root["power"]       = currentSettings.power;
  root["mode"]        = currentSettings.mode;
  root["temperature"] = currentSettings.temperature;
  root["fan"]         = currentSettings.fan;
  root["vane"]        = currentSettings.vane;
  // WideVane?
  // iSee?

  // Home Assistant Auto-Discovery has more topics than Gerard's old code..

  if (strcmp(topic, heatpump_led_command_topic) == 0) {
#ifdef HAVE_RGB_LED
    uint8_t coleh;
    /* Apparently case sensitive */
    if (root.containsKey("ledr")) {
      didLed=true;
      coleh = root["ledr"]; // make the typecast explicit
      _led.R = coleh;
    }
    if (root.containsKey("ledg")) {
      didLed=true;
      _led.G = root["ledg"];
    }
    if (root.containsKey("ledb")) {
      didLed=true;
      _led.B = root["ledb"];
    }
    if (root.containsKey("leds")) {
      didLed=true;
      _scale = root["leds"];
    } 
    if (root.containsKey("wizzard")) {
      didLed=true;
      _autoMode = root["wizzard"];
      if( !_autoMode ) {
        _led=_blackLed;
        _scale=100; // assume daylight
      }
    }
#endif
  } else if (strcmp(topic, heatpump_mode_command_topic) == 0) { // if the incoming message is on the heatpump_mode_command_topic topic...
    if (strcmp(message, "off") == 0) {
      const char* power = "OFF";
      didAction=true;
      hp.setPowerSetting(power);
      root["power"] = power;
    } else if (strcmp(message, "heat_cool") == 0) {
      const char* power = "ON";
      didAction=true;
      hp.setPowerSetting(power);      
      root["power"] = power;
      const char* mode = "AUTO";
      hp.setModeSetting(mode);
      root["mode"] = mode;
    } else if (strcmp(message, "fan_only") == 0) {
      const char* power = "ON";
      didAction=true;
      hp.setPowerSetting(power);      
      root["power"] = power;
      const char* mode = "FAN";
      hp.setModeSetting(mode);
      root["mode"] = mode;
    } else {
      const char* power = "ON";
      didAction=true;
      hp.setPowerSetting(power);
      root["power"] = power;
      const char* mode = strupr(message);
      hp.setModeSetting(mode);
      root["mode"] = mode;
    }
  } else if (strcmp(topic, heatpump_temperature_command_topic) == 0) { // Temperature Command
    float temperature = atof(message);
      didAction=true;
    hp.setTemperature(temperature);
    root["temperature"] = temperature;
  } else if (strcmp(topic, heatpump_fan_mode_command_topic) == 0) { // Fan Command
    const char* fan = strupr(message);      
      didAction=true;
    hp.setFanSpeed(fan);
    root["fan"] = fan;
  } else if (strcmp(topic, heatpump_swing_mode_command_topic) == 0) { // Swing Command
    const char* vane = strupr(message);
      didAction=true;
    hp.setVaneSetting(vane);
    root["vane"] = vane;
  } // WideVane?

  if( didAction) {
    // Skip the update step if we were only custom / only LED based.
    bool result = false;
    result = hp.update();
    if (!result) {
      dbgprintln("heatpump: update() failed");
      mqtt_client.publish(heatpump_debug_topic, "heatpump: update() failed");
    }
  }
  if( didLed ) {
    //mqtt_client.publish(heatpump_debug_topic, "LEDs updated");
    update_led(true);
  }

  // Finish by publishing a status update
  char buffer[512];
  serializeJson(root, buffer);

  if (!mqtt_client.publish(heatpump_state_topic, buffer, true)) {
    mqtt_client.publish(heatpump_debug_topic, "failed to publish STATE topic");
    dbgprintf("fail publish heatpump, length=%u buffer=%s\n", length, buffer);
  }
}

void setup() {
#ifdef HAVE_LEDS
  pinMode(redLedPin, OUTPUT);
  digitalWrite(redLedPin, HIGH);
  pinMode(blueLedPin, OUTPUT);
  digitalWrite(blueLedPin, HIGH);
#endif

#ifdef HAVE_RGB_LED
  rgbled.Begin();
  rgbled.Show(); // apparently defaults to Black.
  _led=_blackLed;
  _scale=100; // assume daylight
#endif

#ifdef SERIAL_IS_DEBUG
  // Move Heatpump to two unused IO. Note 15 has a power-on-requirement
  //Serial.pins(swTXPin,swRXPin);
#else
  // Leave Heatpump on the actual IO sites. No Debug printf today
  //Serial.pins(hwTXPin,hwRXPin);
#endif

#ifdef SERIAL_IS_DEBUG
  // Debug printf is on the actual IO sites
  //dbgSerial->pins(hwTXPin,hwRXPin);
  dbgSerial = &Serial;
  dbgSerial->begin(115200,SERIAL_8N1);

  dbgprintln("\nHello World!");
#else
  dbgSerial = NULL;
#endif

  dbgprintf("Connecting to WiFi SSID: %s\n", mqtt_server);
  WiFi.hostname(client_id);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    // wait 500ms, flashing the blue LED to indicate WiFi connecting...
#ifdef HAVE_LEDS
    digitalWrite(blueLedPin, LOW);
    delay(250);
    digitalWrite(blueLedPin, HIGH);
    delay(250);
#else
    dbgprintln("Wifi: Loop without connection");
    delay(1200);
#endif
  }

  // startup mqtt connection
  mqtt_client.setServer(mqtt_server, mqtt_port);
  mqtt_client.setCallback(mqttCallback);
  mqttConnect();

  // move LWT Online into connect call
  //mqtt_client.publish(heatpump_debug_topic, "Heatpump Controller Online"); // strcat(, WiFi.localIP() )

  dbgprintln("hp.setCallback call here");
  // connect to the heatpump. Callbacks first so that the hpPacketDebug callback is available for connect()
  hp.setSettingsChangedCallback(hpSettingsChanged);
  hp.setStatusChangedCallback(hpStatusChanged);
  hp.setPacketCallback(hpPacketDebug);

#ifdef OTA
#if 0
  ArduinoOTA.setHostname(client_id);
  ArduinoOTA.setPassword(ota_password);
  ArduinoOTA.begin(false);
#else
  ArduinoOTA.begin(WiFi.localIP(), client_id, ota_password, InternalStorage);
#endif
#endif

  dbgprintln("hp.connect call here");
#ifdef SERIAL_IS_DEBUG
  hp.connect(NULL); // No Heatpump today
  //hp.connect(&Serial,true,swTXPin,swRXPin);
#else
  hp.connect(&Serial); // using the first HardwareSerial on the ESP (aka the programming interface)
#endif

  mqttAutoDiscovery();

  lastTempSend = millis();
  lastLEDShow = lastTempSend;
  debugcount = 0;
  dbgprintln("  exitpoint setup()");
}

void loop() {
  /* Begin Loop */
  if( debugcount == 0) {
    dbgprintln("\n  entrypoint loop()");
  } 
  /*
  if( debugcount < 50000 ) {
    debugcount++;
  } else {
    debugcount=0;
  }
  */
  debugcount=1; // only print message once
  //debugcount = (debugcount+1)%4000; // 16 bits or fewer // modulus, why you no work?
  /* post script: modulus no worked because hp.sync() was busywaiting 
   * and breaking interrupts. Fixed by providing explicit 
   * _HardSerial==NULL checks in the library
   */



  /* Auto Mode */
  bool ledChanged=false;
  if( _autoMode ) {
    float roomtherm;
    float targettherm;
    const char *mode;
#ifdef HAVE_RGB_LED
    if( hp.getPowerSettingBool()) {
      ledChanged=(_led==_greenLed?false:true);
      _led=_greenLed;
    } else {
      ledChanged=(_led==_orangeLed?false:true);
      _led=_orangeLed;
    }
#endif
    roomtherm = hp.getRoomTemperature();
    targettherm = hp.getTemperature();
    mode = hp.getModeSetting(); // one of "HEAT", "DRY", "COOL", "FAN", "AUTO"
    if( roomtherm - targettherm > 3.9 ) { // room much hotter than target
      // could tolerate AUTO here?
      if( 0!=strcmp(mode, "COOL")) {
        hp.setModeSetting("COOL");
      }
    } 
    if( targettherm - roomtherm > 3.9 ) { // room much colder than target
    // could tolerate AUTO here?
      if( 0!=strcmp(mode, "HEAT")) {
        hp.setModeSetting("HEAT");
      }
    }
#ifdef HAVE_RGB_LED
    if( roomtherm - targettherm > 1.9 ) { // room hotter than target
      ledChanged=(_led==_redLed?false:true);
      _led=_redLed;
    }
    if( targettherm - roomtherm > 1.9 ) { // room colder than target
      ledChanged=(_led==_blueLed?false:true);
      _led=_blueLed;
    }
#endif
  }



  /* MQTT */
  if (!mqtt_client.connected()) {
    mqttConnect();
  }
  mqtt_client.loop();



  /* HeatPump */
  hp.sync(); // also picks up our auto-derived setModeSetting()s above

  if (millis() > (lastTempSend + SEND_ROOM_TEMP_INTERVAL_MS)) { // only send the temperature every 60s
    //dbgprintln("Periodic RoomStatus");
    //mqtt_client.publish(heatpump_debug_topic, "Periodic RoomStatus");
    heatpumpStatus justnow;
#ifdef SERIAL_IS_DEBUG
    // excessive
    justnow.roomTemperature=0;
    justnow.operating=false;
    heatpumpTimers nultim;
    nultim.mode="NONE";
    nultim.onMinutesSet=0;
    nultim.onMinutesRemaining=0;
    nultim.offMinutesSet=0;
    nultim.offMinutesRemaining=0;
    justnow.timers=nultim;
#endif
    justnow = hp.getStatus();

    hpStatusChanged(justnow);
    lastTempSend = millis();
#if 0
    memcurr = ESP.getFreeHeap();
    dbgprintf("FREEHeap: %d; DIFF %d\n", memcurr, memcurr - memlast);
    memlast = memcurr;
#endif
  }


  /* LEDs */
  update_led(ledChanged);
  if (millis() > (lastLEDShow + 250) ) { // only update the LEDs four times per second
    //dbgprintf("Periodic prettyLights %lu\n", debugcount);
#ifdef HAVE_RGB_LED
    rgbled.Show();
#endif
    lastLEDShow = millis();
  }


  /* OTA Update */
#ifdef OTA
  ArduinoOTA.handle();
#endif


  /* End Loop */
}

