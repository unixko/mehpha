# MitsuCon
Mitsubishi Heat Pump (Air Conditioner) Controller for Home Assistant.

(Mitsubishi Electric / Domestic, not Mitsubishi Heavy Industries models; they use a different protocol)

**Native code works with Home Assistant 0.98.1**

![ESP01 Adapter](https://user-images.githubusercontent.com/43923557/73584206-bc02c680-44fb-11ea-9dc0-10d315d722b7.jpg)

PlatformIO project to build Mitsubishi heat pump controller for Home Assistant. It has 2 branches, classic and native.

## Classic
Older code suited for Home Assistant <=0.96. Unmaintained. Suggest looking at the upstream https://github.com/unixko/MitsuCon instead.

## Native: 
New development code using native MQTT HVAC component built-in Home Assistant, and discovery, without requiring custom components.

Extended by myself to use PlatformIO in place of Arduion, include a WS2812 status LED, MQTTS Encryption support, and a little onboard "Wizard" to try to manage the temperature more better.

The LED is Amber when the controller is off, Blue when the room is colder than desired, Red when the room is warmer than desired, and Green when the room is within 2°C of the target.

As of January 2020, ArduinoOTA is probably broken. Again.

This project is based on https://github.com/SwiCago/HeatPump library.

## Credits
Thanks to all contributors especially:
* unixko  https://github.com/unixko/MitsuCon
* lekobob https://github.com/lekobob/mitsu_mqtt
* SwiCago https://github.com/SwiCago/HeatPump
* Hadley  https://nicegear.co.nz/blog/hacking-a-mitsubishi-heat-pump-air-conditioner/

![Black Heatpump, Blue LED](https://user-images.githubusercontent.com/43923557/73584371-c70a2680-44fc-11ea-9b7b-495027dc0c9b.jpg)

Top right of the unit is a little blue light showing the adapter is in place and the room is colder than desired.
