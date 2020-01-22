/*
 * Copyright notice goes here
 */

#include <Arduino.h>
#ifdef ESP32
#include <WiFi.h>
#else // not ESP32
#include <ESP8266WiFi.h>
#endif // ESP32 / not
#include <time.h> // NTP
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <HeatPump.h>
#include <NeoPixelBus.h> // conflict rom_i2c_writeReg_Mask ?

#include "mitsucon.h"

#ifdef OTA
#ifdef ESP32
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#else // yes OTA not ESP32
#if !defined(NO_GLOBAL_INSTANCES) && !defined(NO_GLOBAL_MDNS)
#include <ESP8266mDNS.h>
#endif // mDNS
#endif // ESP32 / not
#include <ArduinoOTA.h>
#endif // OTA

char debug_message[256]; // be verbose, eh?
extern HardwareSerial Serial;
//extern HardwareSerial Serial1;

HardwareSerial *dbgSerial;

// wifi, mqtt and heatpump client instances
#ifdef HAVE_MQTTS
//#include "WiFiClientSecureLightBearSSL.h"
BearSSL::WiFiClientSecure *tlsClient;
//WiFiClientSecure espClient;
PubSubClient mqtt_client;
#else // not HAVE_MQTTS
WiFiClient espClient;
PubSubClient mqtt_client(espClient);
#endif // HAVE_MQTTS / not
HeatPump hp;
unsigned long lastTempSend;
unsigned long lastLEDShow;
uint16_t debugcount;

const char* controller_sw_version       = "20200119-1800"; // Software Version displayed in Home Assistant

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
uint8_t _scale=30; // assume indoors
const RgbColor _greenLed(0,48,0);
const RgbColor _orangeLed(48,32,0);
const RgbColor _redLed(48,0,0);
const RgbColor _blueLed(0,0,48);
const RgbColor _blackLed(0,0,0);
// 23,0,47 is a nice purple/mauve
#endif // HAVE_RGB_LED

#ifdef SERIAL_IS_DEBUG
#define dbgprintln(msg) if(dbgSerial) { dbgSerial->println(msg);}
#define dbgprintf(fmt, ...) if(dbgSerial) { dbgSerial->printf(fmt, ##__VA_ARGS__);}
#else // not SERIAL_IS_DEBUG
// compiler complains about redefining these. Does it not parse the #ifdef above?
#undef dbgprintln
#define dbgprintln(msg) 
#undef dbgprintf
#define dbgprintf(fmt, ...)
#endif // SERIAL_IS_DEBUG


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
#endif // HAVE_RGB_LED
}

// Set time via NTP, as required for x.509 validation
time_t setClock() {
  configTime(ntp_time_offset, ntp_dst_offset, ntp_server1, ntp_server2);

  dbgprintf("Waiting for NTP time sync: ");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) { // delay until 1 January 1970 16:00 uhrs
    delay(500);
    dbgprintf(".");
    now = time(nullptr);
  }
  dbgprintln(""); // EOL
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  dbgprintf("Current time: %s\n", asctime(&timeinfo));
  return now;
}

