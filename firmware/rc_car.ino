/*
  Робот-автомобіль з віддаленим керуванням на ESP32-S3-CAM (HW-679)

  Що робить:
    - піднімає Wi-Fi точку доступу (адреса 192.168.4.1)
    - стрімить відео з камери (MJPEG) на порту 81: /stream
    - віддає веб-сторінку з джойстиком на порту 80: /
    - приймає команди руху: /cmd?x=<-100..100>&y=<-100..100>
    - зупиняє мотори, якщо команд немає понад FAILSAFE_MS

  Потрібно: ESP32 Arduino core 3.x
  Налаштування Arduino IDE:
    Board: ESP32S3 Dev Module
    USB CDC On Boot: Disabled   (Serial іде на UART0: IO43/IO44)
    PSRAM: OPI PSRAM
    Flash Size: 16MB
    Flash Mode: QIO 80MHz
    Partition Scheme: 16M Flash (3MB APP/9.9MB FATFS) або Default

  Мотори (TB6612FNG): PWMA, PWMB, STBY підтягнуті до 3V3,
  швидкість задається ШІМ прямо на AIN1/AIN2 та BIN1/BIN2.
*/

#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"

// ===================== Wi-Fi =====================
const char* AP_SSID = "RC-Car";
const char* AP_PASS = "rccar1234";  // мінімум 8 символів, змініть за бажанням

// ===================== Мотори =====================
#define A_IN1 41   // AIN1
#define A_IN2 40   // AIN2
#define B_IN1 39   // BIN1
#define B_IN2 38   // BIN2

const bool INVERT_A = false;  // true, якщо мотор A крутиться у зворотний бік
const bool INVERT_B = false;  // те саме для мотора B

const int PWM_FREQ = 20000;   // 20 кГц (поза чутним діапазоном)
const int PWM_BITS = 8;       // значення 0..255
const int MIN_DUTY = 100;     // мінімальний ШІМ, при якому мотор рушає (підібрати)
const int MAX_DUTY = 220;     // обмеження зверху (мотори на 3-4,5 В, VM = 5 В)
const uint32_t FAILSAFE_MS = 500;

// ===================== Камера =====================
// УВАГА: розпіновку камери перевірте за документацією плати HW-679!
// Нижче типова розкладка плат ESP32-S3-CAM (як у Freenove).
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM   4
#define SIOC_GPIO_NUM   5
#define Y2_GPIO_NUM    11
#define Y3_GPIO_NUM     9
#define Y4_GPIO_NUM     8
#define Y5_GPIO_NUM    10
#define Y6_GPIO_NUM    12
#define Y7_GPIO_NUM    18
#define Y8_GPIO_NUM    17
#define Y9_GPIO_NUM    16
#define VSYNC_GPIO_NUM  6
#define HREF_GPIO_NUM   7
#define PCLK_GPIO_NUM  13

// ===================== Стан =====================
static httpd_handle_t ctrlHttpd = nullptr;
static httpd_handle_t streamHttpd = nullptr;
static volatile uint32_t lastCmdMs = 0;

// ===================== Мотори =====================
static void driveMotor(int in1, int in2, int speed, bool invert) {
  if (invert) speed = -speed;
  int duty = 0;
  if (speed != 0) duty = map(abs(speed), 1, 100, MIN_DUTY, MAX_DUTY);
  if (speed > 0) {
    ledcWrite(in1, duty);
    ledcWrite(in2, 0);
  } else if (speed < 0) {
    ledcWrite(in1, 0);
    ledcWrite(in2, duty);
  } else {
    ledcWrite(in1, 0);
    ledcWrite(in2, 0);
  }
}

// throttle: вперед/назад, turn: вправо/вліво (обидва -100..100)
static void setDrive(int throttle, int turn) {
  int left = constrain(throttle + turn, -100, 100);
  int right = constrain(throttle - turn, -100, 100);
  driveMotor(A_IN1, A_IN2, left, INVERT_A);
  driveMotor(B_IN1, B_IN2, right, INVERT_B);
}

