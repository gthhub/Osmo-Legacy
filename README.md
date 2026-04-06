Built by Ty Builds
[@tybuilds_](https://x.com/tybuilds_) 
*tech for techless sake*


~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Osmosis - the diffusion of water or other solvents through a semipermeable membrane (one that blocks the passage of dissolved substances—i.e., solutes)

A natural, passive filter
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~



---

****NOTE: This firmware is the inital firmware release for iPhone use. Not compatible with further updates and companion apps.

---




# Osmo Notifier

A pocket-size e-ink notification pager. Shows filtered iPhone notifications on a non-glowing, non-distracting paper screen. One button to dismiss. That's it.

I built this because I wanted a way to stop checking my phone without feeling completely disconnected. To functionally disconnect without fully *being* disconnected. 
~
This is the freedom of what is effectively a ~modern day pager~. It lets me drop my phone in a drawer / bag out of reach to make a true physical barrier, while not feeling like I'm completely disconnected from important stuff I may be needed for (like still getting notifications from my family or urgent work pings and such). Now, I only go grab my phone IF something truly needs action.

I've been using it as an (intentionally and literally) tiny window into my digital life. My phone is out of reach 95% of the day. 

Hope you can enjoy it too.

Please star this on Github to receive updates for this project!

---



## What it does

- Receives iPhone notifications via BLE (ANCS)
- Shows one notification at a time on a 1.54" e-ink display
- How to use:
  1. Single button: short press to dismiss, double press to toggle mode, triple press to clear all
  2. Single press that same button while no notifications are showing to get the "Still connected" proof screen, giving you peace of mind (and if you've received at least one notif since boot up, it'll show the current time there too!)

- Two modes: **All** (every notification) or **Filtered** (whitelist only)
- Battery indicator, RTC time display, auto-reconnect on disconnect

## What it doesn't do

No scrolling. No feeds. No replies. No apps. No keyboard. No settings UI on the device.

If something needs action, you reach for your phone intentionally. If not, dismiss and move on. Glance over to know you're still good. 

---

## Hardware

**Waveshare ESP32-S3 1.54" e-Paper Module** (~$25 on Amazon + buy a battery, even less direct from waveshare)

Direct: https://www.waveshare.com/esp32-s3-epaper-1.54.htm?srsltid=AfmBOoqwqe976edHKiOOEdibZsqfdZR157A26Mu9u9ND_rKFB2AjP8JO

Amazon (need to buy a 1.25jst battery though too in this case): https://a.co/d/0fAMwlll

Battery I used that fits in case: https://www.amazon.com/dp/B0FRFY1F76?ref=ppx_pop_mob_ap_share

That's it. No custom PCB, no special parts. This is the exact board I'm using.

---

## Setup

### Requirements
- Arduino IDE with ESP32 board support
- Libraries (install these beforehand thru Arduino IDE):
  - `NimBLE-Arduino`
  - `GxEPD2`
  - `ArduinoJson`

### Flash
1. Clone this repo and install the above libraries
2. Open `OsmoFirmware.ino` in Arduino IDE
3. Select board: `ESP32S3 Dev Module`
4. Flash

### Pair with iPhone
1. Power on Osmo — it will show "Waiting for iPhone..."
2. Go to iPhone **Settings → Bluetooth**
3. Pair with **"Osmo"**
4. Accept the pairing request
5. Done — notifications will start flowing

> First pairing takes ~10 seconds. It bonds, so reconnection should be mostly automatic after that.

---

## Limitations

- iPhone only (ANCS is Apple's protocol)
- Code is fully functional but not polished. Use at your own risk.

---

## Why

> Phones create continuous pull. This creates discrete push. Get back to living, not reacting.

Built by Ty Builds
[@tybuilds_](https://x.com/tybuilds_) 
— *tech for techless sake*