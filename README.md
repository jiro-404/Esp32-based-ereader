# Esp32-based-ereader
# Shakib's E-Reader

A tiny single-button e-reader built on an ESP32-C3 SuperMini and a WeAct Studio 2.13" tri-color (black/white/red) e-ink display. Books are uploaded wirelessly over the device's own WiFi hotspot, stored on onboard flash, and read with a single physical button.

## Hardware

- ESP32-C3 SuperMini
- WeAct Studio 2.13" tri-color e-ink display (250×122, SSD1680 driver, GxEPD2 class `GxEPD2_213_Z98c`)
- 1x momentary push button
- 3.7V LiPo battery (connected directly to the 3V3 pin, bypassing the onboard regulator — see Power Notes below)

### Wiring

| Component pin | ESP32-C3 GPIO |
|---|---|
| BUSY | 4 |
| RST | 5 |
| DC | 6 |
| CS | 7 |
| SCK | 8 |
| MOSI | 10 |
| Button (one leg) | 2 |
| Button (other leg) | 0 *(driven LOW in software as a "ground" pin — see note below)* |
| Battery + | 3V3 |
| Battery − | GND |

**Note on the button's second leg:** GPIO 0 is configured as an `OUTPUT` driven `LOW` in `setup()`, acting as a software ground for the button. This was done because GND wasn't convenient to reach in this build. GPIO 0 is a boot-strapping pin, but since it's only driven low *after* boot completes, this doesn't interfere with normal startup — the only edge case is if the button is physically held down at the exact moment of a cold power-on/reset.

## Files

| File | Purpose |
|---|---|
| `shakib_ereader.ino` | Main firmware |
| `samurai_bitmap.h` | Pre-converted 122×250 black/red bitmap data, shown as the deep-sleep screensaver |

Both files must be in the same sketch folder (named to match the `.ino` filename, per Arduino IDE convention).

## Required Libraries

Install via Arduino Library Manager:
- **GxEPD2**
- **Adafruit GFX Library** (GxEPD2 dependency)

Built-in (no install needed): `Preferences`, `LittleFS`, `WiFi`, `WebServer`, `esp_sleep`.

## Board Settings (Arduino IDE)

- Board: **ESP32C3 Dev Module**
- **USB CDC On Boot: Enabled** *(required — without this, Serial Monitor shows nothing, since the C3 uses native USB rather than a separate USB-serial chip)*
- Partition Scheme: **Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)**
- Upload Speed: 921600 (drop to 115200 if uploads are unreliable)

## Features

- **Menu system:** Continue reading, Booklist, Download Books
- **Book list:** browse uploaded books, open or delete them
- **Per-book reading position:** each book remembers its own page independently, persisted in NVS flash (survives power loss/reboots)
- **WiFi book upload:** device creates its own hotspot with a simple upload webpage — no need to join your home network
- **Deep sleep:** after 10 minutes idle, the device shows a samurai artwork screensaver and enters deep sleep; a button press wakes it back into your book
- **Single-button control** via three gestures (see below)

## Button Controls

| Gesture | Action |
|---|---|
| **Single tap** (quick press-release) | Next page / move menu selection |
| **Double tap** (two quick taps) | Open menu / confirm selection |
| **Long hold** (800ms+, fires immediately, no upper limit) | Previous page *(reading screen only)* |

Serial Monitor fallback (115200 baud) — type + Enter:
- `n` = single tap
- `d` = double tap
- `l` = long hold

## WiFi Upload

1. From the main menu, select **Download Books**
2. On your phone/laptop, connect to WiFi network **`Shakib-Reader`**, password `readbooks123`
3. Open a browser to `http://192.168.4.1`
4. Choose a `.txt` file and upload

**Only plain `.txt` files are supported.** Uploaded text is automatically sanitized on load:
- Curly/smart quotes, em/en dashes, and ellipsis characters are converted to plain ASCII equivalents (the display font doesn't support Unicode, and unconverted multi-byte characters corrupt text-wrapping calculations)
- Windows-style `\r\n` line endings are stripped down to `\n`

If a book still displays incorrectly, it likely contains other non-ASCII characters not yet covered by the sanitizer — the fix is adding more replacements to the `sanitizeText()` function.

## Known Limitations

- **No partial refresh.** Tri-color e-ink panels can't do partial refresh (a hardware limitation, not a bug) — every screen update, including menu navigation, is a full refresh taking roughly **15-18 seconds**. This applies throughout the whole UI, not just page turns.
- **Whole book loaded into RAM.** Works fine for typical book-sized `.txt` files, but very large files could exhaust the ESP32-C3's ~400KB RAM. `ESP.getFreeHeap()` is logged to Serial during loading if you need to diagnose this.
- **~4-5 lines of text per page** at the current font/size, due to the small screen and generous line spacing needed to avoid visual overlap.

## Power Notes

This build powers the board by connecting the LiPo battery directly to the 3V3 pin, bypassing the onboard regulator (since the bare ESP32-C3 SuperMini has no built-in battery charging circuit). Implications:
- No overcharge/overdischarge protection unless your battery cell has it built in
- No onboard charging — remove the battery to charge externally, or add a separate charger module
- If WiFi becomes unreliable, consider adding a bulk capacitor (~470-1000µF electrolytic + 100nF ceramic) across 3.3V/GND to smooth current spikes during radio transmit bursts

## Customizing

- **WiFi name/password:** `WIFI_SSID` / `WIFI_PASS` near the top of the sketch
- **Idle timeout before sleep:** `IDLE_TIMEOUT_MS`
- **Font/line spacing:** `lineHeightPx`, and the `FreeSans9pt7b` font used for body text
- **Deep sleep screensaver image:** regenerate `samurai_bitmap.h` with a different image using the same black/red bitmap conversion process (any image works, ideally already high-contrast with red accents)

Suggest me if you have any better idea.
