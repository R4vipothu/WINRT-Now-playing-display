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

#define ACCENT 0x4F80FF

#define VINYL_SIZE 200 // diameter of the album-art disc
#define RING_SIZE 216  // progress ring diameter, slightly larger than the art

#define DRAW_BUF_SIZE (TFT_HOR_RES * TFT_VER_RES / 24 * (LV_COLOR_DEPTH / 8))
uint32_t draw_buf[DRAW_BUF_SIZE / 4];

#define MUSICDISPLAYTYPE 2 //1 for standard, 2 for spinning disk (theres no alpha variable so make the image smaller so the blended background doesnt clip, could prolly adjust when the text is displayed but i dont wanna :) ) 

TFT_eSPI tft = TFT_eSPI();

#include "config.h"  // Wi-Fi credentials + PC address (see config.example.h)

const char* SSID = WIFI_SSID;
const char* PASSWORD = WIFI_PASSWORD;

WebSocketsClient webSocket;

bool setupComplete = false;

uint16_t volume;
bool playing;
String title;
String artist;

uint16_t currentPosition;
uint16_t endPosition;
uint16_t lastPosition;

String lastTitle;
String lastArtist;

uint16_t heightOfImage;
uint16_t widthOfImage;

uint8_t* albumData = nullptr; // allow for the image to different sizes (usually for yt videos)

static lv_image_dsc_t lvglCoverImage;

lv_obj_t *coverImage = nullptr;
lv_obj_t *placeHolderCover = nullptr;
lv_obj_t *progressBar = nullptr;
lv_obj_t *nameOfSong;
lv_obj_t *nameOfArtist;
lv_obj_t *blackCircle;
lv_obj_t *vinylDisc = nullptr; // circular clipping container that holds the art
lv_obj_t *textStrip = nullptr; // semi-transparent strip behind the track info

const int DISTANCE_FROM_TOP = 14; //px the vinyl group is nudged up from center

size_t bytesReceived;
size_t albumDataSize;

int angleOfAlbum;
int ROTATIONTIMER_MS = 40;
unsigned long previousMillis = 0;

//hex codes for colors
uint32_t BLACK = 0x000000;
uint32_t DARK_GRAY = 0x121212;
uint32_t NAVY_BLUE = 0x101828;
uint32_t WHITE = 0xFFFFFF;
uint32_t PURPLE = 0x20102F;
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

