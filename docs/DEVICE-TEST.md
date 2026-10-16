# Plaque OS beta 0.1 on the X4 Pro: a short device test

Beta 0.1 is Plaque OS with the X4 Classic capability layer, the university Wi-Fi profile and the Register sleep screen
(an instrument's schedule). **Nothing in it has run on a reader.** Flashing can brick a device: read the warning in the
[README](../README.md) first. Do the steps in order and stop at the first one that is wrong: a later step leans on the
ones before it.

On the X4 Pro the controls are the Left and Right side buttons, Power, the Home pad (the touch key below the screen) and
taps. Swipe in from the left edge of the screen to leave a screen. There are no other keys.

In the commands below `$REPO` is your checkout of this repository, `$PORT` is the reader's serial port, and
`./build/` is any folder you keep the files in (outside the repository's tracked files; it is ignored by Git).

You need:

- the image you built: `$REPO/.pio/build/x4pro/firmware.bin` from `pio run -e x4pro` (see the README), checked with
  `python3 $REPO/test/check_firmware_image.py --board x4pro $REPO/.pio/build/x4pro/firmware.bin`. Copy it to
  `./build/plaque-x4pro.bin` and note its size and SHA-256 (`shasum -a 256`). The commit it was built from is the part of
  the firmware line in About after the `+`. Never flash the `x4c` image on a Pro.
- **a backup of the app slot you are about to overwrite, made first** (section 0). Without it there is no way back except
  USB flashing of some other image, or the SD-card firmware update.
- the Plaque Mac app (github.com/samporter14/plaque), a Google Calendar for one instrument added to your reader (Settings >
  Readers > the reader > Instrument calendar), and the mailbox live. The reader's mailbox URL, device ID and token are
  set already (see "Mailbox configuration" in the README).

## 0. Read the partition table and back up app1

The serial logger holds the reader's port: stop it first. List the ports, and put the reader's in `PORT` (if there is more
than one, unplug the reader, list again, and the one that went is the reader):

```
ls /dev/cu.usbmodem*
PORT=/dev/cu.usbmodem1101
```

Read the partition table and compare it with `$REPO/partitions-x4pro.csv`: app0 at 0x10000 and app1 at 0x7F0000, each
0x7E0000 long. If it differs, stop: this guide is for that table only.

```
mkdir -p ./build
esptool --chip esp32s3 --port "$PORT" --baud 460800 read-flash 0x8000 0xC00 ./build/ptable.bin
```

Then save the app1 slot, the one these steps overwrite, and check it twice:

```
esptool --chip esp32s3 --port "$PORT" --baud 460800 read-flash 0x7F0000 0x7E0000 ./build/app1-backup.bin
shasum -a 256 ./build/app1-backup.bin
```

(esptool 5.x spells it `read-flash` and `write-flash`; esptool 4.x spells them `read_flash` and `write_flash`.) Keep the
backup: it holds whatever is on your reader now, which may be the stock firmware. Do not publish it.

## 1. Flash

In a terminal:

```
shasum -a 256 ./build/plaque-x4pro.bin
```

Write the image into the app1 slot:

```
esptool --chip esp32s3 --port "$PORT" --baud 460800 --after watchdog-reset write-flash 0x7F0000 ./build/plaque-x4pro.bin
```

(`--after watchdog-reset` restarts the reader when the write ends.) Start the logger again.

**Expect:** the reader restarts by itself and shows Home. If it does not boot, go to the rollback in section 9. If it boots
but About (next section) does not show the beta, the reader may be booting from app0: do not flash again, and open an issue
with what About shows.

## 2. About, and the card

1. Tap Settings > About. **Expect:** the firmware line reads `1.6.5-x4pro+<commit>.<branch>` (the commit and branch you built from), with **no** `.dirty`; the Model
   row says `Xteink X4 Pro`; there is a Touch row and a Frontlight row.
2. Power the reader off, take the card out, and read `/Lamina/reader.json` on the Mac. **Expect:** `"schema": 2`,
   `"model": "x4pro"`, and `"capabilities"` ending `..., "wifi_eap_tls", "register"]`. Also read
   `/Lamina/network-profile.json`: `{"schema":1,"installed":false}` (or the installed profile's description). Both files are
   written once, at the first boot of this firmware (`reader.json` because it gained the `register` word, the other because
   it did not exist before); a second boot writes neither.

## 3. Home and Method, as before

1. Home shows Method. Tap it; the shelf opens at once, with no wait for the network.
2. Open a book. Turn pages with the Right and Left side buttons and by tapping.
3. On a page that has a timer, tap the timer box to start it. **Expect:** the running box reads "Tap to stop · ends ...".
   Tap it, then tap **Stop** (not "Keep running") on the question.
4. Press Power to sleep inside the book. **Expect:** the step stays on the panel with the moon mark; Power wakes to the
   same page.

Anything that is not as described is a bug: write down what you did and what you saw.

## 4. Messages and the Register rows

0. Before you change anything, write down how these three rows are set now (you will want them back after a rollback):
   Latest message as sleep screen, Check while sleeping, Polling interval.
1. Tap Settings > System > Messages. **Expect:** twelve rows: View latest message; Check for message now; **View the
   register; Check the register now**; Mailbox base URL; Device ID; Device bearer token; Polling interval; Latest message
   as sleep screen; **Register as sleep screen**; Check while sleeping; Allow local development HTTP.
2. Tap Register as sleep screen first. **Expect:** it ticks and Latest message as sleep screen unticks. If Check while
   sleeping was off it ticks too, the Polling interval becomes **15 min** (whatever it was), and a line under the row says
   "Check while sleeping is on, every 15 min." If it was already on, the interval is left as it was.
3. Then tap Polling interval and set it to **5 min** for this test (ticking Register after setting the interval would have
   overwritten it).

## 5. The Register screen, with the real calendar

1. Tap Check the register now. **Expect:** "Reading the calendar...", then the screen: the instrument's name; **Free** or
   **In use** (with who and until when); Next; Today and Tomorrow; a footer "As of <time>".
2. Compare it with the Google Calendar of that instrument: the booking in progress, the next one, today's and
   tomorrow's. The times must be the instrument's own, whatever the reader's time zone is.
3. Press the Right side button (or tap): the full day, page 1, page 2, ..., then the screen again. The Left side button
   goes the other way. The footer of each page says which button does what. Swipe in from the left edge: back to the
   screen; once more: out of Register. The Home pad: Home, from anywhere.
4. Turn the Wi-Fi off on the router (or move out of range) and tap Check the register now again. **Expect:** the saved
   schedule, with "The check failed. This is the saved schedule." in its footer.
5. Join-by-profile (only if a network profile is installed on this reader and it is in range): repeat step 1 with the saved
   network forgotten or out of range. **Expect:** it joins through the profile and reads the calendar.

## 6. Sleep on the Register screen, a boundary wake and a poll wake

Put a booking in the calendar that **starts about 12 minutes from now**, and make sure none is in progress. The reader
only knows about it once the Worker has read the calendar again (about as many minutes as you have instrument calendars)
and the reader has asked: so tap Check the register now, again and again, until the booking shows under Next, and only
then go on. A reader put to sleep before that has no wake armed for the booking, and step 2 would fail for that reason.

1. From Home, press Power to sleep. **Expect:** the panel shows the Register screen ("Free", the booking next), and
   nothing else on top of it (no moon mark).
2. Leave the reader alone. At the booking's start (the reader wakes a few seconds early) the panel redraws by itself to
   **In use**. **Expect:** the footer's "As of" time has *not* changed: a boundary wake draws and does not touch the
   network. In the log: `Sleeping for ... s, for a boundary`, then `Redraw: the screen changed`.
3. Edit the booking in Google Calendar (move its end by 30 minutes). Wait for the next poll (the interval is the 5 minutes you set in section 4, counted from the last poll, once
   the Worker has read the calendar again, which takes about as many minutes as you have instrument calendars). **Expect:**
   the panel redraws with the new end and a new "As of" time; the log shows `Schedule read: ...`.
4. Press Power to wake the reader, and tap Home. **Expect:** Home, as always.

## 7. A step timer with Register

1. Open the Method book and start a timer on a page (tap its box). Press Power to sleep. **Expect:** the panel keeps the *step* (the Method
   step outranks Register), with "Ends ...". Register does not replace it.
2. Leave it. With a step on the panel Register arms no boundary wakes, only its poll; a poll that falls due within the last
   2.5 minutes of the timer is skipped, so the alert is not late (the log says `Poll skipped: the timer ends in ... ms`; with
   a network profile installed the window is 160 or 175 seconds).
3. At zero the reader wakes, the timer box inverts, the light pulses, and it stays awake. Tap to answer. **Expect:** the
   same page, the timer marked done.
4. Press Home, then Power to sleep again. **Expect:** the Register screen on the panel once more.

## 8. Reset the clock check

1. Tap Settings > System > Clock > Reset the clock check. **Expect:** a question with "Use this if the reader shows a
   date in the future and can't connect." and **Keep** highlighted.
2. Tap Keep: nothing changes. Open it again and tap **Reset**: "Done. The clock is set again the next time the reader
   connects."
3. Before connecting, tap View the register. **Expect:** the schedule with **Not known** where it said Free or In use (the
   reader has no time until a sync: it will not guess).
4. Tap Check the register now. **Expect:** it reads the calendar (the clock is set again by that connection), and the
   Register screen is right: the day, the times and "As of" are correct.

## 9. Rollback

Stop the logger. Use the same `PORT` as in section 0. To go back to what was on the reader before, write back the backup you
made in section 0:

```
esptool --chip esp32s3 --port "$PORT" --baud 460800 --after watchdog-reset write-flash 0x7F0000 ./build/app1-backup.bin
```

To go back to a Plaque OS image you ran before, write that file the same way. The SD-card firmware update (Settings > System >
SD Card Firmware Update) is the other way back, and needs no cable.

The card keeps what beta 0.1 wrote: `/Lamina/reader.json` (schema 2, with the `register` word, which older firmware and the Mac
ignore), `/Lamina/network-profile.json`, `/Lamina/register.json` and the settings in `/.crosspoint/ambient-config.json`. An
older firmware ignores the Register setting, but ticking Register in section 4 changed three others: it **unticked Latest
message as sleep screen**, it may have **ticked Check while sleeping**, and it **set the Polling interval** (5 min). After
a rollback the note is therefore not the sleep screen, and the reader does not poll for it, until you tick Latest message as
sleep screen again (Settings > System > Messages) and put the interval back to what you wrote down in section 4. Delete
`/Lamina/register.json` if you want the card as it was.

## 10. What to write down

For each step: it worked, or what you did and what the panel showed, plus the log lines around it. The things this beta
most needs a reader for: that the Register panel survives a boundary wake (section 6, 2), that a poll over the saved
network or the profile updates it (6, 3), and that a Method step is never drawn over (7, 1).

## X4 Classic: not tested on a real Classic

Nothing in this beta has run on an X4 Classic. Before the Classic image is flashed at all, read the Classic's partition
table (`docs/X4C-WIFI-DEVICE-ACCEPTANCE.md`, section 0) and compare it with `partitions-x4c.csv`. Install it from the SD
card (Settings > Install from card), not by esptool. The Classic has no touch, no Home pad and no light: its keys are the
side Up and Down keys, Power and the four bottom keys (Left, Right, Confirm, Back).

On a Classic, with the same calendar:

1. Settings > System > Messages shows the same twelve rows. Up and Down move; Confirm chooses; Back leaves.
2. Check the register now: the screen as above. Right goes to the next page of the full day, Left to the previous one
   (the footer says "Right: next", not "tap"); Back returns to the screen, and from the screen leaves.
3. Sleep on the Register screen: the panel keeps it, and a boundary wake and a poll wake behave as on the Pro. **Unverified:**
   that the panel image survives a timer wake on a Classic at all (`x4c-support.md`, "Not verifiable without a reader").
4. Step timer: the running box reads "Confirm: stop · ends ...", the alert has no light, and any key answers it.
