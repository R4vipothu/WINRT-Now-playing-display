/*
 * Now Playing Display - ESP32 firmware
 *
 * A desk display that shows what is currently playing on a Windows PC
 * (any app that reports to the Windows media session API: Spotify,
 * YouTube in a browser, VLC, etc.) and updates in near real time.
 *
 * How it works:
 *   The PC runs pc_server/now_playing_server.py, which reads the Windows
 *   media session (WinRT) and pushes updates over a WebSocket. This
 *   sketch receives track metadata as JSON and the album art as binary
 *   RGB565 chunks, and renders everything with LVGL on a 240x240 TFT.
 *
 * Measured performance: ~120 ms from a track change on the PC to the
 * updated track shown on the display.
 *
 * Hardware:
 *   - ESP32 development board
 *   - 240x240 TFT display (TFT_eSPI), backlight on GPIO 21
 *
 * Libraries (Arduino Library Manager):
 *   - TFT_eSPI, LVGL, ArduinoJson, WebSocketsClient (Markus Sattler)
 *
 * Setup:
 *   1. Configure TFT_eSPI for your display in its User_Setup file.
 *   2. Enable the Montserrat 10 and 14 fonts in your LVGL config.
 *   3. Copy config.example.h to config.h and fill in your Wi-Fi
 *      credentials and your PC's local IP address. config.h is
 *      gitignored - never commit your real credentials.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <cstring>
#include <new>
#include <cstddef>

#define TFT_HOR_RES 240
#define TFT_VER_RES 240
#define TFT_ROTATION LV_ROTATION_0

#define VINYL_SIZE 200 // diameter of the album-art disc
#define RING_SIZE 216  // progress ring diameter, slightly larger than the art
#define VINYL_Y_OFFSET 14 // px the vinyl group is nudged up from center
#define ROTATION_TIMER_MS 40    // spin tick interval
#define ROTATION_STEP_DEG 20    // spin step per tick (of 3600 = one turn)
#define STRIP_HEIGHT 58         // bottom text strip
#define STRIP_CORE_HEIGHT 34    // denser lower band of the strip

/*
 * ----------------------------------------------------------------
 * Analog joystick with push switch
 * ----------------------------------------------------------------
 *
 * Physical pins:
 *   VRY/Y  → GPIO32, analog vertical axis (ADC1)
 *   VRX/X  → GPIO35, analog horizontal axis (ADC1)
 *   SW     → GPIO27, momentary switch to GND (INPUT_PULLUP)
 *   VCC/+  → ESP32 3V3 (do not use VIN/5 V: ADC pins are 3.3 V maximum)
 *   GND    → ESP32 GND
 *
 * Volume and direction controls:
 *   - Hold up for 220 ms, then repeat a VolumeUp command.
 *   - Hold down for 220 ms, then repeat a VolumeDown command.
 *   - Move RIGHT once for Next; move LEFT once for Previous.
 *   - A calibrated-center offset requires deliberate axis displacement.
 *   - Horizontal/vertical priority prevents diagonal accidental swipes.
 *   - Actions fire once, then reset after returning to neutral.
 *
 * Push-button gestures (SW / digital):
 *   Gesture          Action sent to PC   Detection
 *   ───────────────  ──────────────────   ──────────────────────
 *   Short click      PlayPause           Release within 2 seconds
 *   Long click       Shuffle             Hold for 2 seconds
 *
 *   Debouncing: raw click changes require JOY_CLICK_DEBOUNCE_MS to settle.
 *
 * None of this code blocks; everything is millis()-based so the
 * LVGL tick and the WebSocket pump in loop() keep running.
 * ----------------------------------------------------------------
 */
#define JOY_X_PIN 35          // joystick horizontal axis → GPIO35 (ADC1)
#define JOY_Y_PIN 32          // joystick vertical axis → GPIO32 (ADC1)
#define JOY_SW_PIN 27         // joystick click → GPIO27, input with INPUT_PULLUP
#define JOY_ACTION_OFFSET 900 // clear displacement from calibrated center
#define JOY_RETURN_OFFSET 450 // hysteresis band for returning to neutral
#define JOY_REPEAT_MS 160     // volume repeat delay while held away from center
#define JOY_DIRECTION_MS 220  // minimum hold before an edge direction is accepted
#define JOY_CLICK_DEBOUNCE_MS 20
#define JOY_CLICK_LONG_MS 2000

