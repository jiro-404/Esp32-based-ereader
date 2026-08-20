/*
  Shakib's E-Reader — ESP32-C3 + WeAct 2.13" Tri-Color E-Ink
  ------------------------------------------------------------
  Combines: state-machine UI, NVS-persisted reading position,
  book list with delete, WiFi .txt upload, and real deep sleep
  with the samurai artwork as a screensaver.

  IMPORTANT: put samurai_bitmap.h in the SAME sketch folder as
  this .ino file (the one generated earlier for the samurai image).

  Wiring:
    BUSY -> GPIO 4      RST -> GPIO 5
    DC   -> GPIO 6      CS  -> GPIO 7
    SCK  -> GPIO 8      MOSI-> GPIO 10
    Button -> GPIO 2 (other leg to GND)

  Button gestures (single button):
    Short press          (<500ms)      -> next page / cycle menu selection
    Medium hold          (500-2000ms)  -> open menu / select item
    Long hold            (2000ms+)     -> previous page (reading screen only)

  Note: this panel does NOT support partial refresh (tri-color hardware
  limitation), so every screen change is a full ~12-16s refresh. This
  applies to menu navigation too, not just page turns.
*/

#include <SPI.h>
#include <GxEPD2_3C.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <vector>
#include "samurai_bitmap.h"

// ---- Pins ----
#define EPD_BUSY  4
#define EPD_RST   5
#define EPD_DC    6
#define EPD_CS    7
#define EPD_SCK   8
#define EPD_MOSI  10
#define BTN_PIN   2
#define BTN_GND_PIN 0   // driven LOW in software, acts as "ground" for the button

// ---- Timing ----
#define IDLE_TIMEOUT_MS   (10UL * 60UL * 1000UL)   // 10 min -> deep sleep
#define LONG_HOLD_MS       800     // hold past this -> fires immediately, no upper limit
#define DOUBLE_TAP_MS      350     // max gap between taps to count as a double-tap
#define BTN_DEBOUNCE       30

// ---- WiFi ----
#define WIFI_SSID "Shakib-Reader"
#define WIFI_PASS "readbooks123"

// ---- Display geometry (landscape) ----
#define DISP_W 250
#define DISP_H 122

enum AppState {
  STATE_SPLASH, STATE_MENU, STATE_READING,
  STATE_BOOKLIST, STATE_BOOK_OPTIONS, STATE_DOWNLOAD
};
enum BtnEvent { BTN_NONE, BTN_SINGLE, BTN_DOUBLE, BTN_LONGHOLD };

