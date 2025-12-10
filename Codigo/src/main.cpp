#include <WiFi.h>
#include <WebServer.h>
#include "esp_camera.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "esp_http_server.h"

// ---------------------- CONFIG WIFI ----------------------
const char* ssid = "ROV NAUTILUS";
const char* password = "Rov-Nautilus12";

// ---------------------- PINES MOTORES ----------------------
#define LI_A 13
#define LI_B 12
#define LD_A 14
#define LD_B 15
#define MV_A 2
#define MV_B 4

WebServer server(80);

// ---------------------- ESTADOS ----------------------
bool f_up = false;
bool f_down = false;
bool f_left = false;
bool f_right = false;
bool f_forward = false;
bool f_reverse = false;
bool f_power = false;
bool f_cam = false;   // <-- NUEVO: cámara encendida/apagada

httpd_handle_t stream_httpd = NULL;

// ---------------------- HTML ----------------------
String htmlPage = R"=====(
<!DOCTYPE html>
<html>
<head>
<title>NAUTILUS</title>
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<style>
body { background:#111; color:white; text-align:center; font-family:Arial; }

/* VIDEO */
#camBox {
  width:100%;
  max-width:350px;
  height:auto;
  margin:auto;
  border:2px solid #0af;
  border-radius:10px;
  display:none;
}

/* BOTONES */
button{
 width:120px; height:60px; margin:5px;
 background:#222; color:white;
 border:2px solid #0af;
 border-radius:10px; font-size:18px;
 transition:0.3s;
}

button.active { background:#0af; color:black; }
#power.active { background:red !important; border-color:red !important; }
#cam.active { background:green !important; border-color:green !important; }

.disabled { opacity:0.3; pointer-events:none; }

#netStatus {
 position:fixed; top:10px; left:50%;
 transform:translateX(-50%);
 background:red; padding:10px 20px;
 border-radius:10px; font-size:18px;
 display:none;
}
</style>
</head>
<body>

<h2>NAUTILUS</h2>

<div id="netStatus">Desconectado</div>

<!-- STREAM VIDEO -->
<img id="camBox" src="">

<!-- BOTÓN CÁMARA -->
<button id="cam" onclick="toggle('cam')" style="border-color:green;">Camara</button><br><br>

<!-- POWER -->
<button id="power" onclick="toggle('power')" style="border-color:red;">OFF</button><br><br>

<!-- CONTROLES -->
<button id="forward" class="disabled" onclick="toggle('forward')">Adelante</button><br>
<button id="left" class="disabled" onclick="toggle('left')">Izquierda</button>
<button id="right" class="disabled" onclick="toggle('right')">Derecha</button><br>
<button id="reverse" class="disabled" onclick="toggle('reverse')">Reversa</button><br><br>
<button id="up" class="disabled" onclick="toggle('up')">Subir</button>
<button id="down" class="disabled" onclick="toggle('down')">Bajar</button>

<script>

let wasDisconnected = false;

// Actualizar UI
function updateUI(states){

    for (const key in states){
        const btn = document.getElementById(key);
        if (!btn) continue;

        if(states[key] == 1) btn.classList.add("active");
        else btn.classList.remove("active");
    }

    // Texto POWER
    document.getElementById("power").innerText = states.power ? "ON" : "OFF";

    // Cámara
    const camBox = document.getElementById("camBox");
    if(states.cam){
        camBox.style.display = "block";
        camBox.src = "http://" + location.hostname + ":81/stream";
    } else {
        camBox.style.display = "none";
        camBox.src = "";
    }

    // Deshabilitar controles cuando POWER está OFF
    const disabled = !states.power;
    ["forward","reverse","left","right","up","down"].forEach(id=>{
        const b = document.getElementById(id);
        if (disabled) b.classList.add("disabled");
        else b.classList.remove("disabled");
    });
}

// Ping conexión
function checkConnection(){
    fetch("/ping")
    .then(r => r.text())
    .then(t => {
        if (wasDisconnected){
            const n = document.getElementById("netStatus");
            n.style.display = "block";
            n.style.background = "green";
            n.innerText = "Conectado";
            setTimeout(()=>{ n.style.display="none"; }, 2000);
        }
        wasDisconnected = false;
    })
    .catch(err=>{
        const n = document.getElementById("netStatus");
        n.style.display = "block";
        n.style.background = "red";
        n.innerText = "Desconectado";
        wasDisconnected = true;
    });
}

setInterval(checkConnection, 1200);

// Enviar comandos
function toggle(cmd){
    fetch("/cmd?c=" + cmd)
    .then(r => r.json())
    .then(data => updateUI(data));
}
</script>

</body>
</html>
)=====";


