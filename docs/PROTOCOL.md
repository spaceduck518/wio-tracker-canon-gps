# Canon EOS Bluetooth LE GPS protocol – notes

This is what the firmware does on the air, plus everything learned while testing with an
**EOS R6 Mark II**. The basic protocol was reverse engineered by the
[furble](https://github.com/gkoh/furble) project (MIT license); see also the furble blog post
[Canon GPS Reverse Engineering](https://furble-blog.pages.dev/posts/canon-gps).
Canon does not publish this protocol – everything here is observed behaviour.

## Two pairing modes

A Canon camera offers two different Bluetooth roles, depending on how you start pairing
in the camera menu:

| Camera menu | Advertised service | What it can do |
|---|---|---|
| Wireless remote (BR-E1 compatible) | `00050000-0000-1000-0000-d8492fffa821` | shutter / focus only, **no GPS** |
| Smartphone (Camera Connect) | `00010000-0000-1000-0000-d8492fffa821` | pairing, mode, **GPS** |

The tracker must be paired as a **smartphone**. If it finds a camera in remote mode it
says so on the display ("Remote mode!") and in the serial log.

## UUIDs (all `XXXXXXXX-0000-1000-0000-d8492fffa821`)

| UUID prefix | Type | Use |
|---|---|---|
| `00010000` | service | pairing / identification |
| `00010006` | characteristic | write name, **indications** = pairing result |
| `0001000a` | characteristic | write identification data |
| `00030000` / `00030010` | service / char. | camera mode (not used by this firmware) |
| `00040000` | service | GPS |
| `00040002` | characteristic | write GPS position |
| `00040003` | characteristic | **indications** = GPS request / GPS status |

## Connection sequence

1. Connect, then **encrypt the link**. The camera uses LE "Just Works" bonding
   (no PIN). With an existing bond the link is encrypted with the stored key.
   - Pitfall (Adafruit Bluefruit library): `BLEConnection::requestPairing()` only *starts*
     pairing and returns immediately. You must wait for the link to be secured / the
     pairing-complete callback before writing anything, otherwise the camera rejects writes.
2. Subscribe to indications on `00010006`.
3. Write the pairing request:
   - `0x01` + device name → `00010006`
   - `0x03` + 16-byte device UUID (random, generated once and stored) → `0001000a`
   - `0x04` + device name → `0001000a`
   - `0x05 0x02` → `0001000a`
4. Wait for the indication on `00010006`: `0x02` = accepted, `0x03` = rejected.
   - First pairing: the camera asks the user to confirm; the answer arrives after the user
     presses OK (allow up to ~60 s).
   - Already paired camera: the answer comes after ~1.5 s **or not at all**. The firmware
     waits 3 s and then continues.
5. Subscribe to indications on `00040003`.
6. Write `0x01` → `0001000a` (finishes the handshake).
7. GPS handshake (indications on `00040003`):
   - camera sends `0x03` (+ padding byte) = "please send location" → write `0x01` to `00040002`
   - camera sends `0x02` (+ padding) = GPS accepted → start sending positions
   - camera also sends `0x05 0x04` (meaning unknown, harmless)
   - on reconnects the camera often sends `0x02` directly without asking first.

Skipping step 3/4 for a known camera does **not** work on the R6 Mark II: the camera then
never enables GPS.

## GPS packet (20 bytes, written to `00040002` with response)

| Offset | Size | Content |
|---|---|---|
| 0 | 1 | `0x04` |
| 1 | 1 | `'N'` or `'S'` |
| 2 | 4 | latitude, absolute value, float32 little-endian, degrees |
| 6 | 1 | `'E'` or `'W'` |
| 7 | 4 | longitude, absolute value, float32 LE, degrees |
| 11 | 1 | `'+'` or `'-'` |
| 12 | 4 | altitude, absolute value, float32 LE, metres |
| 16 | 4 | UTC time, uint32 LE, seconds since 1970-01-01 |

Example: `04 4e 2e125242 45 f5115641 2b a0c12742 ebb2f46a`.

The firmware sends one packet every 5 s (configurable) and **only with a valid GNSS fix and
valid GNSS time** – the camera never gets a stale or invented position. In our test the
R6 Mark II did not use this time to set its clock.

## Camera switched off – the "ghost clicking" problem

With Bluetooth enabled, a switched-off camera keeps advertising and **accepts connections**.
But every connection wakes it up briefly – on the R6 Mark II you hear the shutter
(it closes the shutter at power-off and re-opens/closes it when woken). It also drops the
connection again after a few seconds (HCI reason 0x13), so a naive "reconnect immediately"
loop makes the camera click every few seconds.

Neither the advertising payload nor the pairing sequence tells "on" from "off" (the last byte
of the manufacturer data, `...2a05` / `...2a02` / `...2a01`, appears in both states).
What does differ is the **advertising rate**:

| Camera state | Adverts received per 10 s (passive scan) |
|---|---|
| off (steady state) | ~7–9 |
| on | ~150 |
| off, during the first ~25 s after it dropped a connection | ~150 |

The firmware therefore counts adverts in 3-second windows and only connects when the camera
advertises fast (≥ 9 per 3 s). After the camera itself dropped an active connection, it waits
45 s before trusting a fast rate again. Result: no connection attempts – and no clicking –
while the camera is off; connection within a few seconds after switching it on.

Also: furble writes `0x02` (shooting mode) to `00030010` after connecting. That is not needed
for GPS and this firmware does not do it.

## Other observations (R6 Mark II)

* After the tracker reboots, the first encryption attempt sometimes fails because the camera
  still holds the old link; the second attempt works. Do **not** delete the bond on such a
  failure – the camera keeps its key and would refuse the tracker afterwards.
* Writing can fail while the link still exists ("half-dead" link). The firmware disconnects
  after 3 failed writes in a row and reconnects with a short back-off.
* The manufacturer data uses Canon's company ID `0x01A9`.