GxEPD2_3C<GxEPD2_213_Z98c, GxEPD2_213_Z98c::HEIGHT> display(
    GxEPD2_213_Z98c(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

AppState currentState = STATE_SPLASH;

const char* menuItems[] = { "Continue", "Booklist", "Download Books" };
const int MENU_COUNT = 3;
int menuSelection = 0;

#define MAX_BOOKS 20
String bookFiles[MAX_BOOKS];
int bookCount = 0;
int bookSelection = 0;
int bookOptSelection = 0;

// ---- Reading state ----
String currentBook = "";
String bookText = "";
std::vector<int> pageStarts;
int currentPage = 0;
int totalPages = 0;

String lastBook = "";
int lastPage = 0;

unsigned long lastActivityMs = 0;
bool wifiActive = false;
bool pendingUploadConfirm = false;
String pendingUploadName = "";

Preferences prefs;
WebServer server(80);

// ---- Layout constants for reading area ----
const int bodyTop = 30;
const int bodyBottom = DISP_H - 12;
const int bodyLeft = 4;
const int bodyWidth = DISP_W - 8;
const int lineHeightPx = 20;  // FreeSans9pt7b needs ~19-20px to avoid line overlap

const char UPLOAD_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Shakib's E-Reader</title>
  <style>
    body{font-family:sans-serif;max-width:480px;margin:40px auto;padding:0 16px}
    h1{font-size:1.4rem}
    input[type=file]{margin:12px 0;width:100%}
    button{background:#2a5;color:#fff;border:none;padding:10px 24px;border-radius:6px;font-size:16px;cursor:pointer}
    #msg{margin-top:16px;padding:10px;border-radius:6px;display:none}
    .ok{background:#d4edda;color:#155724}
    .err{background:#f8d7da;color:#721c24}
  </style>
</head>
<body>
  <h1>Shakib's E-Reader</h1>
  <p>Upload a <strong>.txt</strong> book file to the device.</p>
  <form id="frm">
    <input type="file" id="file" accept=".txt" required>
    <br><br>
    <button type="submit">Upload Book</button>
  </form>
  <div id="msg"></div>
  <script>
    document.getElementById('frm').onsubmit = async e => {
      e.preventDefault();
      const f = document.getElementById('file').files[0];
      if (!f) return;
      const fd = new FormData();
      fd.append('file', f, f.name);
      const msg = document.getElementById('msg');
      msg.style.display = 'block';
      msg.textContent = 'Uploading...';
      try {
        const r = await fetch('/upload', {method:'POST', body:fd});
        const t = await r.text();
        msg.className = r.ok ? 'ok' : 'err';
        msg.textContent = t;
      } catch(err) {
        msg.className = 'err';
        msg.textContent = 'Upload failed: ' + err;
      }
    };
  </script>
</body>
</html>
)rawliteral";

// ---------------- NVS helpers ----------------
void loadGlobalState() {
  prefs.begin("reader", false);
  lastBook = prefs.getString("lastBook", "");
  lastPage = prefs.getInt("lastPage", 0);
  prefs.end();
}

void saveGlobalState() {
  prefs.begin("reader", false);
  prefs.putString("lastBook", lastBook);
  prefs.putInt("lastPage", lastPage);
  prefs.end();
}

String bookKey(const String& fname) {
  String k = "pg_" + fname;
  if (k.length() > 15) k = k.substring(0, 15);
  return k;
}

int loadBookPage(const String& fname) {
  prefs.begin("bookpages", false);
  int p = prefs.getInt(bookKey(fname).c_str(), 0);
  prefs.end();
  return p;
}

void saveBookPage(const String& fname, int page) {
  prefs.begin("bookpages", false);
  prefs.putInt(bookKey(fname).c_str(), page);
  prefs.end();
}

void deleteBookData(const String& fname) {
  prefs.begin("bookpages", false);
  prefs.remove(bookKey(fname).c_str());
  prefs.end();
  String path = "/" + fname;
  if (LittleFS.exists(path)) LittleFS.remove(path);
}

// ---------------- Book scanning ----------------
void scanBooks() {
  bookCount = 0;
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  while (f && bookCount < MAX_BOOKS) {
    String name = String(f.name());
    if (name.endsWith(".txt")) {
      if (name.startsWith("/")) name = name.substring(1);
      bookFiles[bookCount++] = name;
    }
    f = root.openNextFile();
  }
}

// ---------------- Pagination (proportional-font word wrap, index-based) ----------------
// These work on integer offsets into bookText directly -- no per-word
// object allocation, so memory use stays flat regardless of book size.

int findLineEnd(int pos, int boundEnd, int maxW) {
  while (pos < boundEnd && bookText[pos] == ' ') pos++;
  int lineStart = pos;
  int i = pos;
  int lastFitEnd = pos;

  while (i < boundEnd) {
    int wordEnd = i;
    while (wordEnd < boundEnd && bookText[wordEnd] != ' ' && bookText[wordEnd] != '\n') wordEnd++;

    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(bookText.substring(lineStart, wordEnd).c_str(), 0, 0, &x1, &y1, &w, &h);

    if ((int)w > maxW) {
      if (lastFitEnd > lineStart) return lastFitEnd;   // stop before the word that doesn't fit
      return wordEnd;                                   // single word wider than line -- take it anyway
    }
    lastFitEnd = wordEnd;

    if (wordEnd >= boundEnd) return wordEnd;
    if (bookText[wordEnd] == '\n') return wordEnd;      // stop at explicit newline

    i = wordEnd + 1;  // skip the space
  }
  return lastFitEnd;
}

int skipSeparators(int pos, int boundEnd) {
  while (pos < boundEnd && bookText[pos] == ' ') pos++;
  if (pos < boundEnd && bookText[pos] == '\n') pos++;
  return pos;
}

void paginateBook() {
  pageStarts.clear();
  pageStarts.push_back(0);

  display.setFont(&FreeSans9pt7b);
  int maxLines = (bodyBottom - bodyTop) / lineHeightPx;

  int len = bookText.length();
  int pos = 0;
  int lineOnPage = 0;

  while (pos < len) {
    int lineEnd = findLineEnd(pos, len, bodyWidth);
    pos = skipSeparators(lineEnd, len);

    lineOnPage++;
    if (lineOnPage >= maxLines && pos < len) {
      pageStarts.push_back(pos);
      lineOnPage = 0;
    }
  }
  totalPages = pageStarts.size();
  Serial.printf("Paginated: %d pages, free heap after: %u bytes\n", totalPages, ESP.getFreeHeap());
}

// Adafruit GFX / GxEPD2 fonts have no UTF-8 awareness -- each byte of a
// multi-byte UTF-8 character (like curly quotes) gets treated as its own
// invalid character, which both fails to render AND corrupts the width
// measurements our word-wrap logic depends on. Replace common "smart"
// punctuation with plain ASCII equivalents before we ever measure it.
String sanitizeText(String text) {
  struct Repl { const char* from; const char* to; };
  static const Repl repls[] = {
    {"\xE2\x80\x98", "'"},   // ' left single quote
    {"\xE2\x80\x99", "'"},   // ' right single quote / apostrophe
    {"\xE2\x80\x9C", "\""},  // " left double quote
    {"\xE2\x80\x9D", "\""},  // " right double quote
    {"\xE2\x80\x93", "-"},   // en dash
    {"\xE2\x80\x94", "--"},  // em dash
    {"\xE2\x80\xA6", "..."}, // ellipsis
    {"\xEF\xBB\xBF", ""},    // UTF-8 BOM, if present at file start
  };
  for (auto& r : repls) text.replace(r.from, r.to);
  text.replace("\r", "");  // strip Windows-style CR entirely; \n alone marks line breaks
  return text;
}

void loadAndPaginate(const String& fname) {
  Serial.printf("Free heap before load: %u bytes\n", ESP.getFreeHeap());
  File f = LittleFS.open("/" + fname, "r");
  if (!f) {
    Serial.println("Failed to open book file!");
    bookText = "";
    totalPages = 0;
    return;
  }
  bookText = f.readString();
  f.close();
  bookText = sanitizeText(bookText);
  Serial.printf("Loaded '%s' -- %d bytes. Free heap after read: %u bytes\n",
                fname.c_str(), bookText.length(), ESP.getFreeHeap());
  paginateBook();
}

// ---------------- Display setup ----------------
void initDisplay() {
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200, true, 2, false);
  display.setRotation(1);  // landscape
}

void printCentred(const char* text, int16_t y) {
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  display.setCursor((DISP_W - (int16_t)w) / 2 - x1, y);
  display.print(text);
}

// ---------------- Screens ----------------
void drawSplash() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(1, 1, DISP_W - 2, DISP_H - 2, GxEPD_BLACK);
    display.drawRect(3, 3, DISP_W - 6, DISP_H - 6, GxEPD_BLACK);
    display.setFont(&FreeSansBold12pt7b);
    display.setTextColor(GxEPD_BLACK);
    printCentred("SHAKIB'S", 40);
    printCentred("E-READER", 65);
    display.drawFastHLine(30, 75, DISP_W - 60, GxEPD_BLACK);
    display.setFont(&FreeSans9pt7b);
    printCentred("press button to start", 98);
  } while (display.nextPage());
}

void drawMenuFull() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(1, 1, DISP_W - 2, DISP_H - 2, GxEPD_BLACK);
    display.fillRect(1, 1, DISP_W - 2, 18, GxEPD_BLACK);
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_WHITE);
    printCentred("Main Menu", 13);
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    for (int i = 0; i < MENU_COUNT; i++) {
      int16_t y = 32 + i * 26;
      if (i == menuSelection) display.fillRect(DISP_W - 16, y, 11, 14, GxEPD_BLACK);
      else                    display.drawRect(DISP_W - 16, y, 11, 14, GxEPD_BLACK);
      display.setCursor(10, y + 11);
      display.print(menuItems[i]);
    }
    display.setFont();
    display.setCursor(4, DISP_H - 10);
    display.print("Tap=move  DblTap=select");
  } while (display.nextPage());
}