void setup() {
  Serial.begin(115200);
  delay(5000);
  
  backgroundColor = BLACK;

  Serial.print("Free Heap Memory: ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);

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
  placeHolderCover = lv_obj_create(lv_screen_active());
  lv_obj_set_size(placeHolderCover, VINYL_SIZE, VINYL_SIZE);
  lv_obj_align(placeHolderCover, LV_ALIGN_CENTER, 0, -DISTANCE_FROM_TOP);
  lv_obj_set_style_radius(placeHolderCover, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(placeHolderCover, lv_color_hex(0x141414), 0);
  lv_obj_set_style_bg_opa(placeHolderCover, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(placeHolderCover, 0, 0);
  lv_obj_set_style_pad_all(placeHolderCover, 0, 0);
  lv_obj_remove_flag(placeHolderCover, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *hole = lv_obj_create(placeHolderCover); // spindle hole of the blank record
  lv_obj_set_size(hole, 26, 26);
  lv_obj_align(hole, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(hole, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(hole, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(hole, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(hole, 0, 0);
  lv_obj_set_style_pad_all(hole, 0, 0);
  lv_obj_remove_flag(hole, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *dot = lv_obj_create(hole); // tiny center dot
  lv_obj_set_size(dot, 5, 5);
  lv_obj_align(dot, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(0x6E6E6E), 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_set_style_pad_all(dot, 0, 0);
  lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
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
  lv_obj_set_size(textStrip, TFT_HOR_RES, 58);
  lv_obj_align(textStrip, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(textStrip, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(textStrip, LV_OPA_30, 0); // faint upper fade
  lv_obj_set_style_border_width(textStrip, 0, 0);
  lv_obj_set_style_radius(textStrip, 0, 0);
  lv_obj_set_style_pad_all(textStrip, 0, 0);
  lv_obj_remove_flag(textStrip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *stripCore = lv_obj_create(textStrip); // stronger lower band -> fake gradient
  lv_obj_set_size(stripCore, TFT_HOR_RES, 34);
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
  title = "Unknown";
  artist = "No Artist Available";
  lastArtist = "null";
  lastTitle = "null";

  currentPosition = 0;
  endPosition = 0;
  lastPosition = -1;

  heightOfImage = 1;
  widthOfImage = 1;

  angleOfAlbum = 0;
}

void loop() {
  webSocket.loop();
  if (title == "Unknown" && artist == "No Artist Available" && title != lastTitle && artist != lastArtist){ 
    setUnknownLabels();

    if(coverImage != nullptr){
      lv_obj_add_flag(coverImage, LV_OBJ_FLAG_HIDDEN);
    }
    if(vinylDisc != nullptr){
      lv_obj_add_flag(vinylDisc, LV_OBJ_FLAG_HIDDEN);
    }

    lastTitle = title;
    lastArtist = artist;
    
  }
  else if (lvglCoverImage.data != nullptr){
    if (coverImage == nullptr){ // creates the cover image for the first time
      initLVGLLabels();
    }
    else{
      lv_obj_remove_flag(coverImage, LV_OBJ_FLAG_HIDDEN);
      if (vinylDisc != nullptr){
        lv_obj_remove_flag(vinylDisc, LV_OBJ_FLAG_HIDDEN);
      }
    }

    if (title != lastTitle || artist != lastArtist) {
      updateLabels();
    }

    if (heightOfImage == widthOfImage && MUSICDISPLAYTYPE == 2 && my_tick()-previousMillis >= ROTATIONTIMER_MS){
      lv_image_set_rotation(coverImage, angleOfAlbum);
      lv_obj_remove_flag(blackCircle, LV_OBJ_FLAG_HIDDEN);
      angleOfAlbum = (angleOfAlbum + 20) % 3600;
      previousMillis = my_tick();
    }
    else{
      lv_image_set_rotation(coverImage, 0);
      lv_obj_add_flag(blackCircle, LV_OBJ_FLAG_HIDDEN);
    }

    if (endPosition != 0){
      int progress = (currentPosition * 100) / endPosition;
      if (lastPosition != progress){
        lv_arc_set_value(progressBar, progress);
        lastPosition = progress;
      }
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
  Serial.printf(
    "WS event=%d, length=%u, free heap=%u\n",
    (int)type,
    (unsigned int)length,
    (unsigned int)ESP.getFreeHeap()
  );
  switch (type) {
    case WStype_CONNECTED:
    {
      Serial.println("Connected to PC!");

      JsonDocument doc;
      doc["BGColor"] = backgroundColor;
      doc["DisplayType"] = MUSICDISPLAYTYPE;

      String message;
      serializeJson(doc, message);

      webSocket.sendTXT("GET SET UP");
      webSocket.sendTXT(message);
      break;
    }
    case WStype_DISCONNECTED:
      Serial.println("Disconnected from PC.");
      break;

    case WStype_TEXT:
      {
        sendAction("SpeedTest"); // to test how long it takes to get and then send back info
        JsonDocument doc;

        DeserializationError error = deserializeJson(doc, payload, length);

        if (error) {
          Serial.println("Invalid JSON received");
          break;
        }

        if (doc["type"] == "media_update"){
          volume = doc["volume"] | 0;
          playing = doc["playing"] | false;
          title = doc["title"] | "Unknown";
          artist = doc["artist"] | "No Artist Available";
          currentPosition = doc["currentPos"] | 1;
          endPosition = doc["endTime"] | 0;

          Serial.println(doc["currentPos"].as<float>());
          Serial.println(doc["endTime"].as<float>());
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
        }
        break;
      }
  case WStype_BIN: {
     Serial.printf("Binary received: %u bytes\n",
                  (unsigned int)length);

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

    Serial.printf("Received so far: %u / %u bytes\n",
                  (unsigned int)bytesReceived,
                  (unsigned int)albumDataSize);

    if (bytesReceived == albumDataSize) {
        Serial.println("Complete image received!");
        lvglCoverImage.data = albumData;
    }
    break;
  } 
    default:
      break;
  }
}

void sendAction(const char* action) { // use this to send messages to the python
  JsonDocument doc;
  doc["action"] = action;

  String message;
  serializeJson(doc, message);
  Serial.println(message);
  webSocket.sendTXT(message);
}

static uint32_t my_tick(void) {
  return millis();
}

static void fitArtToDisc(){ // scale the art so its larger side fills the 200px disc
  uint16_t largest = lvglCoverImage.header.w > lvglCoverImage.header.h ? lvglCoverImage.header.w : lvglCoverImage.header.h;
  if (largest == 0) largest = 1;
  lv_image_set_scale(coverImage, 256 * VINYL_SIZE / largest); // 256 = 100% in LVGL v9
}

void initLVGLLabels(){
  // progress ring around the record (created first so the art sits over its middle)
  progressBar = lv_arc_create(lv_screen_active());
  lv_obj_set_size(progressBar, RING_SIZE, RING_SIZE);
  lv_obj_align(progressBar, LV_ALIGN_CENTER, 0, -DISTANCE_FROM_TOP);
  lv_arc_set_bg_angles(progressBar, 0, 360); // full circle
  lv_arc_set_range(progressBar, 0, 100);
  lv_arc_set_value(progressBar, 0);
  lv_obj_remove_style(progressBar, NULL, LV_PART_KNOB); // no knob
  lv_obj_set_style_bg_opa(progressBar, LV_OPA_TRANSP, LV_PART_MAIN); // clear the middle, keep only the ring line
  lv_obj_set_style_arc_color(progressBar, lv_color_hex(RING_TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_width(progressBar, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_color(progressBar, lv_color_hex(RING_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(progressBar, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(progressBar, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(progressBar, true, LV_PART_INDICATOR);

  // circular clipping container: the album art becomes the record
  vinylDisc = lv_obj_create(lv_screen_active());
  lv_obj_set_size(vinylDisc, VINYL_SIZE, VINYL_SIZE);
  lv_obj_align(vinylDisc, LV_ALIGN_CENTER, 0, -DISTANCE_FROM_TOP);
  lv_obj_set_style_radius(vinylDisc, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_clip_corner(vinylDisc, true, 0); // clip children to the circle
  lv_obj_set_style_bg_color(vinylDisc, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(vinylDisc, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(vinylDisc, 0, 0);
  lv_obj_set_style_pad_all(vinylDisc, 0, 0);
  lv_obj_remove_flag(vinylDisc, LV_OBJ_FLAG_SCROLLABLE);

  coverImage = lv_image_create(vinylDisc);
  lv_image_set_src(coverImage, &lvglCoverImage);
  lv_obj_align(coverImage, LV_ALIGN_CENTER, 0, 0);
  fitArtToDisc();
  lv_image_set_pivot(coverImage, lvglCoverImage.header.w/2, lvglCoverImage.header.h/2);

  // spindle: small dark hub with a tiny center dot, on top of the rotating art
  blackCircle = lv_obj_create(lv_screen_active());
  lv_obj_set_size(blackCircle, 26, 26);
  lv_obj_align(blackCircle, LV_ALIGN_CENTER, 0, -DISTANCE_FROM_TOP);
  lv_obj_set_style_radius(blackCircle, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(blackCircle, lv_color_hex(0x0A0A0A), 0);
  lv_obj_set_style_bg_opa(blackCircle, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(blackCircle, 0, 0);
  lv_obj_set_style_pad_all(blackCircle, 0, 0);
  lv_obj_remove_flag(blackCircle, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *spindleDot = lv_obj_create(blackCircle);
  lv_obj_set_size(spindleDot, 5, 5);
  lv_obj_align(spindleDot, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(spindleDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(spindleDot, lv_color_hex(0x8A8A8A), 0);
  lv_obj_set_style_bg_opa(spindleDot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(spindleDot, 0, 0);
  lv_obj_set_style_pad_all(spindleDot, 0, 0);
  lv_obj_remove_flag(spindleDot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(blackCircle, LV_OBJ_FLAG_HIDDEN);

  // strip and text belong on top of the art
  lv_obj_move_foreground(textStrip);
  lv_obj_move_foreground(nameOfSong);
  lv_obj_move_foreground(nameOfArtist);
}

void updateLabels(){ // updates every loop (there is an image)
  lv_image_set_src(coverImage, &lvglCoverImage);
  fitArtToDisc();
  lv_image_set_pivot(coverImage, lvglCoverImage.header.w/2, lvglCoverImage.header.h/2);
  lastTitle = title;
  lastArtist = artist;
  lv_obj_add_flag(placeHolderCover, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(nameOfSong, title.c_str());
  lv_obj_set_pos(nameOfSong, 8, 188);
  lv_obj_set_style_text_font(nameOfSong, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(nameOfSong, lv_color_make(255, 255, 255), LV_PART_MAIN);

  lv_label_set_text(nameOfArtist, artist.c_str());
  lv_obj_set_pos(nameOfArtist, 8, 208);
  lv_obj_set_style_text_font(nameOfArtist, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_set_style_text_color(nameOfArtist, lv_color_make(160, 160, 160), LV_PART_MAIN);
}

void setUnknownLabels(){
    // no album art yet: show the blank drawn vinyl disc instead of a bitmap
    lv_obj_remove_flag(placeHolderCover, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(nameOfSong, title.c_str());
    lv_obj_set_pos(nameOfSong, 8, 188);
    lv_obj_set_style_text_font(nameOfSong, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(nameOfSong, lv_color_make(255, 255, 255), LV_PART_MAIN);

    lv_label_set_text(nameOfArtist, artist.c_str());
    lv_obj_set_pos(nameOfArtist, 8, 208);
    lv_obj_set_style_text_font(nameOfArtist, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(nameOfArtist, lv_color_make(160, 160, 160), LV_PART_MAIN);
}