// ---------------------- STREAM CAMARA ----------------------
static esp_err_t stream_handler(httpd_req_t *req){
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;

  char part_buf[64];
  res = httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");

  while(true){
    if(!f_cam) break;  // SI APAGAN CAMARA → SE SALE

    fb = esp_camera_fb_get();
    if(!fb) continue;

    if(fb->format != PIXFORMAT_JPEG){
      bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
      if(!jpeg_converted){
        esp_camera_fb_return(fb);
        continue;
      }
    } else {
      _jpg_buf = fb->buf;
      _jpg_buf_len = fb->len;
    }

    size_t hlen = snprintf(part_buf, 64, "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", _jpg_buf_len);

    httpd_resp_send_chunk(req, part_buf, hlen);
    httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    httpd_resp_send_chunk(req, "\r\n", 2);

    if(fb->format != PIXFORMAT_JPEG) free(_jpg_buf);
    esp_camera_fb_return(fb);
  }

  return ESP_OK;
}

void startCameraServer(){
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 81;

  httpd_uri_t uri_stream = {
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };

  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &uri_stream);
  }
}

// ---------------------- MOTORES ----------------------
void updateMotors() {

  if (!f_power){
    digitalWrite(LI_A, LOW);
    digitalWrite(LI_B, LOW);
    digitalWrite(LD_A, LOW);
    digitalWrite(LD_B, LOW);
    digitalWrite(MV_A, LOW);
    digitalWrite(MV_B, LOW);
    return;
  }

  digitalWrite(LI_A, LOW);
  digitalWrite(LI_B, LOW);
  digitalWrite(LD_A, LOW);
  digitalWrite(LD_B, LOW);
  digitalWrite(MV_A, LOW);
  digitalWrite(MV_B, LOW);

  if (f_forward) digitalWrite(LI_A, HIGH), digitalWrite(LD_A, HIGH);
  if (f_reverse) digitalWrite(LI_B, HIGH), digitalWrite(LD_B, HIGH);
  if (f_left)    digitalWrite(LD_A, HIGH);
  if (f_right)   digitalWrite(LI_A, HIGH);
  if (f_up)      digitalWrite(MV_A, HIGH);
  if (f_down)    digitalWrite(MV_B, HIGH);
}

// ---------------------- JSON ----------------------
String getJSON(){
  String j = "{";
  j += "\"forward\":" + String(f_forward) + ",";
  j += "\"reverse\":" + String(f_reverse) + ",";
  j += "\"left\":"    + String(f_left)    + ",";
  j += "\"right\":"   + String(f_right)   + ",";
  j += "\"up\":"      + String(f_up)      + ",";
  j += "\"down\":"    + String(f_down)    + ",";
  j += "\"power\":"   + String(f_power)   + ",";
  j += "\"cam\":"     + String(f_cam);
  j += "}";
  return j;
}

// ---------------------- SETUP ----------------------
void setup(){
  Serial.begin(115200);

  // Pines motores
  pinMode(LI_A, OUTPUT);
  pinMode(LI_B, OUTPUT);
  pinMode(LD_A, OUTPUT);
  pinMode(LD_B, OUTPUT);
  pinMode(MV_A, OUTPUT);
  pinMode(MV_B, OUTPUT);

  // ---------------------- CONFIGURAR CAMARA ----------------------
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 5;
  config.pin_d1 = 18;
  config.pin_d2 = 19;
  config.pin_d3 = 21;
  config.pin_d4 = 36;
  config.pin_d5 = 39;
  config.pin_d6 = 34;
  config.pin_d7 = 35;
  config.pin_xclk = 0;
  config.pin_pclk = 22;
  config.pin_vsync = 25;
  config.pin_href = 23;
  config.pin_sccb_sda = 26;
  config.pin_sccb_scl = 27;
  config.pin_pwdn = 32;
  config.pin_reset = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 10;
  config.fb_count = 1;

  esp_camera_init(&config);

  // AP WiFi
  WiFi.softAP(ssid, password);
  Serial.println(WiFi.softAPIP());

  // SERVICIOS WEB
  server.on("/", [](){ server.send(200, "text/html", htmlPage); });
  server.on("/ping", [](){ server.send(200, "text/plain", "ok"); });

  server.on("/cmd", [](){
    String c = server.arg("c");

    if(c == "cam"){
      f_cam = !f_cam;
      if(f_cam) startCameraServer();
    }

    if(c == "power"){
      f_power = !f_power;
      if(!f_power){
        f_forward = f_reverse = f_left = f_right = f_up = f_down = false;
      }
    }

    if(!f_power){
      updateMotors();
      server.send(200, "application/json", getJSON());
      return;
    }

    if(c=="forward"){ f_forward=!f_forward; if(f_forward) f_reverse=0; }
    if(c=="reverse"){ f_reverse=!f_reverse; if(f_reverse) f_forward=0; }
    if(c=="left"){ f_left=!f_left; if(f_left) f_right=0; }
    if(c=="right"){ f_right=!f_right; if(f_right) f_left=0; }
    if(c=="up"){ f_up=!f_up; if(f_up) f_down=0; }
    if(c=="down"){ f_down=!f_down; if(f_down) f_up=0; }

    updateMotors();
    server.send(200, "application/json", getJSON());
  });

  server.begin();
}

// ---------------------- LOOP ----------------------
void loop(){
  server.handleClient();
}