void drawReadingScreen() {
  int start = pageStarts[currentPage];
  int end = (currentPage + 1 < (int)pageStarts.size()) ? pageStarts[currentPage + 1] : bookText.length();

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, DISP_W, 15, GxEPD_BLACK);
    display.setFont();
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(3, 4);
    String hdr = currentBook;
    if (hdr.length() > 24) hdr = hdr.substring(0, 24) + "..";
    display.print(hdr);
    display.setCursor(DISP_W - 46, 4);
    display.printf("p%d/%d", currentPage + 1, totalPages);

    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);

    int y = bodyTop;
    int pos = start;
    while (pos < end) {
      int lineEnd = findLineEnd(pos, end, bodyWidth);
      display.setCursor(bodyLeft, y);
      display.print(bookText.substring(pos, lineEnd));
      y += lineHeightPx;
      pos = skipSeparators(lineEnd, end);
    }

    display.setFont();
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(4, DISP_H - 10);
    display.print("Tap=next  DblTap=menu  Hold=prev");
  } while (display.nextPage());
}

void drawBookList() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(1, 1, DISP_W - 2, DISP_H - 2, GxEPD_BLACK);
    display.fillRect(1, 1, DISP_W - 2, 15, GxEPD_BLACK);
    display.setFont();
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(4, 5);
    display.print("Booklist");
    size_t freeB = LittleFS.totalBytes() - LittleFS.usedBytes();
    display.setCursor(160, 5);
    display.printf("Free:%dKB", (int)(freeB / 1024));
    display.setTextColor(GxEPD_BLACK);
    if (bookCount == 0) {
      display.setFont(&FreeSans9pt7b);
      display.setCursor(8, 50);
      display.print("No books found.");
      display.setCursor(8, 68);
      display.print("Use Download Books to add.");
    } else {
      int maxVisible = min(bookCount, 5);
      for (int i = 0; i < maxVisible; i++) {
        int16_t y = 20 + i * 20;
        if (i == bookSelection) display.fillRect(DISP_W - 13, y + 2, 9, 11, GxEPD_BLACK);
        else                    display.drawRect(DISP_W - 13, y + 2, 9, 11, GxEPD_BLACK);
        display.setFont();
        display.setCursor(4, y + 4);
        String bname = bookFiles[i];
        if (bname.length() > 26) bname = bname.substring(0, 26) + "..";
        display.print(bname);
        display.setCursor(DISP_W - 58, y + 4);
        display.printf("p%d", loadBookPage(bookFiles[i]) + 1);
      }
    }
    display.setFont();
    display.setCursor(4, DISP_H - 10);
    display.print("Tap=move  DblTap=options");
  } while (display.nextPage());
}