#define DRAW_BUF_SIZE (TFT_HOR_RES * TFT_VER_RES / 24 * (LV_COLOR_DEPTH / 8))
uint32_t draw_buf[DRAW_BUF_SIZE / 4];

#define MUSICDISPLAYTYPE 2 // 2 = spinning vinyl layout (sent to the PC as "DisplayType"); 1 = legacy static layout, unused since the vinyl redesign

#define DEBUG_LOG 0 // 1 = verbose serial logging (WS events, chunks, positions); errors always print

#include "config.h"  // Wi-Fi credentials + PC address (see config.example.h)

const char* SSID = WIFI_SSID;
const char* PASSWORD = WIFI_PASSWORD;

WebSocketsClient webSocket;

char title[64];      // fixed buffers: no heap churn from per-second String reassignment
char artist[64];
char lastTitle[64];
char lastArtist[64];

uint16_t volume;     // read from media_update, reserved for future UI use
bool playing;        // read from media_update, reserved for future UI use

uint32_t currentPosition;
uint32_t endPosition;
int lastPosition;    // last progress percentage shown on the ring

static bool haveTrackInfo = false;     // true once a real media_update arrives
static bool unknownStateShown = false; // blank record + placeholder text already shown
static bool spinning = false;          // the record art is currently rotating

uint16_t heightOfImage;
uint16_t widthOfImage;

uint8_t* albumData = nullptr; // allow for the image to different sizes (usually for yt videos)

static lv_image_dsc_t lvglCoverImage;

lv_obj_t *coverImage = nullptr;
lv_obj_t *placeHolderCover = nullptr;
lv_obj_t *progressRing = nullptr;
lv_obj_t *nameOfSong;
lv_obj_t *nameOfArtist;
lv_obj_t *spindleHub;
lv_obj_t *vinylDisc = nullptr; // circular clipping container that holds the art
lv_obj_t *textStrip = nullptr; // semi-transparent strip behind the track info

size_t bytesReceived;
size_t albumDataSize;

int angleOfAlbum;
unsigned long previousMillis = 0;

//hex codes for colors
uint32_t BLACK = 0x000000;
uint32_t RING_TRACK = 0x3A3A3A;  // dim gray ring track line
uint32_t RING_ACCENT = 0xFF5A3C; // orange-red ring progress

uint32_t backgroundColor;

void initCoverImage(){
  lvglCoverImage.header.w = 180; // assuming 1:1 ratio from picture
  lvglCoverImage.header.h = 180;
  lvglCoverImage.data_size = 180* 180 * LV_COLOR_DEPTH / 8;
  lvglCoverImage.header.cf = LV_COLOR_FORMAT_NATIVE;          /* Set the color format */
  lvglCoverImage.data = nullptr;
}

static lv_obj_t *makeDisc(lv_obj_t *parent, int size, uint32_t bgColor) {
  lv_obj_t *disc = lv_obj_create(parent);
  lv_obj_set_size(disc, size, size);
  lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(disc, lv_color_hex(bgColor), 0);
  lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(disc, 0, 0);
  lv_obj_set_style_pad_all(disc, 0, 0);
  lv_obj_remove_flag(disc, LV_OBJ_FLAG_SCROLLABLE);
  return disc;
}