void mqttConnect() {
    dbgprintln("\n  entrypoint mqttConnect()");

  // Loop until we're reconnected
  while (!mqtt_client.connected()) {
    // Attempt to connect
    dbgprintln("attempt mqtt connect");
    bool victory = false;
#ifdef HAVE_MQTTS
    tlsClient->setFingerprint( mqtt_fingerprint);
    setClock(); // NB: does not always include Daylight Savings.
#endif // HAVE_MQTTS

    //dbgprintf("attempt mqtt connect( %s, %s, %s, %s, 1, true, %s)\n", client_id, mqtt_username, mqtt_password, heatpump_availability_topic, heatpump_lastwill_message);
    if (mqtt_client.connect(client_id, mqtt_username, mqtt_password, heatpump_availability_topic, 1, true, heatpump_lastwill_message)) {
      dbgprintln("mqtt reports connected");
#ifdef HAVE_MQTTS
      // this function gets marked as Deprecated. Not sure. Probably the "setFingerprint" above makes this one redundant?
      if (tlsClient->verify(mqtt_fingerprint, mqtt_server)) { // note the FQDN. It should match the certificate, eh?
        dbgprintln("certificate matches");
        victory=true;
      } else {
        dbgprintln("certificate no match");
        victory=false;
      }
#else // not HAVE_MQTTS
      victory=true;
#endif // HAVE_MQTTS / not
    }
    if( victory ) {
        mqtt_client.setCallback(mqttCallback);
        mqtt_client.publish(heatpump_availability_topic,heatpump_online_message, true);
        mqtt_client.subscribe(heatpump_mode_command_topic);
        mqtt_client.subscribe(heatpump_temperature_command_topic);
#ifdef HAVE_RGB_LED
        mqtt_client.subscribe(heatpump_led_command_topic);
#endif // HAVE_RGB_LED
        mqtt_client.subscribe(heatpump_fan_mode_command_topic);
        mqtt_client.subscribe(heatpump_swing_mode_command_topic);
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
  
  const size_t bufferSizeDiscovery = JSON_OBJECT_SIZE(60);
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
    // 19 Jan 2020, Home Assistant marks the unit as always "Unavailble" because it advertises LWT. So let us not advertise it
  //rootDiscovery["avty_t"]              = "~/tele/LWT";
  //rootDiscovery["pl_avail"]            = heatpump_online_message;
  //rootDiscovery["pl_not_avail"]        = heatpump_lastwill_message;
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

  size_t __attribute__((unused)) serzlen;
  char bufferDiscovery[1280];
  serzlen = serializeJson(rootDiscovery, bufferDiscovery);

  if (!mqtt_client.publish(mqtt_discov_topic.c_str(), bufferDiscovery, true)) {
    mqtt_client.publish(heatpump_debug_topic, "failed to publish DISCOV topic");
    dbgprintf("fail publish heatpump, length=%u buffer=%s\n", serzlen, bufferDiscovery);
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
#endif // SERIAL_IS_DEBUG
  currentSettings = hp.getSettings();

  root["power"]       = currentSettings.power;
  root["mode"]        = currentSettings.mode;
  root["temperature"] = currentSettings.temperature;
  root["fan"]         = currentSettings.fan;
  root["vane"]        = currentSettings.vane;
  root["wideVane"]    = currentSettings.wideVane;
  //root["iSee"]        = currentSettings.iSee;

  size_t __attribute__((unused)) serzlen;
  char buffer[512];
  serzlen = serializeJson(root, buffer);

  if (!mqtt_client.publish(heatpump_state_topic, buffer, true)) {
    mqtt_client.publish(heatpump_debug_topic, "failed to publish STATE topic");
    dbgprintf("fail publish heatpump, length=%u buffer=%s\n", serzlen, buffer);
  }
}

void hpStatusChanged(heatpumpStatus currentStatus) {
  dbgprintln("  entrypoint hpStatusChanged()");

  // Publish Temperature alone
  const size_t bufferSizeInfo = JSON_OBJECT_SIZE(2); // 1 key + 1 value
  DynamicJsonDocument rootInfo(bufferSizeInfo);

  rootInfo["roomTemperature"] = currentStatus.roomTemperature;

  size_t __attribute__((unused)) serzlen;
  char bufferInfo[512];
  serzlen = serializeJson(rootInfo, bufferInfo);

  if (!mqtt_client.publish(heatpump_temperature_topic, bufferInfo, true)) {
    mqtt_client.publish(heatpump_debug_topic, "failed to publish TEMP topic");
    dbgprintf("fail publish room temp, length=%u buffer=%s\n", serzlen, bufferInfo);
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
  serzlen = serializeJson(rootLights, bufferLights);

  if (!mqtt_client.publish(heatpump_led_topic, bufferLights, true)) {
    mqtt_client.publish(heatpump_debug_topic, "failed to publish LED topic");
    dbgprintf("fail publish led, length=%u buffer=%s\n", serzlen, bufferLights);
  }
#endif // HAVE_RGB_LED


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
  serzlen = serializeJson(rootTimers, bufferTimers);

  if (!mqtt_client.publish(heatpump_attribute_topic, bufferTimers, true)) {
    mqtt_client.publish(heatpump_debug_topic, "failed to publish ATTR topic");
    dbgprintf("fail publish attr, length=%u buffer=%s\n", serzlen, bufferTimers);
  }

  //mqtt_client.publish(heatpump_availability_topic, heatpump_online_message, true);
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

    size_t __attribute__((unused)) serzlen;
    char buffer[512];
    serzlen = serializeJson(root, buffer);

    if (!mqtt_client.publish(heatpump_debug_topic, buffer)) {
      mqtt_client.publish(heatpump_debug_topic, "failed to publish DEBUG topic");
      dbgprintf("fail debug publish, length=%u buffer=%s\n", serzlen, buffer);
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  dbgprintln("\n  entrypoint mqttCallback()");
  bool didLed=false;
  bool didAction=false;

  // Copy payload into message buffer
  char message[length + 1]; // The GCC is strong with this one
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

  if (strcmp(topic, heatpump_led_command_topic) == 0) { // incoming message is JSON on LED topic
#ifdef HAVE_RGB_LED
    uint8_t coleh;
    DynamicJsonDocument ledroot(bufferSize);
    DeserializationError error = deserializeJson(ledroot, message);

    if (error) {
      dbgprintln("!root.success(): invalid JSON on heatpump_set_topic.");
      mqtt_client.publish(heatpump_debug_topic, "!root.success(): invalid JSON on heatpump_set_topic...");
      return;
    }
    /* Apparently case sensitive */
    if (ledroot.containsKey("ledr")) {
      didLed=true;
      coleh = ledroot["ledr"]; // make the typecast explicit
      _led.R = coleh;
    }
    if (ledroot.containsKey("ledg")) {
      didLed=true;
      coleh = ledroot["ledg"]; // make the typecast explicit
      _led.G = coleh;
    }
    if (ledroot.containsKey("ledb")) {
      didLed=true;
      coleh = ledroot["ledb"]; // make the typecast explicit
      _led.B = coleh;
    }
    if (ledroot.containsKey("leds")) {
      didLed=true;
      coleh = ledroot["leds"]; // make the typecast explicit
      _scale = coleh;
    } 
    if (ledroot.containsKey("wizzard")) {
      didLed=true;
      _autoMode = ledroot["wizzard"];
      if( !_autoMode ) {
        _led=_blackLed;
        _scale=30; // assume indoors
      }
    }
#endif // HAVE_RGB_LED
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
    dbgprintf("LEDs Updated. R %u G %u B %u scale %u\n", _led.R, _led.G, _led.B, _scale);
    update_led(true);
  }

  // Finish by publishing a status update
  size_t __attribute__((unused)) serzlen;
  char buffer[512];
  serzlen = serializeJson(root, buffer);

  if (!mqtt_client.publish(heatpump_state_topic, buffer, true)) {
    mqtt_client.publish(heatpump_debug_topic, "failed to publish STATE topic");
    dbgprintf("fail publish heatpump, length=%u buffer=%s\n", serzlen, buffer);
  }
}

void setup() {
#ifdef HAVE_LEDS
  pinMode(redLedPin, OUTPUT);
  digitalWrite(redLedPin, HIGH);
  pinMode(blueLedPin, OUTPUT);
  digitalWrite(blueLedPin, HIGH);
#endif // HAVE_LEDS

#ifdef HAVE_RGB_LED
  rgbled.Begin();
  rgbled.Show(); // apparently defaults to Black.
#ifdef SERIAL_IS_DEBUG
  _led=_orangeLed;
  _scale=30; // assume Indoors
#else // not SERIAL_IS_DEBUG
  _led=_blackLed;
  _scale=30; // assume indoors
#endif // SERIAL_IS_DEBUG / not
  //update_led(true);
#endif // HAVE_RGB_LED

#ifdef SERIAL_IS_DEBUG
  // Move Heatpump to two unused IO. Note 15 has a power-on-requirement
  //Serial.pins(swTXPin,swRXPin);
#else // not SERIAL_IS_DEBUG
  // Leave Heatpump on the actual IO sites. No Debug printf today
  //Serial.pins(hwTXPin,hwRXPin);
#endif // SERIAL_IS_DEBUG / not

#ifdef SERIAL_IS_DEBUG
  // Debug printf is on the actual IO sites
  //dbgSerial->pins(hwTXPin,hwRXPin);
  dbgSerial = &Serial;
  dbgSerial->begin(115200,SERIAL_8N1);

  dbgprintln("\nHello World!");
#else // not SERIAL_IS_DEBUG
  dbgSerial = NULL;
#endif // SERIAL_IS_DEBUG / not

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
#else // not HAVE_LEDS
    dbgprintln("Wifi: Loop without connection");
    delay(1200);
#endif // HAVE_LEDS / not
  }

  // startup mqtt connection
#ifdef HAVE_MQTTS
  tlsClient = new BearSSL::WiFiClientSecure();
  mqtt_client.setClient(*tlsClient);
#endif // HAVE_MQTTS
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
#else // always
  ArduinoOTA.begin(WiFi.localIP(), client_id, ota_password, InternalStorage);
#endif // never / always
#endif // OTA

  dbgprintln("hp.connect call here");
#ifdef SERIAL_IS_DEBUG
  hp.connect(NULL); // No Heatpump today
  //hp.connect(&Serial,true,swTXPin,swRXPin);
#else // not SERIAL_IS_DEBUG
  hp.connect(&Serial); // using the first HardwareSerial on the ESP (aka the programming interface)
#endif // SERIAL_IS_DEBUG / not

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
#endif // HAVE_RGB_LED
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
#endif // HAVE_RGB_LED
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
#endif // SERIAL_IS_DEBUG
    justnow = hp.getStatus();

    hpStatusChanged(justnow);
    lastTempSend = millis();
#if 0
    memcurr = ESP.getFreeHeap();
    dbgprintf("FREEHeap: %d; DIFF %d\n", memcurr, memcurr - memlast);
    memlast = memcurr;
#endif // never
  }


  /* LEDs */
  update_led(ledChanged);
  if (millis() > (lastLEDShow + 250) ) { // only update the LEDs four times per second
    //dbgprintf("Periodic prettyLights %lu\n", debugcount);
#ifdef HAVE_RGB_LED
    rgbled.Show();
#endif // HAVE_RGB_LED
    lastLEDShow = millis();
  }


  /* OTA Update */
#ifdef OTA
  ArduinoOTA.handle();
#endif // OTA


  /* End Loop */
}