void drawBookOptions() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(1, 1, DISP_W - 2, DISP_H - 2, GxEPD_BLACK);
    display.fillRect(1, 1, DISP_W - 2, 18, GxEPD_BLACK);
    display.setFont();
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(6, 6);
    String bname = bookFiles[bookSelection];
    if (bname.length() > 34) bname = bname.substring(0, 34) + "..";
    display.print(bname);
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    if (bookOptSelection == 0) display.fillRect(DISP_W - 16, 34, 11, 14, GxEPD_BLACK);
    else                       display.drawRect(DISP_W - 16, 34, 11, 14, GxEPD_BLACK);
    display.setCursor(10, 46);
    display.print("Open book");
    if (bookOptSelection == 1) display.fillRect(DISP_W - 16, 66, 11, 14, GxEPD_BLACK);
    else                       display.drawRect(DISP_W - 16, 66, 11, 14, GxEPD_BLACK);
    display.setCursor(10, 78);
    display.print("Delete book");
    display.setFont();
    display.setCursor(4, DISP_H - 10);
    display.print("Tap=move  DblTap=confirm");
  } while (display.nextPage());
}

void drawDownloadScreen() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, DISP_W, 15, GxEPD_BLACK);
    display.setFont();
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(4, 5);
    display.print("Download Books - WiFi On");
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(4, 32);
    display.print("SSID: " WIFI_SSID);
    display.setCursor(4, 50);
    display.print("Pass: " WIFI_PASS);
    display.setCursor(4, 68);
    display.print("URL: 192.168.4.1");
    display.setFont();
    display.setCursor(4, 88);
    display.print("Open URL in browser to upload .txt");
    display.setCursor(4, DISP_H - 10);
    display.print("DblTap=stop WiFi & go back");
  } while (display.nextPage());
}