// ===================== Веб-сторінка =====================
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>RC-Car</title>
<style>
html,body{margin:0;height:100%;background:#111;color:#eee;font-family:sans-serif;touch-action:none;overflow:hidden}
#v{position:absolute;inset:0;width:100%;height:100%;object-fit:contain;background:#000}
#pad{position:absolute;left:4vmin;bottom:4vmin;width:38vmin;height:38vmin;border-radius:50%;
background:rgba(255,255,255,.12);border:2px solid rgba(255,255,255,.35)}
#knob{position:absolute;left:50%;top:50%;width:14vmin;height:14vmin;margin:-7vmin 0 0 -7vmin;
border-radius:50%;background:rgba(255,255,255,.55)}
#st{position:absolute;top:8px;left:10px;font-size:14px;opacity:.8}
</style></head><body>
<img id="v" alt="">
<div id="st">...</div>
<div id="pad"><div id="knob"></div></div>
<script>
const pad=document.getElementById('pad'),knob=document.getElementById('knob'),st=document.getElementById('st');
document.getElementById('v').src='http://'+location.hostname+':81/stream';
let x=0,y=0,active=false,busy=false,last='0,0';
function upd(e){
  const r=pad.getBoundingClientRect();
  let dx=(e.clientX-(r.left+r.width/2))/(r.width/2);
  let dy=((r.top+r.height/2)-e.clientY)/(r.height/2);
  const m=Math.hypot(dx,dy); if(m>1){dx/=m;dy/=m;}
  x=Math.round(dx*100); y=Math.round(dy*100);
  knob.style.left=(50+dx*40)+'%'; knob.style.top=(50-dy*40)+'%';
}
pad.addEventListener('pointerdown',e=>{active=true;pad.setPointerCapture(e.pointerId);upd(e);});
pad.addEventListener('pointermove',e=>{if(active)upd(e);});
function end(){active=false;x=0;y=0;knob.style.left='50%';knob.style.top='50%';}
pad.addEventListener('pointerup',end);
pad.addEventListener('pointercancel',end);
window.addEventListener('blur',end);
async function send(){
  busy=true;
  try{await fetch('/cmd?x='+x+'&y='+y,{cache:'no-store'});st.textContent='online';}
  catch(e){st.textContent='offline';}
  busy=false;
}
setInterval(()=>{
  if(busy) return;
  const s=x+','+y;
  if(active||s!==last){last=s;send();}
},60);
</script></body></html>
)rawliteral";

// ===================== HTTP-обробники =====================
static esp_err_t indexHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t cmdHandler(httpd_req_t* req) {
  char query[64];
  char val[16];
  int throttle = 0, turn = 0;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    if (httpd_query_key_value(query, "y", val, sizeof(val)) == ESP_OK) throttle = atoi(val);
    if (httpd_query_key_value(query, "x", val, sizeof(val)) == ESP_OK) turn = atoi(val);
  }
  setDrive(constrain(throttle, -100, 100), constrain(turn, -100, 100));
  lastCmdMs = millis();
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, "ok", 2);
}

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CT = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t streamHandler(httpd_req_t* req) {
  esp_err_t res = httpd_resp_set_type(req, STREAM_CT);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char part[64];
  while (true) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
    } else {
      size_t hlen = snprintf(part, sizeof(part), STREAM_PART, (unsigned)fb->len);
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, part, hlen);
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
      esp_camera_fb_return(fb);
    }
    if (res != ESP_OK) break;  // клієнт відключився
  }
  return res;
}

static void startServers() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;

  httpd_uri_t indexUri = {.uri = "/", .method = HTTP_GET, .handler = indexHandler, .user_ctx = NULL};
  httpd_uri_t cmdUri = {.uri = "/cmd", .method = HTTP_GET, .handler = cmdHandler, .user_ctx = NULL};
  if (httpd_start(&ctrlHttpd, &cfg) == ESP_OK) {
    httpd_register_uri_handler(ctrlHttpd, &indexUri);
    httpd_register_uri_handler(ctrlHttpd, &cmdUri);
  }

  // Відео на окремому сервері, щоб потік не блокував команди керування
  cfg.server_port = 81;
  cfg.ctrl_port += 1;
  httpd_uri_t streamUri = {.uri = "/stream", .method = HTTP_GET, .handler = streamHandler, .user_ctx = NULL};
  if (httpd_start(&streamHttpd, &cfg) == ESP_OK) {
    httpd_register_uri_handler(streamHttpd, &streamUri);
  }
}

// ===================== Камера =====================
static bool initCamera() {
  camera_config_t c = {};
  // Камері віддаємо timer 3 / channel 7, щоб не конфліктувати з ШІМ моторів
  c.ledc_channel = LEDC_CHANNEL_7;
  c.ledc_timer = LEDC_TIMER_3;
  c.pin_d0 = Y2_GPIO_NUM;
  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;
  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;
  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;  // якщо в кадрі шум, спробуйте 10000000
  c.pixel_format = PIXFORMAT_JPEG;
  c.grab_mode = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    c.frame_size = FRAMESIZE_VGA;  // 640x480
    c.jpeg_quality = 12;           // 10..63, менше число = краща якість
    c.fb_count = 2;
    c.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    c.frame_size = FRAMESIZE_QVGA;
    c.jpeg_quality = 15;
    c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }
  return true;
}

// ===================== setup / loop =====================
void setup() {
  Serial.begin(115200);
  delay(200);

  // ШІМ для моторів
  ledcAttach(A_IN1, PWM_FREQ, PWM_BITS);
  ledcAttach(A_IN2, PWM_FREQ, PWM_BITS);
  ledcAttach(B_IN1, PWM_FREQ, PWM_BITS);
  ledcAttach(B_IN2, PWM_FREQ, PWM_BITS);
  setDrive(0, 0);

  if (!initCamera()) {
    Serial.println("Камера не запустилась: перевірте піни та шлейф. Керування працюватиме без відео.");
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 2);  // канал 1, видима, до 2 клієнтів
  WiFi.setSleep(false);

  startServers();
  lastCmdMs = millis();

  Serial.print("Wi-Fi: ");
  Serial.println(AP_SSID);
  Serial.print("Відкрийте http://");
  Serial.println(WiFi.softAPIP());
}

void loop() {
  // Захист: немає команд — зупинити мотори
  if (millis() - lastCmdMs > FAILSAFE_MS) {
    setDrive(0, 0);
  }
  delay(20);
}
