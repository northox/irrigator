# ESP8266 Irrigation Manager

A lightweight, MQTT-driven irrigation controller based on the ESP-12F.  Control up to three watering zones manually via MQTT (or Home Assistant), schedule them for specific weekdays/times, and Home Assistant will hold off watering if rain is expected.

## Key Features

- **MQTT Manual Control**  
  – Topics: `irrigation/control0`, `…/control1`, `…/control2`  
  – Payloads: `on` / `off`  
- **Weather-Aware Watering**  
  - Home Assistant checks your local rain-probability sensor and skips any “on” if precipitation ≥ your threshold.  
- **30 min Auto-Off Timeout if no WIFI/MQTT**  
  – Each zone resets its own 30m watchdog on every “on”  
- **Easy Home Assistant Integration**
  – Pre-built `configuration.yaml` & `automations.yaml` snippets  
- **Wi-Fi Auto-Reconnect**  
  – Ticker-driven rejoin every 30s if disconnected  
- **Hardware Watchdog**
  – Reboots the ESP if it ever locks up for > 5 min

## Prerequisites

1. **ESP8266** (NodeMCU or bare `ESP-12F`)  
2. **3-channel relay module** (active LOW) wired to D8/D7/D6 (GPIO 15/13/12)  
3. **Home Assistant** (Core or OS) with MQTT broker  
4. **OpenWeatherMap API key** (for rain-probability sensor)

## Home Assistant Integration

### MQTT Switches

Add to configuration.yaml (or include via switches.yaml):

```yaml
mqtt:
  switch:
    - name: "Irrigation Zone 0"
      command_topic: "irrigation/control0"
      payload_on: "on"
      payload_off: "off"
      qos: 1
      optimistic: true

    - name: "Irrigation Zone 1"
      command_topic: "irrigation/control1"
      payload_on: "on"
      payload_off: "off"
      qos: 1
      optimistic: true

    - name: "Irrigation Zone 2"
      command_topic: "irrigation/control2"
      payload_on: "on"
      payload_off: "off"
      qos: 1
      optimistic: true
```

Restart HA to expose:
- switch.irrigation_zone_0
- switch.irrigation_zone_1
- switch.irrigation_zone_2

### Weather Sensor

In your configuration.yaml (or split into sensors.yaml):

```yaml
weather:
  - platform: openweathermap
    api_key: YOUR_OPENWEATHERMAP_API_KEY
    mode: hourly
    name: home_weather

sensor:
  - platform: openweathermap
    api_key: YOUR_OPENWEATHERMAP_API_KEY
    monitored_conditions:
      - precipitation_probability
    name: rain_probability
```

This creates:
- weather.home_weather (hourly forecast)
- sensor.rain_probability (next-period % chance of rain)

### Time-Based Automations With Rain Check

Place in automations.yaml:

```yaml
# Zone 0 – Morning watering on Tue & Fri if dry
- alias: "Zone 0 – Morning Watering (Tue & Fri, if dry)"
  trigger:
    platform: time
    at: "06:00:00"
  condition:
    - condition: time
      weekday: [ tue, fri ]
    - condition: numeric_state
      entity_id: sensor.rain_probability
      below: 50
  action:
    - service: switch.turn_on
      entity_id: switch.irrigation_zone_0

# Zone 0 – Stop watering (always)
- alias: "Zone 0 – Stop Morning Watering (Tue & Fri)"
  trigger:
    platform: time
    at: "06:30:00"
  condition:
    condition: time
    weekday: [ tue, fri ]
  action:
    - service: switch.turn_off
      entity_id: switch.irrigation_zone_0
```

Repeat for Zone 1 and Zone 2, adjusting 'at:' times and days as needed, and including the same rain_probability check in the “on” automations.

### Lovelace Dashboard

```yaml
type: entities
title: Garden Irrigation
entities:
  - switch.irrigation_zone_0
  - switch.irrigation_zone_1
  - switch.irrigation_zone_2
  - sensor.rain_probability
  - weather.home_weather
```

Now you have manual toggles, plus automated schedules on selected weekdays.

## Future Plans

- **Plant Mood Detection™**: Snap a picture of your sad ferns and run them through a neural net. If they give you the stink eye, trigger an emergency sprinkling.
- **Quantum Soil Moisture Sensor**: Uses Schrödinger’s moisture principle: the soil is both wet and dry until you observe it. More research needed.
- **Interplanetary Irrigation**: When Mars colonization kicks off, automatically switch to red-dust-resistant nozzles and dust-storm mode. Earth mode still works if Elon sends you potable water.
- **Astrological Alignment**: “Mercury in retrograde? Best hold off. Full moon in Leo? Water twice.” Because the cosmos knows best.
- **Plant-GPT Integration**: Chat with your basil: “Hey Basil, you look thirsty. Would you like a drizzle or a deluge?” Basil: “Yes.”
- **Ultimate Goal**: Replace roving sprinklers entirely. Let robots mounted on drones deliver a precisely calibrated droplet to each leaf—because why not?