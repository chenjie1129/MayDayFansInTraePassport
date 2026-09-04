<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

# Bubu Passport

> A quiet way for Mayday fans to recognize one another in a crowd.

![Bubu's six growth forms](../main/assets/preview_pet/forms_light_x6.png)

Bubu Passport is an anonymous encounter game made for Mayday fans.

It does not ask people to become friends or exchange names and contact details. When two nearby Passports recognize the same circle, each device shows a small sign:

**"Another fan is here."**

Sometimes being recognized without being interrupted is enough.

## The three ideas

### 1. Wear a secret sign

Before heading out, choose a symbol:

- carrot
- rabbit
- nine ball
- a red, yellow, blue, green, or pink ball

You can also choose a short status such as "HELLO," "COFFEE BREAK," or "NEED HELP."

When another Passport from the same circle is nearby, both devices show an encounter. There is no friend request and no social obligation.

### 2. The nine ball is a mood, not an identity

The nine ball means, "Today is not great, but I may not want to explain."

Receiving it does not trigger a lively animation or sound. It only shows a quiet line of text. The other fan can press `OK` to respond:

**"Someone nearby stayed with you for a moment."**

The response is addressed only to the anonymous Passport from that encounter. It does not create a friend relationship or reveal contact details.

### 3. Take Bubu to more places

Bubu remembers one thing: how many different places you have visited together.

**Seed -> Sprout -> Young Bubu -> Bubu -> Traveler -> World Bubu**

Bubu only grows. Staying home for a while never removes progress or erases the road already traveled.

## How to play together

### With one Passport

One device is enough to raise Bubu, discover places, review the current journey, and unlock all six growth forms.

### With two Passports

1. Open `Places` on both devices.
2. Choose a symbol under `TRIBE ICON`.
3. Turn `STEALTH` off and keep the devices nearby.
4. Wait for automatic discovery; a result normally appears within one roughly 60-second cycle.
5. When a nine-ball signal appears, stay on `LIVE` and press `OK` to send a quiet reply.

There is no pairing ceremony. It is closer to noticing a familiar shirt in the crowd after a concert: you recognize each other, then keep walking.

## More things to find

- **Place archive:** remembers familiar environments without storing GPS coordinates.
- **Current trail:** shows the places visited since the latest boot.
- **Regulars:** when explicitly enabled, counts repeat encounters by anonymous ID only.
- **Gathering memory:** saves a small memento when many fans are nearby.
- **Stealth mode:** observe nearby activity without announcing yourself.
- **Reset identity:** generate a fresh anonymous ID whenever you choose.

## Three buttons are enough

| Button | Action |
| --- | --- |
| `UP` / `DOWN` | Move between pages or choices |
| `OK` | Confirm, change a sign, or answer a nine-ball signal |
| Hold `OK` | Return to the main menu |

## We do not want to know who you are

- No names, phone numbers, or social accounts are exchanged.
- There is no free-text chat, reducing unwanted contact.
- GPS location is never recorded.
- Encounters use a random anonymous ID that can be regenerated manually.
- Place, growth, and regular-encounter records stay on the device.

This is not a tool for collecting more contacts. It is a lighter kind of company: knowing someone else is present can be enough.

## Get and install it

Download a complete firmware image from this repository's [Releases](https://github.com/chenjie1129/MayDayFansInTraePassport/releases), then install it with the [AI Passport web flasher](https://ai-passport.folotoy.cn/tools/web-flasher/).

Use the project's `FoloToy-AI-Passport-full.bin` and preserve the device identity and recovery regions.

## For fans who want to build with us

The engineering material remains available, but it no longer has to be the first thing every visitor reads:

- [Documentation index](INDEX.md)
- [Build and test guide](development/build-and-test.md)
- [Firmware compatibility and protected partitions](development/ble-recovery-compatibility.md)
- [Contribution guide](../.github/CONTRIBUTING.md)

The project uses ESP-IDF and LVGL. Its protocol, place matching, regular eviction, and Bubu growth rules have host-side tests that run without hardware.

## Note

This is a personal, non-commercial fan project and is not an official product of Mayday or its related teams. See the [asset notice](../main/assets/source/NOTICE.md) for the source and usage boundaries of the Bubu reference artwork.
