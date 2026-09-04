# Sunrise alarm: the Home Assistant side

The fade itself runs on the ESP32. Home Assistant has exactly one job here —
keep the device's `Alarm time` pointing at the next alarm on your phone — and
one automation does it. Once that value is written, waking up does not depend
on Home Assistant, the VPN, or Wi-Fi.

That split is the whole design. The alarm is set at night, when the phone and
HA are both awake and talking; the sunrise happens hours later, when only the
ESP32 needs to be. See [README > Sunrise alarm](../README.md#sunrise-alarm) for
what the device does with the value.

---

## What it hooks into

**Android's `AlarmManager`, by way of the Home Assistant Companion app.**

The companion app exposes a `Next Alarm` sensor built on
`AlarmManager.getNextAlarmClock()`, which is an OS-level call: it returns the
next alarm registered by *any* app that uses the standard alarm API. Google
Clock, the Samsung and Xiaomi clocks, Sleep as Android — all of them register
there, so none of them needs its own integration and nothing has to be
configured per app.

It also means there is nothing to install beyond the companion app you probably
already have, and no webhook, no cloud, no third-party service in the path.

### Enabling the sensor

It ships **disabled**. On the phone:

> Companion app → **Settings** → **Manage sensors** → **Alarm** → **Next alarm**
> → toggle **Enable sensor**

It will then appear in Home Assistant as `sensor.<your_phone>_next_alarm`, with
a UTC ISO-8601 timestamp for a state. It updates on Android's
`ACTION_NEXT_ALARM_CLOCK_CHANGED` broadcast, so it follows an alarm being set,
changed or cancelled within seconds rather than at the next sensor poll.

**Check the state before writing the automation.** Developer Tools → States,
filter on `next_alarm`. If it is `unavailable`, the sensor is not enabled or
the phone has no alarm set.

---

## The entities on the device

| Entity | |
|---|---|
| `datetime.smarterfan_relay_alarm_time` | The wake time. **This is the one the automation writes.** |
| `switch.smarterfan_relay_sunrise_alarm` | Master enable. Off and nothing arms. |
| `number.smarterfan_relay_sunrise_duration` | Minutes before the alarm that the fade starts. Default 30. |
| `number.smarterfan_relay_sunrise_end_brightness` | Where the fade ends. Default 100%. |
| `number.smarterfan_relay_sunrise_end_colour_temperature` | Where the colour ends. Default 3800 K. |
| `sensor.smarterfan_relay_sunrise_progress` | 0–100% while a fade runs, 0 otherwise. |
| `button.smarterfan_relay_sunrise_test` | Runs the whole curve in 60 s. |
| `button.smarterfan_relay_sunrise_cancel` | Stops a running fade, leaves the light alone. |

Entity IDs are derived from the device's `friendly_name`; if yours differs,
adjust. All of them are also on the device's own web page at
`http://smarterfan-relay.local/`, which is how you set an alarm with no Home
Assistant at all.

---

## The automation

```yaml
automation:
  - alias: "SmarterFan: mirror the phone's next alarm"
    id: smarterfan_mirror_next_alarm
    description: >
      Keeps the fan's `Alarm time` equal to the next alarm registered on the
      phone. The device does everything else.
    mode: single

    triggers:
      # The alarm was set, changed, snoozed or cancelled.
      - trigger: state
        entity_id: sensor.pixel_next_alarm

      # The device came back -- an OTA, a watchdog, a power cut. It restores
      # its own alarm time across a reboot, so this is belt and braces.
      - trigger: state
        entity_id: datetime.smarterfan_relay_alarm_time
        from: unavailable

      # Home Assistant itself restarted.
      - trigger: homeassistant
        event: start

      # Self-healing: if the device was unreachable at the moment the alarm was
      # set, the state trigger fired into the void and nothing would notice.
      - trigger: time_pattern
        hours: "/1"

    actions:
      - action: datetime.set_value
        target:
          entity_id: datetime.smarterfan_relay_alarm_time
        data:
          datetime: >-
            {% set a = states('sensor.pixel_next_alarm') %}
            {% if a not in ['unknown', 'unavailable', 'none', ''] %}
              {{ as_local(as_datetime(a)).strftime('%Y-%m-%d %H:%M:%S') }}
            {% else %}
              2000-01-01 00:00:00
            {% endif %}
```

Replace `sensor.pixel_next_alarm` with your phone's sensor in both places — the
trigger and the template (and in the optional condition below, if you use it).

**Adding the automation does not run it.** An automation only runs when a
trigger fires, so `Alarm time` stays `unknown` until one does: you next set or
change an alarm on the phone, Home Assistant restarts, or the hourly
`time_pattern` comes round. To write it immediately, open the automation and
use the three-dot menu → **Run**, which executes the actions without waiting
for a trigger.

**Three things that automation is doing deliberately:**

- **`as_local`.** The sensor's state is UTC; the device stores wall-clock time
  in the zone named by `timezone:` in `relay.yaml`. `as_local` converts using
  Home Assistant's configured timezone, so **HA's timezone and `relay.yaml`'s
  `timezone:` have to be the same zone.** They disagree silently, and the
  symptom is a sunrise that is a whole number of hours off.

- **The `2000-01-01` fallback.** Cancelling tomorrow's alarm has to reach the
  device, or it would still fade at 06:00. Writing a time far in the past
  disarms it, because what arms a fade is a *window* — the half-hour before the
  alarm — and a time in 2000 has no window left.

- **The hourly `time_pattern`.** Writing to an offline ESPHome device fails
  quietly. Re-pushing the same value every hour costs nothing and closes the
  window where the phone's alarm and the fan's disagree.

### Optional: ignore alarms from certain apps

`getNextAlarmClock()` returns whatever app set the alarm, including ones you
did not mean as a wake-up. The sensor carries the package name as an attribute:

```yaml
    conditions:
      - condition: template
        value_template: >-
          {{ state_attr('sensor.pixel_next_alarm', 'Package')
             in ['com.google.android.deskclock', 'unknown'] }}
```

`unknown` is there so that clearing the alarm still gets through to the
`2000-01-01` branch.

---

## Sleep as Android

Sleep as Android registers its alarms through `AlarmManager` like everything
else, so the automation above already works with it — with one limitation worth
knowing about.

Its **smart wake-up** deliberately rings *early*, at a light point in your sleep
cycle, anywhere up to 30 minutes before the alarm you set.
`getNextAlarmClock()` reports the alarm you set, not the moment it decides to
ring. With a 30-minute sunrise and a 30-minute smart window, the light can
still be at a third of its brightness when the phone goes off.

If that matters, Sleep as Android can call a Home Assistant webhook directly
(**Settings → Automation → Webhook / HTTP request**, on the `smart_period`
event) and that automation can write `Alarm time` to `now() + a few minutes` to
pull the sunrise forward. The device does not care which automation writes the
value.

---

## Checking it works

1. **The sensor.** Developer Tools → States → `sensor.<phone>_next_alarm`. Set
   an alarm on the phone; the state should change within seconds.
2. **The mirror.** Developer Tools → States →
   `datetime.smarterfan_relay_alarm_time`. It should match, in local time.
3. **The fade.** Press `button.smarterfan_relay_sunrise_test`. The light should
   go from off to full over a minute, starting at its dimmest and warmest.
4. **The real thing.** Set an alarm for ~35 minutes out and watch
   `sensor.smarterfan_relay_sunrise_progress`, or the device log:

   ```
   [I][sunrise]: starting, 1800 s of fade to the alarm
   [I][sunrise]: complete
   ```

### When nothing happens

| Symptom | Cause |
|---|---|
| `Alarm time` never changes | The `Next alarm` sensor is disabled on the phone, or the entity ID in the automation is wrong |
| Fade starts an hour early or late | HA's timezone and `relay.yaml`'s `timezone:` disagree |
| Nothing fires, `Alarm time` is right | `switch.smarterfan_relay_sunrise_alarm` is off, or the device's clock never synced — check `Uptime` and the log for SNTP |
| Log says "the light is already on -- skipping" | Working as intended: a sunrise will not dim a room that is already lit. Turn the light off |
| Fade stops partway | Something touched the light. Any brightness or colour change during a sunrise cancels it |