static void copyTrackField(char *dst, size_t cap, const char *src) { // bounded copy into a fixed buffer
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

void setup() {
  Serial.begin(115200);
  delay(5000);
  
  backgroundColor = BLACK;

  Serial.print("Free Heap Memory: ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);

  // Analog joystick inputs (see the detailed hardware docs above the #defines)
  pinMode(JOY_SW_PIN, INPUT_PULLUP); // SW shorts to GND when pressed
  analogReadResolution(12);

  //display configuration
  Serial.println("Starting intialization");
  lv_init();

  lv_tick_set_cb(my_tick);
  lv_display_t *disp = lv_tft_espi_create(TFT_HOR_RES, TFT_VER_RES, draw_buf, sizeof(draw_buf));
  lv_display_set_rotation(disp, TFT_ROTATION);

  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(backgroundColor), 0);

  lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

  initCoverImage(); // set up the basic image struct

  // set up placeholder while image connecting
  lv_obj_t *placeholder = lv_label_create(lv_screen_active());
  lv_label_set_text(placeholder, "Attempting to connect!");
  lv_obj_align(placeholder, LV_ALIGN_CENTER, 0, 0);
  // lv_obj_set_style_text_font(placeholder, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(placeholder, lv_color_hex(0xD9B8FF), LV_PART_MAIN);

  // blank vinyl disc shown while there is no album art (no bitmap needed anymore)
  placeHolderCover = makeDisc(lv_screen_active(), VINYL_SIZE, 0x141414);
  lv_obj_align(placeHolderCover, LV_ALIGN_CENTER, 0, -VINYL_Y_OFFSET);
  lv_obj_t *hole = makeDisc(placeHolderCover, 26, BLACK); // spindle hole of the blank record
  lv_obj_align(hole, LV_ALIGN_CENTER, 0, 0);
  lv_obj_t *dot = makeDisc(hole, 5, 0x6E6E6E);            // tiny center dot
  lv_obj_align(dot, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(placeHolderCover, LV_OBJ_FLAG_HIDDEN);

  lv_timer_handler();
  Serial.println( "Screen setup done" );

  // deals with connection
  connect_to_wifi();  // connect to wifi
  webSocket.begin(PC_HOST, PC_PORT, "/"); // open the web socket
  webSocket.onEvent(webSocketEvent);
  lv_obj_del(placeholder); // deleting the placeholder text from above
  webSocket.setReconnectInterval(5000);
  
  lv_timer_handler();

  // dark semi-transparent strip so the track info stays readable over the art
  textStrip = lv_obj_create(lv_screen_active());
  lv_obj_set_size(textStrip, TFT_HOR_RES, STRIP_HEIGHT);
  lv_obj_align(textStrip, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(textStrip, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(textStrip, LV_OPA_30, 0); // faint upper fade
  lv_obj_set_style_border_width(textStrip, 0, 0);
  lv_obj_set_style_radius(textStrip, 0, 0);
  lv_obj_set_style_pad_all(textStrip, 0, 0);
  lv_obj_remove_flag(textStrip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *stripCore = lv_obj_create(textStrip); // stronger lower band -> fake gradient
  lv_obj_set_size(stripCore, TFT_HOR_RES, STRIP_CORE_HEIGHT);
  lv_obj_align(stripCore, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(stripCore, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(stripCore, LV_OPA_50, 0);
  lv_obj_set_style_border_width(stripCore, 0, 0);
  lv_obj_set_style_radius(stripCore, 0, 0);
  lv_obj_set_style_pad_all(stripCore, 0, 0);
  lv_obj_remove_flag(stripCore, LV_OBJ_FLAG_SCROLLABLE);

  nameOfSong = lv_label_create(lv_screen_active());
  lv_obj_set_width(nameOfSong, TFT_HOR_RES - 16); // long titles scroll instead of overflowing
  lv_label_set_long_mode(nameOfSong, LV_LABEL_LONG_SCROLL_CIRCULAR);
  nameOfArtist = lv_label_create(lv_screen_active());
  lv_obj_set_width(nameOfArtist, TFT_HOR_RES - 16);
  lv_label_set_long_mode(nameOfArtist, LV_LABEL_LONG_SCROLL_CIRCULAR);
  
  playing = false; // assume there's no music
  volume = 0;
  strcpy(title, "Unknown");
  strcpy(artist, "No Artist Available");
  strcpy(lastTitle, "null");
  strcpy(lastArtist, "null");

  currentPosition = 0;
  endPosition = 0;
  lastPosition = -1;

  heightOfImage = 1;
  widthOfImage = 1;

  angleOfAlbum = 0;
}

// ============================================================
// Analog joystick service: X/Y axis + SW button.
// Axis values use a high neutral deadband and edge-triggered actions.
// Click sends PlayPause; long click sends Shuffle.
// ============================================================

static int joyNeutralX = 2048;
static int joyNeutralY = 2048;
static bool joyCalibrated = false;
static char activeAxis = 0;
static unsigned long axisStartAt = 0;
static unsigned long nextVolumeRepeatAt = 0;
static bool axisActionFired = false;
static unsigned long lastJoyDebugAt = 0;

static bool btnLastRaw = false;
static bool btnPressed = false;
static unsigned long btnChangeAt = 0;
static unsigned long pressStartAt = 0;
static bool clickActionFired = false;

static lv_obj_t *volumeLabel = nullptr; // stretch goal: tiny on-screen volume readout
static unsigned long volumeLabelHideAt = 0;
static int lastDisplayedVolume = -1;

static void sendVolumeStep(bool increase) {
  JsonDocument doc;
  doc["action"] = increase ? "VolumeUp" : "VolumeDown";

  char msg[32];
  serializeJson(doc, msg, sizeof(msg));
  webSocket.sendTXT(msg);
  Serial.printf("JOYSTICK SEND %s\n", msg);
}

static void showVolumeLabel(int pct) {
  if (volumeLabel == nullptr) { // created lazily, hidden by default, so the
    volumeLabel = lv_label_create(lv_screen_active()); // existing layout is untouched
    lv_obj_set_style_text_font(volumeLabel, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(volumeLabel, lv_color_make(255, 255, 255), LV_PART_MAIN);
    lv_obj_set_style_bg_color(volumeLabel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(volumeLabel, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(volumeLabel, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(volumeLabel, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(volumeLabel, 4, LV_PART_MAIN);
    lv_obj_align(volumeLabel, LV_ALIGN_CENTER, 0, -60);
    lv_obj_add_flag(volumeLabel, LV_OBJ_FLAG_HIDDEN);
  }
  char buf[12];
  snprintf(buf, sizeof(buf), "Vol %d%%", pct);
  lv_label_set_text(volumeLabel, buf);
  lv_obj_remove_flag(volumeLabel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(volumeLabel);
  volumeLabelHideAt = millis() + 1000; // hide ~1 s after the last change
}

static void serviceJoystick() {
  unsigned long now = millis();

  int x = analogRead(JOY_X_PIN);
  int y = analogRead(JOY_Y_PIN);

  // Establish center from the average of 16 readings. Mechanical neutral
  // varies between modules and a 5 V module may output
  // slightly different center voltages between axes.
  if (!joyCalibrated) {
    long sumX = 0;
    long sumY = 0;
    for (int i = 0; i < 16; i++) {
      sumX += analogRead(JOY_X_PIN);
      sumY += analogRead(JOY_Y_PIN);
      delay(5);
    }
    joyNeutralX = (int)(sumX / 16);
    joyNeutralY = (int)(sumY / 16);
    joyCalibrated = true;
    Serial.printf("Joystick calibrated: X=%d Y=%d\n", joyNeutralX, joyNeutralY);
  }

  char direction = 0;
  int xOffset = x - joyNeutralX;
  int yOffset = y - joyNeutralY;

  if (DEBUG_LOG || now - lastJoyDebugAt >= 500) {
    Serial.printf("JOY X=%d Y=%d offsets X=%d Y=%d\n", x, y, xOffset, yOffset);
    lastJoyDebugAt = now;
  }

  // If an axis is active, hold it until both axes return to a neutral band.
  if (activeAxis != 0) {
    bool xNeutral = abs(x - joyNeutralX) <= JOY_RETURN_OFFSET;
    bool yNeutral = abs(y - joyNeutralY) <= JOY_RETURN_OFFSET;
    if (!xNeutral || !yNeutral) direction = activeAxis;
  }

  if (direction == 0) {
    if (x <= joyNeutralX - JOY_ACTION_OFFSET) {
      direction = 'L';
    } else if (x >= joyNeutralX + JOY_ACTION_OFFSET) {
      direction = 'R';
    } else if (y <= joyNeutralY - JOY_ACTION_OFFSET) {
      direction = 'U';
    } else if (y >= joyNeutralY + JOY_ACTION_OFFSET) {
      direction = 'D';
    }
  }

  if (direction != activeAxis) {
    activeAxis = direction;
    axisActionFired = false;
    axisStartAt = now;
    nextVolumeRepeatAt = now;

    if (direction == 'R' || direction == 'L') {
      axisActionFired = true;
      sendAction(direction == 'R' ? "Next" : "Previous");
    }
  }

  if (activeAxis == 'U' || activeAxis == 'D') {
    if (now - axisStartAt >= JOY_DIRECTION_MS && !axisActionFired) {
      axisActionFired = true;
      nextVolumeRepeatAt = now;
    }
    if (axisActionFired && now >= nextVolumeRepeatAt) {
      sendVolumeStep(activeAxis == 'U');
      nextVolumeRepeatAt = now + JOY_REPEAT_MS;
    }
  }

  // ---- joystick click: 50 ms debounce; short = PlayPause, 2 s hold = Shuffle ----
  bool raw = digitalRead(JOY_SW_PIN) == LOW;
  if (raw != btnLastRaw) {
    btnLastRaw = raw;
    btnChangeAt = now;
  }
  if (raw != btnPressed && now - btnChangeAt >= JOY_CLICK_DEBOUNCE_MS) {
    btnPressed = raw;
    if (btnPressed) {
      pressStartAt = now;
      clickActionFired = false;
    } else if (!clickActionFired) {
      sendAction("PlayPause");
    }
  }
  if (btnPressed && !clickActionFired && now - pressStartAt >= JOY_CLICK_LONG_MS) {
    clickActionFired = true;
    sendAction("Shuffle");
  }

  // ---- volume label auto-hide ----
  if (volumeLabel != nullptr &&
      !lv_obj_has_flag(volumeLabel, LV_OBJ_FLAG_HIDDEN) &&
      (long)(now - volumeLabelHideAt) >= 0) {
    lv_obj_add_flag(volumeLabel, LV_OBJ_FLAG_HIDDEN);
  }
}

static void fitArtToDisc(){ // scale the art so its larger side fills the 200px disc
  uint16_t largest = lvglCoverImage.header.w > lvglCoverImage.header.h ? lvglCoverImage.header.w : lvglCoverImage.header.h;
  if (largest == 0) largest = 1;
  lv_image_set_scale(coverImage, 256 * VINYL_SIZE / largest); // 256 = 100% in LVGL v9
}

static void applyImageGeometry(){ // re-fit the cover when the art size changes; no-op if none is up
  if (coverImage == nullptr) return;
  fitArtToDisc();
  lv_image_set_pivot(coverImage, lvglCoverImage.header.w/2, lvglCoverImage.header.h/2);
}

void loop() {
  webSocket.loop();
  serviceJoystick();
  if (!haveTrackInfo) { // no real media_update yet: blank record + placeholder text
    if (!unknownStateShown) {
      setUnknownLabels();
      unknownStateShown = true;

      if (coverImage != nullptr){
        lv_obj_add_flag(coverImage, LV_OBJ_FLAG_HIDDEN);
      }
      if(vinylDisc != nullptr){
        lv_obj_add_flag(vinylDisc, LV_OBJ_FLAG_HIDDEN);
      }
    }
  }
  else if (lvglCoverImage.data != nullptr){ // track info + art available
    unknownStateShown = false;
    if (coverImage == nullptr){ // creates the cover image for the first time
      initVinylUI();
    }
    else{
      lv_obj_remove_flag(coverImage, LV_OBJ_FLAG_HIDDEN);
      if (vinylDisc != nullptr){
        lv_obj_remove_flag(vinylDisc, LV_OBJ_FLAG_HIDDEN);
      }
    }

    if (strcmp(title, lastTitle) != 0 || strcmp(artist, lastArtist) != 0) {
      updateLabels();
    }

    // spin the record only while the art is square (updates happen on state transitions)
    bool wantSpin = (heightOfImage == widthOfImage && MUSICDISPLAYTYPE == 2);
    if (wantSpin) {
      if (!spinning) { // transition: show the spindle once, start turning
        spinning = true;
        lv_obj_remove_flag(spindleHub, LV_OBJ_FLAG_HIDDEN);
      }
      if (my_tick()-previousMillis >= ROTATION_TIMER_MS){
        lv_image_set_rotation(coverImage, angleOfAlbum);
        angleOfAlbum = (angleOfAlbum + ROTATION_STEP_DEG) % 3600;
        previousMillis = my_tick();
      }
    }
    else if (spinning) { // transition: stop and reset once
      spinning = false;
      lv_image_set_rotation(coverImage, 0);
      lv_obj_add_flag(spindleHub, LV_OBJ_FLAG_HIDDEN);
    }

    if (endPosition != 0){
      int progress = (currentPosition * 100) / endPosition;
      if (lastPosition != progress){
        lv_arc_set_value(progressRing, progress);
        lastPosition = progress;
      }
    }
  }
  else { // track info known but the art hasn't finished downloading yet
    if (strcmp(title, lastTitle) != 0 || strcmp(artist, lastArtist) != 0) {
      updateLabels();
    }
  }
  lv_timer_handler();
  delay(5); //testing purposes enabling it decreasing the load and manages the same look
}

void connect_to_wifi() {  // wifi connection
  WiFi.begin(SSID, PASSWORD);
  Serial.print("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi!");
}


void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) { // handles the websocket
  if (DEBUG_LOG) {
    Serial.printf(
      "WS event=%d, length=%u, free heap=%u\n",
      (int)type,
      (unsigned int)length,
      (unsigned int)ESP.getFreeHeap()
    );
  }
  switch (type) {
    case WStype_CONNECTED:
    {
      Serial.println("Connected to PC!");

      JsonDocument doc;
      doc["BGColor"] = backgroundColor;
      doc["DisplayType"] = MUSICDISPLAYTYPE;

      char message[64];
      serializeJson(doc, message, sizeof(message));

      webSocket.sendTXT("GET SET UP");
      webSocket.sendTXT(message);
      break;
    }
    case WStype_DISCONNECTED:
      Serial.println("Disconnected from PC.");
      break;

    case WStype_TEXT:
      {
        // sendAction("SpeedTest"); // network tester is commented out on the server
        JsonDocument doc;

        DeserializationError error = deserializeJson(doc, payload, length);

        if (error) {
          Serial.println("Invalid JSON received");
          break;
        }

        if (doc["type"] == "media_update"){
          volume = doc["volume"] | 0;
          int reportedVolume = constrain((int)volume, 0, 100);
          if (reportedVolume != lastDisplayedVolume) {
            lastDisplayedVolume = reportedVolume;
            showVolumeLabel(reportedVolume);
          }
          playing = doc["playing"] | false;
          copyTrackField(title, sizeof(title), doc["title"] | "Unknown");
          copyTrackField(artist, sizeof(artist), doc["artist"] | "No Artist Available");
          currentPosition = doc["currentPos"] | 1;
          endPosition = doc["endTime"] | 0;
          haveTrackInfo = strcmp(title, "Unknown") != 0; // the PC's fallback for "no info"

          if (DEBUG_LOG) {
            Serial.printf("pos %lu / %lu s\n",
                          (unsigned long)currentPosition, (unsigned long)endPosition);
          }
        }
        else if (doc["type"] == "imageSpecs") {
          widthOfImage = doc["width"] | 0;
          heightOfImage = doc["height"] | 0;

          if (widthOfImage == 0 || heightOfImage == 0 ||
              widthOfImage > 240 || heightOfImage > 240) {
              Serial.println("ERROR: Invalid image dimensions");
              break;
          }

          albumDataSize = (size_t)widthOfImage * heightOfImage * 2;
          lvglCoverImage.data_size = albumDataSize;
          lvglCoverImage.header.w = widthOfImage;
          lvglCoverImage.header.h = heightOfImage;

          //bug when switching between 1:1 and different aspect ratios, the album and offset wrong

          delete[] albumData;
          albumData = new uint8_t[albumDataSize];

          if (albumData == nullptr) {
              Serial.println("ERROR: Album allocation failed");
              break;
          }

          Serial.printf("Allocated %u bytes\n",(unsigned int)albumDataSize);

          bytesReceived = 0;
          applyImageGeometry(); // art size changed: re-fit the existing cover, if one is up
        }
        break;
      }
  case WStype_BIN: {
    if (DEBUG_LOG) {
      Serial.printf("Binary received: %u bytes\n",
                    (unsigned int)length);
    }

    if (albumData == nullptr) {
        Serial.println("ERROR: albumData is nullptr");
        break;
    }

    if (bytesReceived + length > albumDataSize) {
        Serial.println("ERROR: Chunk exceeds buffer capacity");
        break;
    }

    memcpy(albumData + bytesReceived, payload, length);
    bytesReceived += length;

    if (DEBUG_LOG) {
      Serial.printf("Received so far: %u / %u bytes\n",
                    (unsigned int)bytesReceived,
                    (unsigned int)albumDataSize);
    }

    if (bytesReceived == albumDataSize) {
        Serial.println("Complete image received!");
        lvglCoverImage.data = albumData;
        if (coverImage != nullptr) { // a cover is already on screen: push the new pixels
          lv_image_set_src(coverImage, &lvglCoverImage);
          applyImageGeometry();
        }
    }
    break;
  } 
    default:
      break;
  }
}

void sendAction(const char* action) { // use this to send messages to the Python server
  JsonDocument doc;
  doc["action"] = action;

  char message[32];
  serializeJson(doc, message, sizeof(message));
  Serial.println(message);
  webSocket.sendTXT(message);
}

static uint32_t my_tick(void) {
  return millis();
}

void initVinylUI(){
  // progress ring around the record (created first so the art sits over its middle)
  progressRing = lv_arc_create(lv_screen_active());
  lv_obj_set_size(progressRing, RING_SIZE, RING_SIZE);
  lv_obj_align(progressRing, LV_ALIGN_CENTER, 0, -VINYL_Y_OFFSET);
  lv_arc_set_bg_angles(progressRing, 0, 360); // full circle
  lv_arc_set_range(progressRing, 0, 100);
  lv_arc_set_value(progressRing, 0);
  lv_obj_remove_style(progressRing, nullptr, LV_PART_KNOB); // no knob
  lv_obj_set_style_bg_opa(progressRing, LV_OPA_TRANSP, LV_PART_MAIN); // clear the middle, keep only the ring line
  lv_obj_set_style_arc_color(progressRing, lv_color_hex(RING_TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_width(progressRing, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_color(progressRing, lv_color_hex(RING_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(progressRing, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(progressRing, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(progressRing, true, LV_PART_INDICATOR);

  // circular clipping container: the album art becomes the record
  vinylDisc = makeDisc(lv_screen_active(), VINYL_SIZE, BLACK);
  lv_obj_align(vinylDisc, LV_ALIGN_CENTER, 0, -VINYL_Y_OFFSET);
  lv_obj_set_style_clip_corner(vinylDisc, true, 0); // clip children to the circle

  coverImage = lv_image_create(vinylDisc);
  lv_image_set_src(coverImage, &lvglCoverImage);
  lv_obj_align(coverImage, LV_ALIGN_CENTER, 0, 0);
  applyImageGeometry();

  // spindle: small dark hub with a tiny center dot, on top of the rotating art
  spindleHub = makeDisc(lv_screen_active(), 26, 0x0A0A0A);
  lv_obj_align(spindleHub, LV_ALIGN_CENTER, 0, -VINYL_Y_OFFSET);
  lv_obj_t *spindleDot = makeDisc(spindleHub, 5, 0x8A8A8A);
  lv_obj_align(spindleDot, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(spindleHub, LV_OBJ_FLAG_HIDDEN);

  // strip and text belong on top of the art
  lv_obj_move_foreground(textStrip);
  lv_obj_move_foreground(nameOfSong);
  lv_obj_move_foreground(nameOfArtist);
}

void updateLabels(){ // refresh the track text (image geometry is applied when specs/binary arrive)
  strcpy(lastTitle, title);
  strcpy(lastArtist, artist);
  lv_obj_add_flag(placeHolderCover, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(nameOfSong, title);
  lv_obj_set_pos(nameOfSong, 8, 188);
  lv_obj_set_style_text_font(nameOfSong, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(nameOfSong, lv_color_make(255, 255, 255), LV_PART_MAIN);

  lv_label_set_text(nameOfArtist, artist);
  lv_obj_set_pos(nameOfArtist, 8, 208);
  lv_obj_set_style_text_font(nameOfArtist, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_set_style_text_color(nameOfArtist, lv_color_make(160, 160, 160), LV_PART_MAIN);
}

void setUnknownLabels(){
    // no track info yet: show the blank drawn vinyl disc instead of a bitmap
    lv_obj_remove_flag(placeHolderCover, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(nameOfSong, title);
    lv_obj_set_pos(nameOfSong, 8, 188);
    lv_obj_set_style_text_font(nameOfSong, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(nameOfSong, lv_color_make(255, 255, 255), LV_PART_MAIN);

    lv_label_set_text(nameOfArtist, artist);
    lv_obj_set_pos(nameOfArtist, 8, 208);
    lv_obj_set_style_text_font(nameOfArtist, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(nameOfArtist, lv_color_make(160, 160, 160), LV_PART_MAIN);
}

