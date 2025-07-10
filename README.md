# ESP8266 Irrigation Manager

A lightweight, MQTT-driven irrigation controller based on the ESP-12F.  Control up to three watering zones manually via MQTT (or Home Assistant), schedule them for specific weekdays/times, and rest easy.

## Key Features

- **MQTT Manual Control**  
  – Topics: `irrigation/control0`, `…/control1`, `…/control2`  
  – Payloads: `on` / `off`  
- **30 min Auto-Off Timeout if no WIFI/MQTT**  
  – Each zone resets its own 30m watchdog on every “on”  
- **Easy Home Assistant Integration**
  – Pre-built `configuration.yaml` & `automations.yaml` snippets  
- **Wi-Fi Auto-Reconnect**  
  – Ticker-driven rejoin every 30s if disconnected  
- **Hardware Watchdog**
  – Reboots the ESP if it ever locks up for > 5 min

### Home Assistant Integration

#### MQTT Switches

Add to configuration.yaml (or include via switches.yaml):

```
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

Restart HA to see switch.irrigation_zone_0, _1, _2.

### Time-Based Automations

Place in your automations.yaml:

```
- alias: "Zone 0 – Morning Watering (Tue & Fri)"
  trigger:
    platform: time
    at: "06:00:00"
  condition:
    condition: time
    weekday:
      - tue
      - fri
  action:
    - service: switch.turn_on
      entity_id: switch.irrigation_zone_0

- alias: "Zone 0 – Stop Morning Watering (Tue & Fri)"
  trigger:
    platform: time
    at: "06:30:00"
  condition:
    condition: time
    weekday:
      - tue
      - fri
  action:
    - service: switch.turn_off
      entity_id: switch.irrigation_zone_0
```

 Repeat the above pair for zone_1 and zone_2, adjusting times and days as needed.

### Lovelace Dashboard

```
type: entities
title: Garden Irrigation
entities:
  - switch.irrigation_zone_0
  - switch.irrigation_zone_1
  - switch.irrigation_zone_2
```

Now you have manual toggles, plus automated schedules on selected weekdays.

## Future Plans

- **Weather-Aware Watering**: Integrate with a weather API so your system holds off when rain is predicted—because clouds deserve a shot at the job too.
- **Plant Mood Detection™**: Snap a picture of your sad ferns and run them through a neural net. If they give you the stink eye, trigger an emergency sprinkling.
- **Quantum Soil Moisture Sensor**: Uses Schrödinger’s moisture principle: the soil is both wet and dry until you observe it. More research needed.
- **Interplanetary Irrigation**: When Mars colonization kicks off, automatically switch to red-dust-resistant nozzles and dust-storm mode. Earth mode still works if Elon sends you potable water.
- **Astrological Alignment**: “Mercury in retrograde? Best hold off. Full moon in Leo? Water twice.” Because the cosmos knows best.
- **Plant-GPT Integration**: Chat with your basil: “Hey Basil, you look thirsty. Would you like a drizzle or a deluge?” Basil: “Yes.”
- **Ultimate Goal**: Replace roving sprinklers entirely. Let robots mounted on drones deliver a precisely calibrated droplet to each leaf—because why not?