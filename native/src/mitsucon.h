/*
 * Copyright notice goes here
 */

//#define ESP32
#undef ESP32
//#define OTA
#undef OTA
#define HAVE_LEDS
//#undef HAVE_LEDS
//#define HAVE_RGB_LED 1
#undef HAVE_RGB_LED

/* When SERIAL_IS_DEBUG is defined, the Heatpump is detached from 
 * 'HardSerial Serial' (default pins 1=tx, 3=rx) and instead a 
 * 115200,8N1 debug console is presented there.
 */
//#define SERIAL_IS_DEBUG 1
#undef SERIAL_IS_DEBUG

//const char* ota_password = "<YOUR OTA PASSWORD GOES HERE>";

// wifi settings
const char* ssid     = "<YOUR WIFI SSID GOES HERE>";
const char* password = "<YOUR WIFI PASSWORD GOES HERE>";

// mqtt server settings
const char* mqtt_server   = "<YOUR MQTT BROKER IP/HOSTNAME GOES HERE>"; // note that MQTTS prefers FQDN over IP.dot.quad
const int mqtt_port       = 1883; // typically 1883 for MQTT, 8883 for MQTTS
const char* mqtt_username = "<YOUR MQTT USERNAME GOES HERE>";
const char* mqtt_password = "<YOUR MQTT PASSWORD GOES HERE>";

//#define MQTTS
#undef MQTTS

// Home Assistant Mitsubishi Electric Heat Pump Controller https://github.com/unixko/mehpha
// using native MQTT Climate (HVAC) component with MQTT discovery for automatic configuration
// Set PubSubClient.h MQTT_MAX_PACKET_SIZE to 1280

//enable or disable OTA and OTA password
//#define OTA
const char* ota_password  = "OTA_PASSWORD";

/* MQTTS Certificate Fingerprint
 * So the ESP8266 in particular is a touch underpowered
 * It can't validate certificates "properly", and it doesn't check revokation lists.
 * It runs out of stack with a 2048 or 4096 length key; 1024 length is the best you will get
 * So we give the client the sha1 fingerprint of the server certificate, here baked into the code
 * and if the server presents a different certificate at runtime, we drop the link.
 *
 * The constant is a space separated list of exactly 20 hex characters
 * Found by running
 * openssl x509 -in CERT.pem -noout -sha1 -fingerprint | tr ':' ' '
 * (or you could use sed instead of tr)
 */
const char* mqtt_fingerprint = "00 11 22 33 44 55 66 77 88 99 a0 b1 c2 d3 e4 f5 66 77 88 99";


// mqtt client settings
// Change "heatpump" to be same on all lines
const char* ha_entity_id                        = "Lounge Heat Pump"; // Device Name displayed in Home Assistant
const char* client_id                           = "lohp"; // WiFi hostname, OTA hostname, MQTT hostname
const char* heatpump_topic                      = "hp/lohp"; // MQTT topic, must be unique between heat pump unit
//const char* heatpump_availability_topic         = "hp/lohp/tele/lwt";
const char* heatpump_state_topic                = "hp/lohp/tele/stat";
const char* heatpump_temperature_topic          = "hp/lohp/tele/temp";
const char* heatpump_led_topic                  = "hp/lohp/tele/led";
const char* heatpump_attribute_topic            = "hp/lohp/tele/attr";
const char* heatpump_mode_command_topic         = "hp/lohp/cmnd/mode";
const char* heatpump_temperature_command_topic  = "hp/lohp/cmnd/temp";
const char* heatpump_led_command_topic          = "hp/lohp/cmnd/led";
const char* heatpump_fan_mode_command_topic     = "hp/lohp/cmnd/fan";
const char* heatpump_swing_mode_command_topic   = "hp/lohp/cmnd/vane";
const char* heatpump_debug_topic                = "hp/lohp/debug";

// using a similar structure to Tasmota to make all my Last-Wills easier to find
const char* heatpump_lastwill_topic     = "tele/lohp/LWT";
const char* heatpump_lastwill_message   = "Offline";
const char* heatpump_online_message     = "Online";

// Customization
const char* min_temp                    = "16"; // Minimum temperature, check value from heatpump remote control
const char* max_temp                    = "31"; // Maximum temperature, check value from heatpump remote control
const char* temp_step                   = "1"; // Temperature setting step, check value from heatpump remote control
const char* mqtt_discov_prefix          = "homeassistant"; // Home Assistant MQTT Discovery Prefix


// pinouts
const int redLedPin  = 0; // Onboard LED = digital pin 0 (red LED on adafruit ESP8266 huzzah)
const int blueLedPin = 2; // Onboard LED = digital pin 0 (blue LED on adafruit ESP8266 huzzah)

const uint8_t resetPin   = 0;  // Prototype uses GPIO0 as a flash/run button only
const uint8_t rgbLedPin  = 2;  // Mostly-free GPIO on ESP01
const uint8_t swRXPin    = 13; // GPIO13 MOSI/CTS0/RXD2
const uint8_t swTXPin    = 15; // GPIO15 CS/RTS0/TXD2, value at power on matters
const uint8_t hwRXPin    = 3;  // GPIO3 RXD0
const uint8_t hwTXPin    = 1;  // GPIO1 TXD0

// sketch settings
const unsigned int SEND_ROOM_TEMP_INTERVAL_MS = 60000;