void showUploadConfirmation(const String& fname) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, DISP_W, 15, GxEPD_BLACK);
    display.setFont();
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(4, 5);
    display.print("Book Uploaded!");
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(4, 32);
    display.print("Added to library:");
    display.setCursor(4, 50);
    String n = fname;
    if (n.length() > 26) n = n.substring(0, 26) + "..";
    display.print(n);
    display.setCursor(4, 68);
    display.printf("Total books: %d", bookCount);
    display.setFont();
    display.setCursor(4, DISP_H - 10);
    display.print("DblTap=back to menu");
  } while (display.nextPage());
}

// ---------------- Deep sleep screensaver ----------------
void enterDeepSleep() {
  if (currentState == STATE_READING) saveBookPage(currentBook, currentPage);
  saveGlobalState();

  display.setRotation(0);  // portrait, matches samurai bitmap's native orientation
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawBitmap(0, 0, samurai_black, SAMURAI_W, SAMURAI_H, GxEPD_BLACK);
    display.drawBitmap(0, 0, samurai_red, SAMURAI_W, SAMURAI_H, GxEPD_RED);
  } while (display.nextPage());
  display.hibernate();

  // ESP32-C3 has no ext0/ext1 wakeup controller (unlike classic ESP32) —
  // it uses this GPIO wakeup API instead. We also explicitly hold the
  // pull-up, since the C3's low-power IO is simpler than classic ESP32's
  // and doesn't use a separate rtc_gpio_* API.
  gpio_pullup_en((gpio_num_t)BTN_PIN);
  gpio_pulldown_dis((gpio_num_t)BTN_PIN);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << BTN_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

// ---------------- WiFi ----------------
void handleUpload() {
  HTTPUpload& upload = server.upload();
  static File uploadFile;
  if (upload.status == UPLOAD_FILE_START) {
    String filename = "/" + upload.filename;
    if (!filename.endsWith(".txt")) filename += ".txt";
    uploadFile = LittleFS.open(filename, "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    scanBooks();
    pendingUploadName = upload.filename;
    pendingUploadConfirm = true;
    // Do NOT call server.send() here -- the request handler below sends
    // the actual response once the whole request is complete. Sending
    // from both places caused a malformed/duplicate response, which is
    // why the browser saw "Failed to fetch" even though the file was
    // already written successfully.
  }
}

void startWifi() {
  Serial.println("startWifi() called");
  bool ok = WiFi.softAP(WIFI_SSID, WIFI_PASS);
  Serial.printf("softAP() returned: %s\n", ok ? "true" : "false");
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", UPLOAD_PAGE); });
  server.on("/upload", HTTP_POST, []() {
    server.send(200, "text/plain", "Book uploaded! '" + pendingUploadName + "' added to library.");
  }, handleUpload);
  server.begin();
  wifiActive = true;
  Serial.println("Web server started, wifiActive=true");
}

void stopWifi() {
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiActive = false;
}

// ---------------- Idle / button ----------------
void resetIdle() { lastActivityMs = millis(); }
bool isIdle() { return (millis() - lastActivityMs) > IDLE_TIMEOUT_MS; }

BtnEvent readButton() {
  static bool lastState = HIGH;
  static bool pressed = false;
  static unsigned long pressStart = 0;
  static bool waitingSecondTap = false;
  static unsigned long firstReleaseTime = 0;

  bool state = digitalRead(BTN_PIN);
  unsigned long now = millis();
  BtnEvent result = BTN_NONE;

  if (lastState == HIGH && state == LOW) {
    pressStart = now;
    pressed = true;
  }

  // Long hold fires immediately once threshold is crossed — no need to
  // release within a window, just hold until it triggers.
  if (pressed && state == LOW && (now - pressStart) >= LONG_HOLD_MS) {
    pressed = false;
    waitingSecondTap = false;
    lastState = state;
    return BTN_LONGHOLD;
  }

  if (pressed && state == HIGH) {
    unsigned long heldFor = now - pressStart;
    pressed = false;
    if (heldFor > BTN_DEBOUNCE) {
      if (waitingSecondTap && (now - firstReleaseTime) <= DOUBLE_TAP_MS) {
        waitingSecondTap = false;
        result = BTN_DOUBLE;
      } else {
        waitingSecondTap = true;
        firstReleaseTime = now;
      }
    }
  }

  // No second tap arrived in time -> resolve as a single tap
  if (waitingSecondTap && (now - firstReleaseTime) > DOUBLE_TAP_MS) {
    waitingSecondTap = false;
    result = BTN_SINGLE;
  }

  lastState = state;
  return result;
}

// ---------------- Book open ----------------
void openBook(const String& fname, int startPage) {
  currentBook = fname;
  loadAndPaginate(fname);
  currentPage = min(startPage, max(0, totalPages - 1));
  lastBook = fname;
  lastPage = currentPage;
  saveGlobalState();
  currentState = STATE_READING;
  drawReadingScreen();
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(BTN_GND_PIN, OUTPUT);
  digitalWrite(BTN_GND_PIN, LOW);   // acts as the button's "ground" leg
  pinMode(BTN_PIN, INPUT_PULLUP);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  loadGlobalState();
  scanBooks();
  initDisplay();

  esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();

  // Debounce: wait for the wake-button press to be released before continuing
  while (digitalRead(BTN_PIN) == LOW) delay(10);

  if (wakeReason == ESP_SLEEP_WAKEUP_GPIO) {
    // Woken from deep sleep -> skip splash, go straight back in
    if (lastBook.length() > 0) {
      openBook(lastBook, lastPage);
    } else {
      menuSelection = 0;
      currentState = STATE_MENU;
      drawMenuFull();
    }
  } else {
    currentState = STATE_SPLASH;
    drawSplash();
  }
  resetIdle();
}

void loop() {
  if (wifiActive && currentState == STATE_DOWNLOAD) {
    server.handleClient();
    if (pendingUploadConfirm) {
      pendingUploadConfirm = false;
      showUploadConfirmation(pendingUploadName);
      resetIdle();
    }
  } else if (wifiActive && currentState != STATE_DOWNLOAD) {
    stopWifi();
  }

  BtnEvent evt = readButton();

  // Serial fallback -- same three gestures as the physical button,
  // handy for testing without needing to press it:
  //   n = single tap (next / move)   d = double tap (menu / select)
  //   l = long hold (previous page, reading screen only)
  if (evt == BTN_NONE && Serial.available()) {
    char c = Serial.read();
    if (c == 'n') evt = BTN_SINGLE;
    else if (c == 'd') evt = BTN_DOUBLE;
    else if (c == 'l') evt = BTN_LONGHOLD;
  }

  if (isIdle() && currentState != STATE_DOWNLOAD) {
    enterDeepSleep();  // does not return
    return;
  }

  if (evt == BTN_NONE) return;
  resetIdle();

  if (currentState == STATE_SPLASH) {
    menuSelection = 0;
    currentState = STATE_MENU;
    drawMenuFull();
    return;
  }

  if (currentState == STATE_MENU) {
    if (evt == BTN_SINGLE) {
      menuSelection = (menuSelection + 1) % MENU_COUNT;
      drawMenuFull();
    } else if (evt == BTN_DOUBLE) {
      Serial.printf("Menu double-tap, selection=%d (%s)\n", menuSelection, menuItems[menuSelection]);
      switch (menuSelection) {
        case 0:
          if (lastBook.length() > 0) openBook(lastBook, lastPage);
          else if (bookCount > 0) openBook(bookFiles[0], 0);
          else { bookSelection = 0; currentState = STATE_BOOKLIST; drawBookList(); }
          break;
        case 1: bookSelection = 0; currentState = STATE_BOOKLIST; drawBookList(); break;
        case 2: startWifi(); currentState = STATE_DOWNLOAD; drawDownloadScreen(); break;
      }
    }
    return;
  }

  if (currentState == STATE_READING) {
    if (evt == BTN_SINGLE) {
      if (currentPage < totalPages - 1) {
        currentPage++;
        lastPage = currentPage;
        saveGlobalState();
        saveBookPage(currentBook, currentPage);
      }
      drawReadingScreen();
    } else if (evt == BTN_LONGHOLD) {
      if (currentPage > 0) {
        currentPage--;
        lastPage = currentPage;
        saveGlobalState();
        saveBookPage(currentBook, currentPage);
      }
      drawReadingScreen();
    } else if (evt == BTN_DOUBLE) {
      saveBookPage(currentBook, currentPage);
      saveGlobalState();
      currentState = STATE_MENU;
      drawMenuFull();
    }
    return;
  }

  if (currentState == STATE_BOOKLIST) {
    if (evt == BTN_SINGLE) {
      if (bookCount > 0) { bookSelection = (bookSelection + 1) % bookCount; drawBookList(); }
      else { currentState = STATE_MENU; drawMenuFull(); }
    } else if (evt == BTN_DOUBLE) {
      if (bookCount > 0) { bookOptSelection = 0; currentState = STATE_BOOK_OPTIONS; drawBookOptions(); }
      else { currentState = STATE_MENU; drawMenuFull(); }
    }
    return;
  }

  if (currentState == STATE_BOOK_OPTIONS) {
    if (evt == BTN_SINGLE) {
      bookOptSelection = (bookOptSelection + 1) % 2;
      drawBookOptions();
    } else if (evt == BTN_DOUBLE) {
      if (bookOptSelection == 0) {
        openBook(bookFiles[bookSelection], loadBookPage(bookFiles[bookSelection]));
      } else {
        String toDelete = bookFiles[bookSelection];
        deleteBookData(toDelete);
        if (lastBook == toDelete) { lastBook = ""; lastPage = 0; saveGlobalState(); }
        scanBooks();
        bookSelection = 0;
        currentState = STATE_BOOKLIST;
        drawBookList();
      }
    }
    return;
  }

  if (currentState == STATE_DOWNLOAD) {
    if (evt == BTN_DOUBLE || evt == BTN_LONGHOLD) {
      stopWifi();
      currentState = STATE_MENU;
      drawMenuFull();
    }
    return;
  }
}
