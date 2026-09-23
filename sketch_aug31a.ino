#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <AnimatedGIF.h>
#include <esp_sleep.h>
#include <time.h>
#include "freertos/semphr.h"

#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "Display_ST7701.h"
#include "Level_IMU.h"

// 留空时设备建立热点；填写后设备接入家中 Wi-Fi。
constexpr char WIFI_SSID[] = "";
constexpr char WIFI_PASSWORD[] = "";
constexpr char AP_SSID[] = "CircleFrame";
constexpr char AP_PASSWORD[] = "12345678";
// false：不插 SD 卡也可使用，照片暂存 PSRAM，重启后会清空。
// true：照片保存到 Micro SD 卡，重启后仍保留。
constexpr bool USE_SD_CARD = true;
constexpr uint16_t SCREEN_SIZE = 480;
constexpr size_t IMAGE_BYTES = SCREEN_SIZE * SCREEN_SIZE * 2;
constexpr uint8_t ROTATION_TILE_SIZE = 48;
constexpr uint8_t ROTATION_CACHE_SIZE = 72; // 48x48 rotated by 45 degrees plus margins.
constexpr uint8_t MAX_IMAGES = USE_SD_CARD ? 30 : 10;
constexpr uint8_t MAX_GIFS = 30;

constexpr char GIF_TEMP_PATH[] = "/animation.tmp";
constexpr uint32_t DOUBLE_CLICK_MS = 350;
constexpr uint32_t LONG_PRESS_MS = 1000;
constexpr uint32_t SLEEP_PRESS_MS = 3000;
uint32_t levelUpdateMs = 20;
float levelDeadBandDeg = 0.7f;
constexpr float LEVEL_ANGLE_OFFSET_DEG = 90.0f; // Default image orientation: 90 degrees clockwise.
constexpr float LEVEL_ROTATION_SIGN = -1.0f;    // Change to +1 if compensation runs backwards.
constexpr float BRIGHTNESS_TILT_SIGN = 1.0f;    // Change to -1 if left/right brightness feels reversed.
constexpr float BRIGHTNESS_DEGREES_PER_STEP = 2.0f;
constexpr uint8_t BRIGHTNESS_PERCENT_PER_STEP = 2;

WebServer server(80);
Preferences preferences;
uint8_t imageCount = 0;
int8_t currentImage = -1;
bool sdReady = false, uploadOk = false;
String uploadPath;
File uploadFile;
File animationFile;
AnimatedGIF gifDecoder;
uint8_t *ramImages[MAX_IMAGES] = {};
uint8_t *uploadBuffer = nullptr;
uint8_t *displayBuffer = nullptr;  // Must remain valid while the RGB DMA reads it.
const uint16_t *levelSource = nullptr; // Full-resolution 480x480 RGB565 source.
uint16_t *frameBuffers[2] = {};
uint16_t rotationTileCache[ROTATION_CACHE_SIZE * ROTATION_CACHE_SIZE];
uint16_t rotationOutputTile[ROTATION_TILE_SIZE * ROTATION_TILE_SIZE];
SemaphoreHandle_t displayVsync = nullptr;
uint8_t visibleFrame = 0;
bool imuReady = false;
bool settingsActive = false;
bool wifiUploadActive = false;
bool serverRoutesReady = false;
bool serverRunning = false;
bool stationConnected = false;
String savedWifiSsid;
String savedWifiPassword;
bool brightnessAdjustActive = false;
uint8_t settingsFocus = 0;
uint8_t screenBrightness = 100;
uint8_t brightnessAtAdjustStart = 100;
float brightnessReferenceAngle = 0.0f;
bool levelLockEnabled = true;
bool brakeLightEnabled = false;
bool brakeLightActive = false;
bool autoBrakeLightEnabled = false;
bool solarLocationValid = false;
bool ntpStarted = false;
double solarLatitude = 0.0;
double solarLongitude = 0.0;
int32_t solarUtcOffsetSeconds = 0;
float sunriseLocalHour = 6.0f;
float sunsetLocalHour = 18.0f;
int16_t solarScheduleDay = -1;
uint32_t lastSolarLocationAttempt = 0;
uint32_t lastAutoBrakeCheck = 0;
String autoBrakeStatus = "AUTO WAIT WIFI";
bool animationAvailable = false;
bool animationPlaying = false;
bool gifDecoderOpen = false;
uint8_t gifCount = 0;
int8_t currentGif = -1;
uint8_t *gifFrameBuffer = nullptr;
uint16_t gifCanvasWidth = 0;
uint16_t gifCanvasHeight = 0;
uint32_t nextAnimationFrameAt = 0;
size_t gifUploadBytes = 0;
bool gifUploadOk = false;
float levelAngle = 0.0f, renderedAngle = 1000.0f;
uint32_t lastLevelRender = 0;
size_t uploadBytes = 0;
bool buttonDown = false;
bool longPressHandled = false;
bool sleepPressHandled = false;
bool pressStartedInSettings = false;
uint32_t buttonDownAt = 0, lastButtonChange = 0, lastClickAt = 0;
uint8_t clickCount = 0;
uint8_t lastWifiFlashPhase = 0xFF;

enum SettingsItem : uint8_t { SETTINGS_BRIGHTNESS, SETTINGS_WIFI, SETTINGS_BRAKE_LIGHT, SETTINGS_AUTO_BRAKE, SETTINGS_BACK, SETTINGS_ITEM_COUNT };
enum PowerMode : uint8_t { MODE_PERFORMANCE, MODE_BALANCED, MODE_POWER_SAVE };
PowerMode powerMode = MODE_PERFORMANCE;

void drawSettingsPage();
void startUploadWiFi();
void stopUploadWiFi();
void leaveSettings();
// Performance-mode selection is temporarily disabled; the stable performance profile remains active.
// void applyPowerMode(PowerMode mode);
void updateAutoBrakeControl(bool forceNetwork = false);
void enterDeepSleep();
void loadWifiCredentials();
void handleWifiSetupPage();
void handleWifiScan();
void handleWifiConnect();
void handleWifiStatus();
void handleGifPage();
void handleGifUploadDone();
void handleGifUpload();
void handleMediaPage();
void handleMediaStatus();
void handleMediaDelete();
bool startAnimation(uint8_t index);
bool showAnimationFrame();

const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><meta name="viewport" content="width=device-width,initial-scale=1"><title>圆形相框上传</title><style>body{margin:0;background:#101114;color:#f4f4f5;font:16px system-ui;text-align:center}main{max-width:520px;margin:auto;padding:28px 18px}canvas{width:min(82vw,360px);height:min(82vw,360px);border-radius:50%;background:#000;display:block;margin:20px auto;border:2px solid #45464c}input,button{font:inherit;margin:8px;padding:11px;border-radius:9px;border:0}button{background:#4f7cff;color:#fff;font-weight:700}small{color:#b8bbc5;line-height:1.6;display:block}</style><main><h2>圆形相框</h2><small>选择照片后会自动居中裁切为圆形，再上传到相框。</small><input id="file" type="file" accept="image/*"><canvas id="c" width="480" height="480"></canvas><button id="send" disabled>上传这张图片</button><small id="msg">加载中…</small></main><script>const c=document.querySelector('#c'),ctx=c.getContext('2d'),file=document.querySelector('#file'),send=document.querySelector('#send'),msg=document.querySelector('#msg');let imageReady=false;function draw(img){const s=Math.max(480/img.naturalWidth,480/img.naturalHeight),w=img.naturalWidth*s,h=img.naturalHeight*s;ctx.clearRect(0,0,480,480);ctx.save();ctx.beginPath();ctx.arc(240,240,240,0,Math.PI*2);ctx.clip();ctx.drawImage(img,(480-w)/2,(480-h)/2,w,h);ctx.restore();imageReady=true;send.disabled=false}file.onchange=()=>{const f=file.files[0];if(!f)return;const im=new Image;im.onload=()=>{draw(im);URL.revokeObjectURL(im.src)};im.src=URL.createObjectURL(f)};send.onclick=async()=>{if(!imageReady)return;send.disabled=true;msg.textContent='正在转换及上传…';const p=ctx.getImageData(0,0,480,480).data,raw=new Uint8Array(480*480*2);for(let i=0,j=0;i<p.length;i+=4){let v=((p[i]&248)<<8)|((p[i+1]&252)<<3)|(p[i+2]>>3);raw[j++]=v&255;raw[j++]=v>>8}const f=new FormData;f.append('image',new Blob([raw],{type:'application/octet-stream'}),'circle.rgb');try{const r=await fetch('/upload',{method:'POST',body:f});msg.textContent=await r.text()}catch(e){msg.textContent='上传失败：'+e}send.disabled=false;};fetch('/status').then(r=>r.json()).then(x=>msg.textContent='已保存 '+x.count+' 张。'+x.hint).catch(()=>msg.textContent='');</script></html>)HTML";

const char WIFI_SETUP_PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Wi-Fi 配网</title><style>body{margin:0;background:#101114;color:#f4f4f5;font:16px system-ui}main{max-width:520px;margin:auto;padding:28px 18px}input,select,button{box-sizing:border-box;width:100%;font:inherit;margin:8px 0;padding:12px;border-radius:9px;border:0}button{background:#4f7cff;color:#fff;font-weight:700}button.alt{background:#333740}small{color:#b8bbc5;line-height:1.6;display:block}a{color:#9db6ff}</style><main><h2>Wi-Fi 配网</h2><small>保存成功后，设备每次进入“WiFi 上传”都会自动连接此网络；离开设置页后仅关闭设备热点，路由器 Wi-Fi 保持连接。</small><button id="scan" class="alt">扫描附近 Wi-Fi</button><select id="ssid"><option value="">请先扫描并选择网络</option></select><input id="password" type="password" placeholder="Wi-Fi 密码（开放网络可留空）" autocomplete="current-password"><button id="save">保存并连接</button><small id="msg">正在读取状态…</small><p><a href="/">返回图片上传</a></p></main><script>const ssid=document.querySelector('#ssid'),password=document.querySelector('#password'),scan=document.querySelector('#scan'),save=document.querySelector('#save'),msg=document.querySelector('#msg');async function status(){try{const x=await (await fetch('/wifi/status')).json();msg.textContent=x.connected?'已连接 '+x.ssid:(x.saved?'已保存网络：'+x.ssid:'尚未保存 Wi-Fi');}catch(e){msg.textContent='无法读取状态';}}scan.onclick=async()=>{scan.disabled=true;msg.textContent='正在扫描…';try{const x=await (await fetch('/wifi/scan')).json();ssid.replaceChildren();if(!x.networks.length){ssid.add(new Option('未发现网络',''));}x.networks.forEach(n=>ssid.add(new Option(n.ssid+' ('+n.rssi+' dBm)',n.ssid)));msg.textContent='请选择网络并输入密码';}catch(e){msg.textContent='扫描失败，请重试';}scan.disabled=false;};save.onclick=async()=>{if(!ssid.value){msg.textContent='请先选择 Wi-Fi';return;}save.disabled=true;msg.textContent='正在连接，最多等待 15 秒…';try{const r=await fetch('/wifi/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({ssid:ssid.value,password:password.value})}),x=await r.json();msg.textContent=x.message||'完成';}catch(e){msg.textContent='连接失败，请检查密码后重试';}save.disabled=false;};status();</script></html>)HTML";

const char GIF_PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><meta name="viewport" content="width=device-width,initial-scale=1"><title>GIF 上传</title><style>body{margin:0;background:#101114;color:#f4f4f5;font:16px system-ui;text-align:center}main{max-width:520px;margin:auto;padding:28px 18px}input,button{box-sizing:border-box;width:100%;font:inherit;margin:10px 0;padding:12px;border-radius:9px;border:0}button{background:#e04444;color:white;font-weight:700}button:disabled{opacity:.45}canvas{width:min(72vw,320px);height:min(72vw,320px);border-radius:50%;background:#000}progress{width:100%;height:18px;margin:10px 0;accent-color:#e04444}small{display:block;color:#b8bbc5;line-height:1.6}a{color:#9db6ff}</style><main><h2>上传 GIF 动画</h2><small>直接上传原始 GIF，由 ESP32 从 SD 卡实时解码播放。上传期间请保持页面亮屏并留在前台。</small><input id="file" type="file" accept="image/gif,.gif"><canvas id="c" width="480" height="480"></canvas><button id="send" disabled>直接上传 GIF</button><progress id="progress" value="0" max="100"></progress><small id="msg">请选择 GIF 文件</small><p><a href="/">返回图片上传</a></p></main><script>
const file=document.querySelector('#file'),send=document.querySelector('#send'),msg=document.querySelector('#msg'),progress=document.querySelector('#progress'),c=document.querySelector('#c'),ctx=c.getContext('2d',{willReadFrequently:true}),sc=document.createElement('canvas'),sx=sc.getContext('2d');let chosen=null;
file.onchange=()=>{chosen=file.files[0];send.disabled=!chosen;msg.textContent=chosen?'已选择 '+chosen.name:'请选择 GIF 文件'};
function rgb565(){const p=ctx.getImageData(0,0,480,480).data,r=new Uint8Array(480*480*2);for(let i=0,j=0;i<p.length;i+=4){const v=((p[i]&248)<<8)|((p[i+1]&252)<<3)|(p[i+2]>>3);r[j++]=v&255;r[j++]=v>>8}return r}
async function post(url,body){const r=await fetch(url,{method:'POST',body});if(!r.ok)throw new Error(await r.text());return r}
function lzw(min,data,want){const clear=1<<min,end=clear+1,prefix=new Int16Array(4096),suffix=new Uint8Array(4096),stack=new Uint8Array(4097),out=new Uint8Array(want);for(let i=0;i<clear;i++)suffix[i]=i;let size=min+1,next=end+1,datum=0,bits=0,pos=0,old=-1,first=0,n=0;const read=()=>{while(bits<size){if(pos>=data.length)return-1;datum|=data[pos++]<<bits;bits+=8}const v=datum&((1<<size)-1);datum>>=size;bits-=size;return v};while(n<want){let code=read();if(code<0||code===end)break;if(code===clear){size=min+1;next=end+1;old=-1;continue}if(old<0){if(code>=clear)throw Error('GIF LZW 数据损坏');out[n++]=suffix[code];first=suffix[code];old=code;continue}const input=code;let top=0;if(code>=next){stack[top++]=first;code=old}while(code>=clear){if(code>=4096)throw Error('GIF LZW 代码越界');stack[top++]=suffix[code];code=prefix[code]}first=suffix[code];stack[top++]=first;while(top&&n<want)out[n++]=stack[--top];if(next<4096){prefix[next]=old;suffix[next]=first;next++;if(next===(1<<size)&&size<12)size++}old=input}return out}
function outputFrame(rgba,w,h){sc.width=w;sc.height=h;sx.putImageData(new ImageData(rgba,w,h),0,0);const s=Math.max(480/w,480/h),dw=w*s,dh=h*s;ctx.fillStyle='#000';ctx.fillRect(0,0,480,480);ctx.save();ctx.beginPath();ctx.arc(240,240,240,0,Math.PI*2);ctx.clip();ctx.translate(240,240);ctx.rotate(Math.PI/2);ctx.drawImage(sc,-dw/2,-dh/2,dw,dh);ctx.restore();return rgb565()}
function countGIF(buffer){const b=new Uint8Array(buffer);let p=0;const byte=()=>{if(p>=b.length)throw Error('GIF 文件不完整');return b[p++]},skip=n=>{p+=n;if(p>b.length)throw Error('GIF 文件不完整')},sub=()=>{let n;while((n=byte()))skip(n)};if(String.fromCharCode(...b.slice(0,6)).slice(0,3)!=='GIF')throw Error('不是有效 GIF');p=6;skip(4);const packed=byte();skip(2);if(packed&128)skip(3*(1<<((packed&7)+1)));let count=0;while(p<b.length){const tag=byte();if(tag===59)break;if(tag===33){byte();sub()}else if(tag===44){skip(8);const ip=byte();if(ip&128)skip(3*(1<<((ip&7)+1)));byte();sub();if(++count>8000)throw Error('GIF 超过 8000 帧')}else if(tag!==0)throw Error('GIF 数据块错误')}if(!count)throw Error('GIF 中没有图像帧');return count}
async function decodeGIF(buffer,total){const b=new Uint8Array(buffer);let p=0;const byte=()=>{if(p>=b.length)throw Error('GIF 文件不完整');return b[p++]},word=()=>byte()|(byte()<<8),skip=n=>{p+=n;if(p>b.length)throw Error('GIF 文件不完整')},table=n=>{const t=b.slice(p,p+n*3);skip(n*3);return t},blocks=()=>{const parts=[];let total=0,n;while((n=byte())){parts.push(b.slice(p,p+n));skip(n);total+=n}const all=new Uint8Array(total);let q=0;for(const x of parts){all.set(x,q);q+=x.length}return all};if(String.fromCharCode(...b.slice(0,6)).slice(0,3)!=='GIF')throw Error('不是有效 GIF');p=6;const W=word(),H=word();if(!W||!H||W*H>4194304)throw Error('GIF 尺寸过大');const packed=byte(),bg=byte();byte();const global=packed&128?table(1<<((packed&7)+1)):null,screen=new Uint8ClampedArray(W*H*4);if(global&&bg*3+2<global.length){const r=global[bg*3],g=global[bg*3+1],bl=global[bg*3+2];for(let i=0;i<screen.length;i+=4){screen[i]=r;screen[i+1]=g;screen[i+2]=bl;screen[i+3]=255}}let delay=100,disposal=0,transparent=-1,lastRect=null,lastDisposal=0,lastRestore=null,done=0;while(p<b.length&&done<total){const tag=byte();if(tag===59)break;if(tag===33){const label=byte();if(label===249){if(byte()!==4)throw Error('GIF 控制块错误');const f=byte();delay=word()*10||100;transparent=(f&1)?byte():-1;disposal=(f>>2)&7;byte()}else blocks();continue}if(tag===0)continue;if(tag!==44)throw Error('不支持的 GIF 数据块');if(lastRect&&lastDisposal===2){for(let y=lastRect.t;y<lastRect.t+lastRect.h&&y<H;y++)for(let x=lastRect.l;x<lastRect.l+lastRect.w&&x<W;x++){const o=(y*W+x)*4;screen[o]=screen[o+1]=screen[o+2]=screen[o+3]=0}}else if(lastRect&&lastDisposal===3&&lastRestore)screen.set(lastRestore);const l=word(),t=word(),w=word(),h=word(),ip=byte(),colors=ip&128?table(1<<((ip&7)+1)):global;if(!colors)throw Error('GIF 缺少调色板');const restore=disposal===3?screen.slice():null,min=byte(),decoded=lzw(min,blocks(),w*h);let indices=decoded;if(ip&64){indices=new Uint8Array(w*h);let from=0;for(const pass of [[0,8],[4,8],[2,4],[1,2]])for(let y=pass[0];y<h;y+=pass[1]){indices.set(decoded.subarray(from,from+w),y*w);from+=w}}for(let y=0;y<h&&t+y<H;y++)for(let x=0;x<w&&l+x<W;x++){const ci=indices[y*w+x];if(ci===transparent)continue;const co=ci*3,o=((t+y)*W+l+x)*4;if(co+2<colors.length){screen[o]=colors[co];screen[o+1]=colors[co+1];screen[o+2]=colors[co+2];screen[o+3]=255}}const raw=outputFrame(screen,W,H),frameDelay=Math.max(80,Math.min(2000,delay)),f=new FormData;f.append('frame',new Blob([raw],{type:'application/octet-stream'}),'frame.rgb');await post('/gif/frame?delay='+frameDelay,f);done++;msg.textContent='正在转换并上传 '+done+' / '+total+' 帧';lastRect={l,t,w,h};lastDisposal=disposal;lastRestore=restore;delay=100;disposal=0;transparent=-1;await new Promise(r=>setTimeout(r,0))}if(done!==total)throw Error('GIF 帧数不完整');return done}
function uploadGIF(data){return new Promise((resolve,reject)=>{const x=new XMLHttpRequest;x.open('POST','/gif/upload');x.upload.onprogress=e=>{if(!e.lengthComputable)return;const percent=Math.round(e.loaded/e.total*100);progress.value=percent;msg.textContent='正在上传 '+percent+'%  '+(e.loaded/1048576).toFixed(1)+' / '+(e.total/1048576).toFixed(1)+' MB'};x.onload=()=>x.status>=200&&x.status<300?resolve(x.responseText):reject(Error(x.responseText||'服务器写入失败'));x.onerror=()=>reject(Error('网络连接中断'));x.send(data)})}
send.onclick=async()=>{if(!chosen)return;send.disabled=true;progress.value=0;try{const f=new FormData;f.append('gif',chosen,chosen.name);await uploadGIF(f);progress.value=100;msg.textContent='GIF 上传完成；退出设置后开始播放。'}catch(e){msg.textContent='失败：'+e.message}send.disabled=false};
</script></html>)HTML";

const char MEDIA_PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><meta name="viewport" content="width=device-width,initial-scale=1"><title>媒体管理</title><style>body{margin:0;background:#101114;color:#f4f4f5;font:16px system-ui}main{max-width:520px;margin:auto;padding:28px 18px}section{display:flex;align-items:center;justify-content:space-between;background:#202228;border-radius:10px;padding:12px 14px;margin:10px 0}button{font:inherit;border:0;border-radius:8px;padding:9px 14px;background:#cf3f45;color:white;font-weight:700}small{color:#b8bbc5}a{color:#9db6ff}</style><main><h2>媒体管理</h2><small>删除后无法恢复，请确认所选项目。</small><div id="list">正在读取…</div><p><a href="/">返回图片上传</a></p></main><script>
const list=document.querySelector('#list');async function load(){try{const x=await(await fetch('/media/status')).json();list.replaceChildren();for(let i=0;i<x.images;i++)add('图片 '+(i+1),'image',i);for(let i=0;i<x.gifs;i++)add('GIF 动画 '+(i+1),'gif',i);if(!x.images&&!x.gifs)list.textContent='暂无媒体';}catch(e){list.textContent='读取失败'}}function add(name,type,index){const row=document.createElement('section'),label=document.createElement('span'),button=document.createElement('button');label.textContent=name;button.textContent='删除';button.onclick=async()=>{if(!confirm('确定删除“'+name+'”吗？'))return;button.disabled=true;const r=await fetch('/media/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({type,index})});if(!r.ok)alert(await r.text());await load()};row.append(label,button);list.append(row)}load();
</script></html>)HTML";

String imagePath(uint8_t index) { return "/photos/pic" + String(index) + ".rgb"; }
String gifPath(uint8_t index) { return "/gifs/gif" + String(index) + ".gif"; }

void clearScreen() { static uint16_t black[SCREEN_SIZE * 8] = {0}; for (uint16_t y = 0; y < SCREEN_SIZE; y += 8) LCD_addWindow(0, y, 479, y + 7, (uint8_t *)black); }
float angleDifference(float a, float b) { float d = a - b; while (d > 180.0f) d -= 360.0f; while (d < -180.0f) d += 360.0f; return d; }
bool IRAM_ATTR onDisplayVsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *userData) { BaseType_t taskWoken = pdFALSE; xSemaphoreGiveFromISR((SemaphoreHandle_t)userData, &taskWoken); return taskWoken == pdTRUE; }
bool initLevelDisplay() { void *fb0 = nullptr, *fb1 = nullptr; if (esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &fb0, &fb1) != ESP_OK) return false; frameBuffers[0] = (uint16_t *)fb0; frameBuffers[1] = (uint16_t *)fb1; displayVsync = xSemaphoreCreateBinary(); if (!frameBuffers[0] || !frameBuffers[1] || !displayVsync) return false; const esp_lcd_rgb_panel_event_callbacks_t callbacks = {.on_vsync = onDisplayVsync}; return esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &callbacks, displayVsync) == ESP_OK; }
bool prepareLevelSource(const uint8_t *rawImage) {
  if (!rawImage) return false;
  levelSource = (const uint16_t *)rawImage;
  return true;
}
void drawBrakeRing(uint16_t *buffer) {
  if (!brakeLightEnabled || !brakeLightActive) return;
  constexpr uint16_t red = 0xF800;
  constexpr int32_t CENTER = SCREEN_SIZE / 2;
  constexpr int32_t OUTER_RADIUS2 = 239 * 239;
  // The outer radius is already limited by the 480 px screen; widen the ring inward.
  constexpr int32_t INNER_RADIUS2 = 199 * 199;
  for (int32_t y = 1; y < SCREEN_SIZE - 1; ++y) {
    const int32_t dy = y - CENTER;
    const int32_t dy2 = dy * dy;
    if (dy2 > OUTER_RADIUS2) continue;
    const int32_t outerDx = (int32_t)sqrtf(OUTER_RADIUS2 - dy2);
    const int32_t innerDx = dy2 < INNER_RADIUS2 ? (int32_t)sqrtf(INNER_RADIUS2 - dy2) : 0;
    for (int32_t x = CENTER - outerDx; x <= CENTER - innerDx; ++x) buffer[y * SCREEN_SIZE + x] = red;
    for (int32_t x = CENTER + innerDx; x <= CENTER + outerDx; ++x) buffer[y * SCREEN_SIZE + x] = red;
  }
}
bool renderLevelFrame(bool force = false) {
  if (!levelSource || !frameBuffers[0] || !frameBuffers[1]) return false;
  const uint32_t now = millis();
  if (!force && (now - lastLevelRender < levelUpdateMs || fabsf(angleDifference(levelAngle, renderedAngle)) < levelDeadBandDeg)) return false;
  const uint8_t backFrame = visibleFrame ^ 1;
  uint16_t *dst = frameBuffers[backFrame];
  const uint16_t *src = levelSource;
  const float outputAngle = levelLockEnabled ? levelAngle : 0.0f;
  const float outputRadians = (LEVEL_ROTATION_SIGN * outputAngle + LEVEL_ANGLE_OFFSET_DEG) * DEG_TO_RAD;
  const int32_t cosQ16 = (int32_t)lroundf(cosf(outputRadians) * 65536.0f);
  const int32_t sinQ16 = (int32_t)lroundf(sinf(outputRadians) * 65536.0f);
  constexpr int32_t CENTER = SCREEN_SIZE / 2;
  constexpr int32_t RADIUS2 = 239 * 239;

  // Work in small tiles. For each output tile, copy its rotated source
  // bounding box from PSRAM into internal SRAM using sequential row reads.
  // Pixel sampling then stays entirely in SRAM instead of randomly hitting PSRAM.
  for (int32_t tileY = 0; tileY < SCREEN_SIZE; tileY += ROTATION_TILE_SIZE) {
    const int32_t tileYEnd = min(tileY + ROTATION_TILE_SIZE, (int32_t)SCREEN_SIZE);
    for (int32_t tileX = 0; tileX < SCREEN_SIZE; tileX += ROTATION_TILE_SIZE) {
      const int32_t tileXEnd = min(tileX + ROTATION_TILE_SIZE, (int32_t)SCREEN_SIZE);
      int32_t minSxQ = INT32_MAX, maxSxQ = INT32_MIN, minSyQ = INT32_MAX, maxSyQ = INT32_MIN;
      const int32_t cornerX[4] = {tileX, tileXEnd - 1, tileX, tileXEnd - 1};
      const int32_t cornerY[4] = {tileY, tileY, tileYEnd - 1, tileYEnd - 1};
      for (uint8_t corner = 0; corner < 4; ++corner) {
        const int32_t dx = cornerX[corner] - CENTER, dy = cornerY[corner] - CENTER;
        const int32_t sxQ = (CENTER << 16) + cosQ16 * dx + sinQ16 * dy;
        const int32_t syQ = (CENTER << 16) - sinQ16 * dx + cosQ16 * dy;
        minSxQ = min(minSxQ, sxQ); maxSxQ = max(maxSxQ, sxQ);
        minSyQ = min(minSyQ, syQ); maxSyQ = max(maxSyQ, syQ);
      }
      int32_t cacheX0 = max((int32_t)0, (minSxQ >> 16) - 1);
      int32_t cacheY0 = max((int32_t)0, (minSyQ >> 16) - 1);
      int32_t cacheX1 = min((int32_t)SCREEN_SIZE, ((maxSxQ + 65535) >> 16) + 2);
      int32_t cacheY1 = min((int32_t)SCREEN_SIZE, ((maxSyQ + 65535) >> 16) + 2);
      const int32_t cacheWidth = cacheX1 - cacheX0, cacheHeight = cacheY1 - cacheY0;
      const bool cacheValid = cacheWidth > 0 && cacheHeight > 0 && cacheWidth <= ROTATION_CACHE_SIZE && cacheHeight <= ROTATION_CACHE_SIZE;
      if (cacheValid) {
        for (int32_t row = 0; row < cacheHeight; ++row)
          memcpy(rotationTileCache + row * cacheWidth, src + (cacheY0 + row) * SCREEN_SIZE + cacheX0, cacheWidth * sizeof(uint16_t));
      }

      for (int32_t y = tileY; y < tileYEnd; ++y) {
        const int32_t dy = y - CENTER;
        int32_t sxQ16 = (CENTER << 16) + cosQ16 * (tileX - CENTER) + sinQ16 * dy;
        int32_t syQ16 = (CENTER << 16) - sinQ16 * (tileX - CENTER) + cosQ16 * dy;
        for (int32_t x = tileX; x < tileXEnd; ++x) {
          const int32_t dx = x - CENTER;
          const int32_t sx = sxQ16 >> 16, sy = syQ16 >> 16;
          rotationOutputTile[(y - tileY) * ROTATION_TILE_SIZE + (x - tileX)] = (cacheValid && dx * dx + dy * dy <= RADIUS2 && sx >= cacheX0 && sx < cacheX1 && sy >= cacheY0 && sy < cacheY1) ? rotationTileCache[(sy - cacheY0) * cacheWidth + (sx - cacheX0)] : 0;
          sxQ16 += cosQ16;
          syQ16 -= sinQ16;
        }
      }
      const int32_t outputWidth = tileXEnd - tileX;
      for (int32_t row = 0; row < tileYEnd - tileY; ++row)
        memcpy(dst + (tileY + row) * SCREEN_SIZE + tileX, rotationOutputTile + row * ROTATION_TILE_SIZE, outputWidth * sizeof(uint16_t));
    }
    taskYIELD();
  }
  drawBrakeRing(dst);

  // Because dst is one of the driver's own full-screen buffers, this selects
  // the completed frame instead of copying it into the buffer being scanned.
  if (displayVsync) {
    xSemaphoreTake(displayVsync, 0); // Discard a stale edge.
    xSemaphoreTake(displayVsync, pdMS_TO_TICKS(40));
  }
  esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, SCREEN_SIZE, SCREEN_SIZE, dst);
  visibleFrame = backFrame;
  renderedAngle = levelAngle;
  lastLevelRender = now;
  return true;
}

const uint8_t SETTINGS_FONT[26][7] = {
  {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
  {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
  {14,17,16,23,17,17,14},{17,17,17,31,17,17,17},{31,4,4,4,4,4,31},
  {7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
  {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
  {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
  {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
  {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
  {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}
};
const uint8_t SETTINGS_DIGITS[10][7] = {
  {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{30,1,1,14,1,1,30},{2,6,10,18,31,2,2},
  {31,16,16,30,1,1,30},{14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,1,14}
};
const uint8_t SETTINGS_DASH[7] = {0,0,0,31,0,0,0};
const uint8_t SETTINGS_UNDERSCORE[7] = {0,0,0,0,0,0,31};
const uint8_t SETTINGS_DOT[7] = {0,0,0,0,0,12,12};

void fillBufferRect(uint16_t *buffer, int x, int y, int width, int height, uint16_t color) {
  const int xEnd = min(x + width, (int)SCREEN_SIZE);
  const int yEnd = min(y + height, (int)SCREEN_SIZE);
  x = max(x, 0); y = max(y, 0);
  for (int row = y; row < yEnd; ++row)
    for (int column = x; column < xEnd; ++column) buffer[row * SCREEN_SIZE + column] = color;
}

void drawSettingsText(uint16_t *buffer, const char *text, int x, int y, uint8_t scale, uint16_t color) {
  for (const char *p = text; *p; ++p) {
    const uint8_t *character = nullptr;
    if (*p >= 'A' && *p <= 'Z') character = SETTINGS_FONT[*p - 'A'];
    else if (*p >= '0' && *p <= '9') character = SETTINGS_DIGITS[*p - '0'];
    else if (*p == '-') character = SETTINGS_DASH;
    else if (*p == '_') character = SETTINGS_UNDERSCORE;
    else if (*p == '.') character = SETTINGS_DOT;
    if (character) {
      for (uint8_t row = 0; row < 7; ++row)
        for (uint8_t column = 0; column < 5; ++column)
          if (character[row] & (1 << (4 - column)))
            fillBufferRect(buffer, x + column * scale, y + row * scale, scale, scale, color);
    }
    x += 6 * scale;
  }
}

void drawCenteredSettingsText(uint16_t *buffer, const char *text, int y, uint8_t scale, uint16_t color) {
  const int width = strlen(text) * 6 * scale;
  drawSettingsText(buffer, text, (SCREEN_SIZE - width) / 2, y, scale, color);
}

String wifiStatusLabel() {
  if (WiFi.status() != WL_CONNECTED) return "WIFI NOT CONNECTED";
  String ssid = WiFi.SSID();
  ssid.toUpperCase();
  String label = "WIFI ";
  for (size_t i = 0; i < ssid.length() && label.length() < 25; ++i) {
    const char c = ssid[i];
    label += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : ' ';
  }
  return label;
}

String wifiAddressLabel() {
  return WiFi.status() == WL_CONNECTED ? "IP " + WiFi.localIP().toString() : "";
}

void drawSettingsItem(uint16_t *buffer, uint8_t item, int y, const char *label) {
  const bool selected = settingsFocus == item;
  const bool wifiActive = item == SETTINGS_WIFI && wifiUploadActive;
  const bool switchItem = item == SETTINGS_BRAKE_LIGHT || item == SETTINGS_AUTO_BRAKE;
  const bool switchOn = (item == SETTINGS_BRAKE_LIGHT && brakeLightEnabled) || (item == SETTINGS_AUTO_BRAKE && autoBrakeLightEnabled);
  const bool wifiFlashOn = (millis() / 400) & 1;
  const uint16_t border = wifiActive ? (wifiFlashOn ? 0x07E0 : 0x0180) : (selected ? 0x07FF : (switchItem ? (switchOn ? 0x07E0 : 0x7800) : 0x39E7));
  const uint16_t background = selected ? 0x1082 : 0x0000;
  fillBufferRect(buffer, 44, y, 392, 40, background);
  fillBufferRect(buffer, 44, y, 392, 3, border);
  fillBufferRect(buffer, 44, y + 37, 392, 3, border);
  fillBufferRect(buffer, 44, y, 3, 40, border);
  fillBufferRect(buffer, 433, y, 3, 40, border);
  const uint16_t textColor = selected ? 0xFFFF : (wifiActive || switchOn ? 0x07E0 : (switchItem ? 0xFBE0 : 0xBDF7));
  drawCenteredSettingsText(buffer, label, y + 9, 3, textColor);
}

#if 0 // Performance management UI is temporarily disabled.
const char *powerModeName() {
  switch (powerMode) {
    case MODE_PERFORMANCE: return "PERFORMANCE";
    case MODE_BALANCED: return "BALANCED";
    case MODE_POWER_SAVE: return "POWER SAVE";
  }
  return "";
}
#endif

void drawSettingsPage() {
  if (!frameBuffers[0] || !frameBuffers[1]) return;
  const uint8_t backFrame = visibleFrame ^ 1;
  uint16_t *dst = frameBuffers[backFrame];
  memset(dst, 0, IMAGE_BYTES);
  drawCenteredSettingsText(dst, "SETTINGS", 20, 4, 0xFFFF);
  drawCenteredSettingsText(dst, "BRIGHTNESS", 58, 2, 0xBDF7);
  fillBufferRect(dst, 70, 82, 340, 18, 0x2104);
  fillBufferRect(dst, 73, 85, screenBrightness * 334 / 100, 12, 0xFFE0);
  if (brightnessAdjustActive) {
    drawCenteredSettingsText(dst, "TILT LEFT DIM", 160, 3, 0xBDF7);
    drawCenteredSettingsText(dst, "RIGHT BRIGHT", 200, 3, 0xBDF7);
    drawCenteredSettingsText(dst, "SHORT OR HOLD", 268, 2, 0xFFE0);
    drawCenteredSettingsText(dst, "TO SAVE", 294, 2, 0xFFE0);
  } else {
    drawSettingsItem(dst, SETTINGS_BRIGHTNESS, 112, "BRIGHTNESS");
    drawSettingsItem(dst, SETTINGS_WIFI, 158, "WIFI UPLOAD");
    drawSettingsItem(dst, SETTINGS_BRAKE_LIGHT, 204, brakeLightEnabled ? "BRAKE LIGHT ON" : "BRAKE LIGHT OFF");
    drawSettingsItem(dst, SETTINGS_AUTO_BRAKE, 250, autoBrakeLightEnabled ? "AUTO BRAKE ON" : "AUTO BRAKE OFF");
    if (autoBrakeLightEnabled) drawCenteredSettingsText(dst, autoBrakeStatus.c_str(), 300, 2, solarLocationValid ? 0x07FF : 0xFFE0);
    drawSettingsItem(dst, SETTINGS_BACK, 330, "BACK");
    const String wifiStatus = wifiStatusLabel();
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    drawCenteredSettingsText(dst, wifiStatus.c_str(), wifiConnected ? 400 : 420, 2, wifiConnected ? 0x07E0 : 0xF800);
    if (wifiConnected) {
      const String wifiAddress = wifiAddressLabel();
      drawCenteredSettingsText(dst, wifiAddress.c_str(), 430, 2, 0x07FF);
    }
  }
  if (displayVsync) {
    xSemaphoreTake(displayVsync, 0);
    xSemaphoreTake(displayVsync, pdMS_TO_TICKS(40));
  }
  esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, SCREEN_SIZE, SCREEN_SIZE, dst);
  visibleFrame = backFrame;
}

const uint8_t GLYPH_N[7] = {17,25,21,19,17,17,17};
const uint8_t GLYPH_O[7] = {14,17,17,17,17,17,14};
const uint8_t GLYPH_I[7] = {31,4,4,4,4,4,31};
const uint8_t GLYPH_M[7] = {17,27,21,21,17,17,17};
const uint8_t GLYPH_A[7] = {14,17,17,31,17,17,17};
const uint8_t GLYPH_G[7] = {14,17,16,23,17,17,14};
const uint8_t GLYPH_E[7] = {31,16,16,30,16,16,31};
const uint8_t *glyph(char c) { switch (c) { case 'N': return GLYPH_N; case 'O': return GLYPH_O; case 'I': return GLYPH_I; case 'M': return GLYPH_M; case 'A': return GLYPH_A; case 'G': return GLYPH_G; case 'E': return GLYPH_E; default: return nullptr; } }
void fillRect(uint16_t x, uint16_t y, uint8_t w, uint8_t h, uint16_t color) { static uint16_t pixels[100]; for (uint16_t i = 0; i < w * h; ++i) pixels[i] = color; LCD_addWindow(x, y, x + w - 1, y + h - 1, (uint8_t *)pixels); }
void drawNoImageMessage() { const char text[] = "NO IMAGE"; constexpr uint8_t scale = 8, charWidth = 5 * scale, gap = 2 * scale; const uint16_t startX = (SCREEN_SIZE - (7 * charWidth + 6 * gap)) / 2, startY = 212; uint16_t x = startX; for (const char *p = text; *p; ++p) { const char c = *p; if (c == ' ') { x += gap; continue; } const uint8_t *g = glyph(c); for (uint8_t row = 0; row < 7; ++row) for (uint8_t col = 0; col < 5; ++col) if (g[row] & (1 << (4 - col))) fillRect(x + col * scale, startY + row * scale, scale, scale, 0xFFFF); x += charWidth + gap; } }
void scanImages() { imageCount = 0; if (!sdReady) return; for (uint8_t i = 0; i < MAX_IMAGES; ++i) { File f = SD_MMC.open(imagePath(i), FILE_READ); if (f && f.size() == IMAGE_BYTES) imageCount = i + 1; if (f) f.close(); } }
void *gifOpenFile(const char *path, int32_t *size) { animationFile = SD_MMC.open(path, FILE_READ); if (!animationFile) return nullptr; *size = animationFile.size(); return &animationFile; }
void gifCloseFile(void *handle) { File *file = static_cast<File *>(handle); if (file) file->close(); }
int32_t gifReadFile(GIFFILE *gifFile, uint8_t *buffer, int32_t length) { File *file = static_cast<File *>(gifFile->fHandle); const int32_t remaining = gifFile->iSize - gifFile->iPos; if (length > remaining) length = remaining; if (length <= 0) return 0; const int32_t read = file->read(buffer, length); gifFile->iPos = file->position(); return read; }
int32_t gifSeekFile(GIFFILE *gifFile, int32_t position) { File *file = static_cast<File *>(gifFile->fHandle); if (!file->seek(position)) return -1; gifFile->iPos = file->position(); return gifFile->iPos; }
void stopAnimation() { animationPlaying = false; if (gifDecoderOpen) { gifDecoder.close(); gifDecoderOpen = false; } if (animationFile) animationFile.close(); if (gifFrameBuffer) { heap_caps_free(gifFrameBuffer); gifFrameBuffer = nullptr; } }
void scanAnimation() {
  gifCount = 0;
  if (!sdReady) { animationAvailable = false; return; }
  for (uint8_t i = 0; i < MAX_GIFS; ++i) {
    File file = SD_MMC.open(gifPath(i), FILE_READ);
    char signature[3];
    const bool valid = file && file.size() > 13 && file.read((uint8_t *)signature, 3) == 3 && !memcmp(signature, "GIF", 3);
    if (file) file.close();
    if (!valid) break;
    gifCount = i + 1;
  }
  animationAvailable = gifCount > 0;
}
void migrateLegacyAnimation() {
  if (!sdReady || !SD_MMC.exists("/animation.gif")) return;
  if (!SD_MMC.exists("/gifs")) SD_MMC.mkdir("/gifs");
  const String firstGif = gifPath(0);
  if (!SD_MMC.exists(firstGif)) SD_MMC.rename("/animation.gif", firstGif);
}
bool startAnimation(uint8_t index) {
  stopAnimation();
  if (!animationAvailable || !sdReady || index >= gifCount) return false;
  gifDecoder.begin(LITTLE_ENDIAN_PIXELS);
  const String path = gifPath(index);
  if (!gifDecoder.open(path.c_str(), gifOpenFile, gifCloseFile, gifReadFile, gifSeekFile, nullptr)) return false;
  gifDecoderOpen = true;
  gifCanvasWidth = gifDecoder.getCanvasWidth();
  gifCanvasHeight = gifDecoder.getCanvasHeight();
  const size_t pixels = (size_t)gifCanvasWidth * gifCanvasHeight;
  if (!gifCanvasWidth || !gifCanvasHeight || pixels > 1800000) { stopAnimation(); return false; }
  gifFrameBuffer = (uint8_t *)heap_caps_malloc(pixels * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!gifFrameBuffer) { stopAnimation(); return false; }
  gifDecoder.setDrawType(GIF_DRAW_COOKED);
  gifDecoder.setFrameBuf(gifFrameBuffer);
  animationPlaying = true;
  currentGif = index;
  currentImage = -1;
  nextAnimationFrameAt = 0;
  return true;
}
bool showAnimationFrame() {
  if (!animationPlaying || !frameBuffers[0] || !frameBuffers[1]) return false;
  int delayMs = 100;
  if (!gifDecoder.playFrame(false, &delayMs)) { gifDecoder.reset(); if (!gifDecoder.playFrame(false, &delayMs)) { stopAnimation(); return false; } }
  const uint8_t backFrame = visibleFrame ^ 1;
  uint16_t *dst = frameBuffers[backFrame];
  const size_t sourcePixels = (size_t)gifCanvasWidth * gifCanvasHeight;
  const uint8_t *source = gifFrameBuffer + sourcePixels;
  const int crop = min(gifCanvasWidth, gifCanvasHeight);
  const int sourceX0 = (gifCanvasWidth - crop) / 2;
  const int sourceY0 = (gifCanvasHeight - crop) / 2;
  int16_t sourceX[SCREEN_SIZE], sourceY[SCREEN_SIZE];
  for (int i = 0; i < SCREEN_SIZE; ++i) { sourceX[i] = sourceX0 + i * crop / SCREEN_SIZE; sourceY[i] = sourceY0 + (SCREEN_SIZE - 1 - i) * crop / SCREEN_SIZE; }
  for (int y = 0; y < SCREEN_SIZE; ++y) {
    uint16_t *row = dst + y * SCREEN_SIZE;
    const int sx = sourceX[y];
    for (int x = 0; x < SCREEN_SIZE; ++x) {
      const size_t offset = ((size_t)sourceY[x] * gifCanvasWidth + sx) * 2;
      row[x] = source[offset] | (uint16_t)source[offset + 1] << 8;
    }
  }
  drawBrakeRing(dst);
  if (displayVsync) { xSemaphoreTake(displayVsync, 0); xSemaphoreTake(displayVsync, pdMS_TO_TICKS(40)); }
  esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, SCREEN_SIZE, SCREEN_SIZE, dst);
  visibleFrame = backFrame;
  nextAnimationFrameAt = millis() + constrain(delayMs, 20, 2000);
  return true;
}
bool showImage(uint8_t index) { stopAnimation(); if (index >= imageCount) return false; if (!sdReady) { if (!ramImages[index] || !prepareLevelSource(ramImages[index])) return false; currentGif = -1; currentImage = index; return renderLevelFrame(true); } File f = SD_MMC.open(imagePath(index), FILE_READ); if (!f || f.size() != IMAGE_BYTES) { if (f) f.close(); return false; } if (!displayBuffer) displayBuffer = (uint8_t *)heap_caps_malloc(IMAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); if (!displayBuffer || f.read(displayBuffer, IMAGE_BYTES) != IMAGE_BYTES) { f.close(); return false; } f.close(); if (!prepareLevelSource(displayBuffer)) return false; currentGif = -1; currentImage = index; return renderLevelFrame(true); }
void nextImage() {
  const int total = imageCount + gifCount;
  if (!total) return;
  const int current = animationPlaying ? imageCount + currentGif : max(0, (int)currentImage);
  const int target = (current + 1) % total;
  if (target < imageCount) showImage(target); else if (startAnimation(target - imageCount)) showAnimationFrame();
}
void previousImage() {
  const int total = imageCount + gifCount;
  if (!total) return;
  const int current = animationPlaying ? imageCount + currentGif : max(0, (int)currentImage);
  const int target = (current + total - 1) % total;
  if (target < imageCount) showImage(target); else if (startAnimation(target - imageCount)) showAnimationFrame();
}
String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '\\' || c == '"') { escaped += '\\'; escaped += c; }
    else if (c == '\n') escaped += "\\n";
    else if (c == '\r') escaped += "\\r";
    else escaped += c;
  }
  return escaped;
}

void loadWifiCredentials() {
  preferences.begin("circleframe", true);
  savedWifiSsid = preferences.getString("ssid", "");
  savedWifiPassword = preferences.getString("pass", "");
  autoBrakeLightEnabled = preferences.getBool("autoBrake", false);
  preferences.end();
}

void saveAutoBrakeSetting() {
  preferences.begin("circleframe", false);
  preferences.putBool("autoBrake", autoBrakeLightEnabled);
  preferences.end();
}

void saveWifiCredentials(const String &ssid, const String &password) {
  preferences.begin("circleframe", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", password);
  preferences.end();
  savedWifiSsid = ssid;
  savedWifiPassword = password;
}

bool jsonNumber(const String &json, const char *key, double &value) {
  const String token = "\"" + String(key) + "\":";
  const int start = json.indexOf(token);
  if (start < 0) return false;
  char *end = nullptr;
  value = strtod(json.c_str() + start + token.length(), &end);
  return end != json.c_str() + start + token.length();
}

double normalizeDegrees(double value) {
  value = fmod(value, 360.0);
  return value < 0.0 ? value + 360.0 : value;
}

double normalizeHours(double value) {
  value = fmod(value, 24.0);
  return value < 0.0 ? value + 24.0 : value;
}

bool calculateSunHour(int dayOfYear, bool sunrise, float &localHour) {
  const double longitudeHour = solarLongitude / 15.0;
  const double approximateTime = dayOfYear + ((sunrise ? 6.0 : 18.0) - longitudeHour) / 24.0;
  const double meanAnomaly = 0.9856 * approximateTime - 3.289;
  double trueLongitude = meanAnomaly + 1.916 * sin(meanAnomaly * DEG_TO_RAD) + 0.020 * sin(2.0 * meanAnomaly * DEG_TO_RAD) + 282.634;
  trueLongitude = normalizeDegrees(trueLongitude);
  double rightAscension = normalizeDegrees(atan(0.91764 * tan(trueLongitude * DEG_TO_RAD)) * RAD_TO_DEG);
  rightAscension += floor(trueLongitude / 90.0) * 90.0 - floor(rightAscension / 90.0) * 90.0;
  rightAscension /= 15.0;
  const double sinDeclination = 0.39782 * sin(trueLongitude * DEG_TO_RAD);
  const double cosDeclination = cos(asin(sinDeclination));
  const double cosHour = (cos(90.833 * DEG_TO_RAD) - sinDeclination * sin(solarLatitude * DEG_TO_RAD)) /
                         (cosDeclination * cos(solarLatitude * DEG_TO_RAD));
  if (cosHour < -1.0 || cosHour > 1.0) return false;
  double hourAngle = acos(cosHour) * RAD_TO_DEG;
  if (sunrise) hourAngle = 360.0 - hourAngle;
  hourAngle /= 15.0;
  const double localMeanTime = hourAngle + rightAscension - 0.06571 * approximateTime - 6.622;
  const double utcHour = normalizeHours(localMeanTime - longitudeHour);
  localHour = normalizeHours(utcHour + solarUtcOffsetSeconds / 3600.0);
  return true;
}

bool refreshSolarLocation() {
  lastSolarLocationAttempt = millis();
  WiFiClient client;
  client.setTimeout(5000);
  if (!client.connect("ip-api.com", 80, 5000)) return false;
  client.print(F("GET /json/?fields=status,lat,lon,offset HTTP/1.1\r\nHost: ip-api.com\r\nConnection: close\r\n\r\n"));
  String response;
  response.reserve(512);
  const uint32_t deadline = millis() + 5000;
  while ((client.connected() || client.available()) && (int32_t)(deadline - millis()) > 0) {
    while (client.available()) response += (char)client.read();
    delay(1);
  }
  client.stop();
  if (!response.startsWith("HTTP/1.1 200") && !response.startsWith("HTTP/1.0 200")) return false;
  const int bodyStart = response.indexOf("\r\n\r\n");
  if (bodyStart < 0) return false;
  const String body = response.substring(bodyStart + 4);
  double latitude, longitude, offset;
  if (body.indexOf("\"status\":\"success\"") < 0 ||
      !jsonNumber(body, "lat", latitude) || !jsonNumber(body, "lon", longitude) || !jsonNumber(body, "offset", offset)) return false;
  solarLatitude = latitude;
  solarLongitude = longitude;
  solarUtcOffsetSeconds = (int32_t)offset;
  solarLocationValid = true;
  solarScheduleDay = -1;
  Serial.printf("Auto brake location: %.4f, %.4f, UTC offset %ld\n", solarLatitude, solarLongitude, (long)solarUtcOffsetSeconds);
  return true;
}

void refreshBrakeVisual() {
  brakeLightActive = false;
  renderedAngle = 1000.0f;
  if (animationPlaying) nextAnimationFrameAt = 0;
  else if (!settingsActive && imageCount) renderLevelFrame(true);
}

void updateAutoBrakeControl(bool forceNetwork) {
  if (!autoBrakeLightEnabled) return;
  if (WiFi.status() != WL_CONNECTED) {
    autoBrakeStatus = "AUTO WAIT WIFI";
    return;
  }
  if (!ntpStarted) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    ntpStarted = true;
  }
  const bool locationExpired = !solarLocationValid || millis() - lastSolarLocationAttempt >= 21600000UL;
  if (locationExpired && (forceNetwork || !lastSolarLocationAttempt || millis() - lastSolarLocationAttempt >= 300000UL)) {
    if (!refreshSolarLocation()) {
      autoBrakeStatus = solarLocationValid ? "AUTO USING OLD LOC" : "AUTO LOCATE FAILED";
      if (!solarLocationValid) {
        if (settingsActive) drawSettingsPage();
        return;
      }
    }
  }
  const time_t now = time(nullptr);
  if (!solarLocationValid || now < 1700000000) {
    autoBrakeStatus = solarLocationValid ? "AUTO WAIT TIME" : "AUTO WAIT LOCATION";
    if (settingsActive) drawSettingsPage();
    return;
  }
  const time_t localEpoch = now + solarUtcOffsetSeconds;
  struct tm localTime;
  gmtime_r(&localEpoch, &localTime);
  if (solarScheduleDay != localTime.tm_yday) {
    if (!calculateSunHour(localTime.tm_yday + 1, true, sunriseLocalHour) || !calculateSunHour(localTime.tm_yday + 1, false, sunsetLocalHour)) {
      autoBrakeStatus = "AUTO SUN DATA ERROR";
      return;
    }
    solarScheduleDay = localTime.tm_yday;
  }
  const float currentHour = localTime.tm_hour + localTime.tm_min / 60.0f + localTime.tm_sec / 3600.0f;
  const bool night = currentHour >= sunsetLocalHour || currentHour < sunriseLocalHour;
  const bool stateChanged = brakeLightEnabled != night;
  brakeLightEnabled = night;
  char status[28];
  const int sunsetHour = (int)sunsetLocalHour;
  const int sunsetMinute = (int)((sunsetLocalHour - sunsetHour) * 60.0f + 0.5f) % 60;
  snprintf(status, sizeof(status), "%s %02d.%02d", night ? "AUTO NIGHT" : "AUTO DAY UNTIL", sunsetHour, sunsetMinute);
  autoBrakeStatus = status;
  if (stateChanged) refreshBrakeVisual();
  if (settingsActive) drawSettingsPage();
}

void handleRoot() {
  String page = FPSTR(PAGE);
  page.replace("选择照片后会自动居中裁切为圆形，再上传到相框。", "可同时选择多张普通图片和 GIF；系统会自动识别格式并依次上传。");
  page.replace("accept=\"image/*\"", "accept=\"image/*,.gif\" multiple");
  page.replace("上传这张图片", "上传所选媒体");
  page.replace("'已保存 '+x.count+' 张。'+x.hint", "'已保存 '+x.count+' 张图片、'+x.gifs+' 个 GIF。'+x.hint");
  page.replace("<small id=\"msg\">", "<progress id=\"mediaProgress\" value=\"0\" max=\"100\" style=\"display:none;width:90%;height:18px\"></progress><small id=\"msg\">");
  page.replace("</html>", R"JS(<script>
const mediaProgress=document.querySelector('#mediaProgress');let batchFiles=[];
async function batchIsGif(f){if(f.type==='image/gif'||f.name.toLowerCase().endsWith('.gif'))return true;const b=new Uint8Array(await f.slice(0,6).arrayBuffer());return b.length===6&&b[0]===71&&b[1]===73&&b[2]===70&&b[3]===56&&(b[4]===55||b[4]===57)&&b[5]===97}
function batchLoadImage(f){return new Promise((resolve,reject)=>{const im=new Image,u=URL.createObjectURL(f);im.onload=()=>{URL.revokeObjectURL(u);resolve(im)};im.onerror=()=>{URL.revokeObjectURL(u);reject(Error('无法读取图片'))};im.src=u})}
async function batchImageBlob(f){const im=await batchLoadImage(f);draw(im);const p=ctx.getImageData(0,0,480,480).data,raw=new Uint8Array(480*480*2);for(let i=0,j=0;i<p.length;i+=4){const v=((p[i]&248)<<8)|((p[i+1]&252)<<3)|(p[i+2]>>3);raw[j++]=v&255;raw[j++]=v>>8}return new Blob([raw],{type:'application/octet-stream'})}
function batchPost(url,field,data,name,index,total){return new Promise((resolve,reject)=>{const form=new FormData,request=new XMLHttpRequest;form.append(field,data,name);request.open('POST',url);request.upload.onprogress=x=>{if(!x.lengthComputable)return;const percent=Math.round((index+x.loaded/x.total)/total*100);mediaProgress.value=percent;msg.textContent='正在上传 '+(index+1)+' / '+total+'：'+name+'（'+Math.round(x.loaded/x.total*100)+'%）'};request.onload=()=>request.status>=200&&request.status<300?resolve():reject(Error(request.responseText||'ESP32 写入失败'));request.onerror=()=>reject(Error('网络连接中断'));request.send(form)})}
file.onchange=async()=>{batchFiles=Array.from(file.files);mediaProgress.style.display='none';mediaProgress.value=0;send.disabled=!batchFiles.length;if(!batchFiles.length){msg.textContent='请选择媒体文件';return}try{if(await batchIsGif(batchFiles[0])){ctx.fillStyle='#000';ctx.fillRect(0,0,480,480)}else draw(await batchLoadImage(batchFiles[0]))}catch(e){}msg.textContent='已选择 '+batchFiles.length+' 个文件，可混合上传图片和 GIF'};
send.onclick=async()=>{if(!batchFiles.length)return;send.disabled=true;file.disabled=true;mediaProgress.style.display='inline-block';mediaProgress.value=0;let success=0,failed=[];for(let i=0;i<batchFiles.length;i++){const chosen=batchFiles[i];try{const gif=await batchIsGif(chosen),data=gif?chosen:await batchImageBlob(chosen);await batchPost(gif?'/gif/upload':'/upload',gif?'gif':'image',data,gif?chosen.name:'circle.rgb',i,batchFiles.length);success++}catch(e){failed.push(chosen.name+'：'+e.message)}mediaProgress.value=Math.round((i+1)/batchFiles.length*100)}send.disabled=false;file.disabled=false;msg.textContent='上传完成：成功 '+success+' 个，失败 '+failed.length+' 个'+(failed.length?'。'+failed.join('；'):'')};
</script></html>)JS");
  page.replace("</main>", "<p><a href=\"/media\" style=\"color:#ffcf70\">管理和删除媒体</a></p><p><a href=\"/wifi-setup\" style=\"color:#9db6ff\">Wi-Fi 配网</a></p></main>");
  server.send(200, "text/html; charset=utf-8", page);
}
void handleStatus() { String hint = sdReady ? "媒体保存至 SD 卡；短按下一项，双击上一项" : "图片暂存内存且不支持 GIF，重启会清空；短按下一项，双击上一项"; server.send(200, "application/json", "{\"count\":" + String(imageCount) + ",\"gifs\":" + String(gifCount) + ",\"hint\":\"" + hint + "\"}"); }
void handleWifiSetupPage() { server.send_P(200, "text/html; charset=utf-8", WIFI_SETUP_PAGE); }
void handleWifiStatus() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  server.send(200, "application/json", "{\"saved\":" + String(savedWifiSsid.length() ? "true" : "false") + ",\"connected\":" + String(connected ? "true" : "false") + ",\"ssid\":\"" + jsonEscape(connected ? WiFi.SSID() : savedWifiSsid) + "\"}");
}
void handleWifiScan() {
  const int found = WiFi.scanNetworks(false, true);
  String json = "{\"networks\":[";
  for (int i = 0; i < found; ++i) {
    if (i) json += ',';
    json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + '}';
  }
  json += "]}";
  WiFi.scanDelete();
  server.send(200, "application/json", json);
}
void handleWifiConnect() {
  if (!server.hasArg("ssid") || !server.arg("ssid").length()) { server.send(400, "application/json", "{\"message\":\"请选择 Wi-Fi\"}"); return; }
  const String ssid = server.arg("ssid");
  const String password = server.arg("password");
  WiFi.mode(WIFI_AP_STA); // Keep the configuration hotspot available while connecting.
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(ssid.c_str(), password.c_str());
  const uint32_t deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) delay(200);
  stationConnected = WiFi.status() == WL_CONNECTED;
  if (!stationConnected) { server.send(400, "application/json", "{\"message\":\"连接失败，请检查账号密码后重试\"}"); return; }
  saveWifiCredentials(ssid, password);
  Serial.printf("Wi-Fi saved and connected: %s\n", WiFi.localIP().toString().c_str());
  server.send(200, "application/json", "{\"message\":\"已保存并连接成功。下次进入 WiFi 上传会自动连接。\"}");
}
void handleUploadDone() { server.send(uploadOk ? 200 : 400, "text/plain; charset=utf-8", uploadOk ? "上传成功，已显示新图片。" : "上传失败：请重新选择一张图片。"); }
void handleUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadOk = false; uploadBytes = 0;
    if (imageCount >= MAX_IMAGES) return;
    if (sdReady) { if (!SD_MMC.exists("/photos")) SD_MMC.mkdir("/photos"); uploadPath = imagePath(imageCount); uploadFile = SD_MMC.open(uploadPath, FILE_WRITE); }
    else uploadBuffer = (uint8_t *)heap_caps_malloc(IMAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (sdReady) { if (uploadFile && uploadFile.write(up.buf, up.currentSize) != up.currentSize) uploadFile.close(); }
    else if (uploadBuffer && uploadBytes + up.currentSize <= IMAGE_BYTES) { memcpy(uploadBuffer + uploadBytes, up.buf, up.currentSize); uploadBytes += up.currentSize; }
  } else if (up.status == UPLOAD_FILE_END) {
    if (sdReady) { if (uploadFile) uploadFile.close(); File check = SD_MMC.open(uploadPath, FILE_READ); uploadOk = check && check.size() == IMAGE_BYTES; if (check) check.close(); if (!uploadOk) SD_MMC.remove(uploadPath); }
    else { uploadOk = uploadBuffer && uploadBytes == IMAGE_BYTES; if (uploadOk) { ramImages[imageCount] = uploadBuffer; uploadBuffer = nullptr; } }
    if (uploadOk) { stopAnimation(); ++imageCount; currentGif = -1; currentImage = imageCount - 1; if (settingsActive) drawSettingsPage(); else showImage(currentImage); }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close(); if (sdReady) SD_MMC.remove(uploadPath); if (uploadBuffer) { heap_caps_free(uploadBuffer); uploadBuffer = nullptr; }
  }
}
void handleGifPage() { server.sendHeader("Cache-Control", "no-store"); server.send_P(200, "text/html; charset=utf-8", GIF_PAGE); }
void handleGifUploadDone() {
  if (!gifUploadOk) { SD_MMC.remove(GIF_TEMP_PATH); server.send(400, "text/plain; charset=utf-8", "GIF 写入或校验失败"); return; }
  if (gifCount >= MAX_GIFS) { SD_MMC.remove(GIF_TEMP_PATH); server.send(400, "text/plain; charset=utf-8", "GIF 数量已满"); return; }
  if (!SD_MMC.exists("/gifs")) SD_MMC.mkdir("/gifs");
  const uint8_t newIndex = gifCount;
  const String destination = gifPath(newIndex);
  SD_MMC.remove(destination);
  if (!SD_MMC.rename(GIF_TEMP_PATH, destination)) { server.send(500, "text/plain; charset=utf-8", "保存 GIF 失败"); return; }
  SD_MMC.remove("/animation.rgf"); // Remove the obsolete expanded-frame animation after replacement succeeds.
  ++gifCount;
  animationAvailable = true;
  if (!startAnimation(newIndex)) { --gifCount; animationAvailable = gifCount > 0; SD_MMC.remove(destination); server.send(400, "text/plain; charset=utf-8", "GIF 无法解码或尺寸过大"); return; }
  server.send(200, "text/plain; charset=utf-8", "GIF 上传完成");
}
void handleGifUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    stopAnimation();
    gifUploadBytes = 0;
    gifUploadOk = sdReady && gifCount < MAX_GIFS;
    SD_MMC.remove(GIF_TEMP_PATH);
    if (gifUploadOk) { uploadFile = SD_MMC.open(GIF_TEMP_PATH, FILE_WRITE); gifUploadOk = (bool)uploadFile; }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (gifUploadOk && uploadFile.write(up.buf, up.currentSize) == up.currentSize) gifUploadBytes += up.currentSize;
    else gifUploadOk = false;
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    File check = SD_MMC.open(GIF_TEMP_PATH, FILE_READ);
    char signature[3];
    gifUploadOk = gifUploadOk && gifUploadBytes > 13 && check && check.read((uint8_t *)signature, 3) == 3 && !memcmp(signature, "GIF", 3);
    if (check) check.close();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
    gifUploadOk = false;
  }
}
void handleMediaPage() { server.sendHeader("Cache-Control", "no-store"); server.send_P(200, "text/html; charset=utf-8", MEDIA_PAGE); }
void handleMediaStatus() { server.send(200, "application/json", "{\"images\":" + String(imageCount) + ",\"gifs\":" + String(gifCount) + "}"); }
void handleMediaDelete() {
  const String type = server.arg("type");
  if (type == "gif") {
    const int index = server.arg("index").toInt();
    if (index < 0 || index >= gifCount) { server.send(404, "text/plain; charset=utf-8", "GIF 不存在"); return; }
    const bool wasGifSelected = currentImage < 0;
    stopAnimation();
    bool ok = sdReady && SD_MMC.remove(gifPath(index));
    for (int i = index + 1; ok && i < gifCount; ++i) ok = SD_MMC.rename(gifPath(i), gifPath(i - 1));
    if (!ok) { scanAnimation(); server.send(500, "text/plain; charset=utf-8", "删除或重新编号失败"); return; }
    --gifCount;
    animationAvailable = gifCount > 0;
    if (currentGif == index) currentGif = gifCount ? min(index, (int)gifCount - 1) : -1;
    else if (currentGif > index) --currentGif;
    if (wasGifSelected) currentImage = -1;
    gifCanvasWidth = 0;
    gifCanvasHeight = 0;
    server.send(200, "text/plain; charset=utf-8", "GIF 已删除");
    return;
  }
  if (type != "image") { server.send(400, "text/plain; charset=utf-8", "媒体类型无效"); return; }
  const int index = server.arg("index").toInt();
  if (index < 0 || index >= imageCount) { server.send(404, "text/plain; charset=utf-8", "图片不存在"); return; }
  bool ok = true;
  if (sdReady) {
    ok = SD_MMC.remove(imagePath(index));
    for (int i = index + 1; ok && i < imageCount; ++i) ok = SD_MMC.rename(imagePath(i), imagePath(i - 1));
  } else {
    if (ramImages[index]) heap_caps_free(ramImages[index]);
    for (int i = index; i + 1 < imageCount; ++i) ramImages[i] = ramImages[i + 1];
    ramImages[imageCount - 1] = nullptr;
  }
  if (!ok) { scanImages(); server.send(500, "text/plain; charset=utf-8", "删除或重新编号失败"); return; }
  --imageCount;
  if (currentImage == index) currentImage = imageCount ? min(index, (int)imageCount - 1) : -1;
  else if (currentImage > index) --currentImage;
  server.send(200, "text/plain; charset=utf-8", "图片已删除");
}

void enterSettings() { settingsActive = true; settingsFocus = SETTINGS_BRIGHTNESS; clickCount = 0; drawSettingsPage(); }

void updateBrightnessFromTilt() {
  const float tiltDegrees = BRIGHTNESS_TILT_SIGN * angleDifference(levelAngle, brightnessReferenceAngle);
  const int steps = tiltDegrees >= 0.0f ? (int)(tiltDegrees / BRIGHTNESS_DEGREES_PER_STEP) : -(int)(-tiltDegrees / BRIGHTNESS_DEGREES_PER_STEP);
  const uint8_t targetBrightness = constrain((int)brightnessAtAdjustStart + steps * BRIGHTNESS_PERCENT_PER_STEP, 10, 100);
  if (targetBrightness != screenBrightness) {
    screenBrightness = targetBrightness;
    Set_Backlight(screenBrightness);
    drawSettingsPage();
  }
}

#if 0 // Performance management settings are temporarily disabled.
void applyPowerMode(PowerMode mode) {
  brightnessAdjustActive = false;
  powerMode = mode;
  switch (powerMode) {
    case MODE_PERFORMANCE:
      screenBrightness = 100;
      levelUpdateMs = 20;
      levelDeadBandDeg = 0.7f;
      levelLockEnabled = true;
      LevelIMU_SetPerformanceProfile();
      break;
    case MODE_BALANCED:
      screenBrightness = 70;
      levelUpdateMs = 80;
      levelDeadBandDeg = 1.2f;
      levelLockEnabled = true;
      LevelIMU_SetBalancedProfile();
      break;
    case MODE_POWER_SAVE:
      screenBrightness = 40;
      levelLockEnabled = false;
      LevelIMU_SetEnabled(false);
      break;
  }
  Set_Backlight(screenBrightness);
  renderedAngle = 1000.0f;
  if (!animationPlaying && imageCount) showImage(currentImage);
  drawSettingsPage();
}
#endif

void confirmSettingsItem() {
  switch (settingsFocus) {
    case SETTINGS_BRIGHTNESS:
      if (!brightnessAdjustActive && powerMode != MODE_POWER_SAVE) {
        brightnessAdjustActive = true;
        brightnessAtAdjustStart = screenBrightness;
        brightnessReferenceAngle = levelAngle;
      } else brightnessAdjustActive = false;
      drawSettingsPage();
      break;
    case SETTINGS_WIFI: startUploadWiFi(); break;
    case SETTINGS_BRAKE_LIGHT:
      autoBrakeLightEnabled = false;
      saveAutoBrakeSetting();
      brakeLightEnabled = !brakeLightEnabled;
      autoBrakeStatus = "AUTO OFF";
      refreshBrakeVisual();
      drawSettingsPage();
      break;
    case SETTINGS_AUTO_BRAKE:
      autoBrakeLightEnabled = !autoBrakeLightEnabled;
      saveAutoBrakeSetting();
      solarScheduleDay = -1;
      if (autoBrakeLightEnabled) {
        autoBrakeStatus = WiFi.status() == WL_CONNECTED ? "AUTO LOCATING" : "AUTO WAIT WIFI";
        updateAutoBrakeControl(true);
      } else {
        brakeLightEnabled = false;
        autoBrakeStatus = "AUTO OFF";
        refreshBrakeVisual();
      }
      drawSettingsPage();
      break;
#if 0 // Performance management settings are temporarily disabled.
    case SETTINGS_PERFORMANCE: applyPowerMode(MODE_PERFORMANCE); break;
    case SETTINGS_BALANCED: applyPowerMode(MODE_BALANCED); break;
    case SETTINGS_POWER_SAVE: applyPowerMode(MODE_POWER_SAVE); break;
#endif
    case SETTINGS_BACK: leaveSettings(); break;
  }
}

void handleButton() {
  const bool pressed = digitalRead(0) == LOW;
  const uint32_t now = millis();
  if (pressed != buttonDown && now - lastButtonChange > 25) {
    buttonDown = pressed;
    lastButtonChange = now;
    if (pressed) {
      buttonDownAt = now;
      longPressHandled = false;
      sleepPressHandled = false;
      pressStartedInSettings = settingsActive;
    } else if (!longPressHandled && settingsActive && brightnessAdjustActive) {
      brightnessAdjustActive = false;
      clickCount = 0;
      drawSettingsPage();
    } else if (!longPressHandled) {
      ++clickCount;
      lastClickAt = now;
    }
  }
  if (buttonDown && !longPressHandled && now - buttonDownAt >= LONG_PRESS_MS) {
    longPressHandled = true;
    clickCount = 0;
    if (settingsActive) confirmSettingsItem(); else enterSettings();
  }
  if (!pressStartedInSettings && buttonDown && !sleepPressHandled && now - buttonDownAt >= SLEEP_PRESS_MS) {
    sleepPressHandled = true;
    enterDeepSleep();
  }
  if (clickCount && now - lastClickAt > DOUBLE_CLICK_MS) {
    const int direction = clickCount == 1 ? 1 : -1;
    clickCount = 0;
    if (settingsActive && !brightnessAdjustActive) {
      settingsFocus = (settingsFocus + direction + SETTINGS_ITEM_COUNT) % SETTINGS_ITEM_COUNT;
      drawSettingsPage();
    } else if (!settingsActive) {
      if (direction > 0) nextImage(); else previousImage();
    }
  }
}
void connectWiFi() {
  stationConnected = false;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD); // Phone can always return to the device hotspot.
  const String ssid = savedWifiSsid.length() ? savedWifiSsid : String(WIFI_SSID);
  const String password = savedWifiSsid.length() ? savedWifiPassword : String(WIFI_PASSWORD);
  if (ssid.length()) {
    WiFi.begin(ssid.c_str(), password.c_str());
    for (uint8_t i = 0; i < 30 && WiFi.status() != WL_CONNECTED; ++i) delay(500);
    stationConnected = WiFi.status() == WL_CONNECTED;
  }
  Serial.printf("热点 %s，访问 http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  if (stationConnected) Serial.printf("已自动连接 Wi-Fi，局域网访问 http://%s\n", WiFi.localIP().toString().c_str());
}
void startWebServer() {
  if (!serverRoutesReady) {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/upload", HTTP_POST, handleUploadDone, handleUpload);
    server.on("/gif/upload", HTTP_POST, handleGifUploadDone, handleGifUpload);
    server.on("/media", HTTP_GET, handleMediaPage);
    server.on("/media/status", HTTP_GET, handleMediaStatus);
    server.on("/media/delete", HTTP_POST, handleMediaDelete);
    server.on("/wifi-setup", HTTP_GET, handleWifiSetupPage);
    server.on("/wifi/status", HTTP_GET, handleWifiStatus);
    server.on("/wifi/scan", HTTP_GET, handleWifiScan);
    server.on("/wifi/connect", HTTP_POST, handleWifiConnect);
    serverRoutesReady = true;
  }
  if (!serverRunning) { server.begin(); serverRunning = true; }
}
void startUploadWiFi() {
  if (!wifiUploadActive) {
    connectWiFi();
    startWebServer();
    wifiUploadActive = true;
  }
  drawSettingsPage();
}
void stopUploadWiFi() {
  WiFi.softAPdisconnect(true); // Stop only the device hotspot; retain the router connection.
  WiFi.mode(WIFI_STA);
  stationConnected = WiFi.status() == WL_CONNECTED;
  wifiUploadActive = false;
}
void leaveSettings() { brightnessAdjustActive = false; stopUploadWiFi(); settingsActive = false; renderedAngle = 1000.0f; if (animationPlaying) showAnimationFrame(); else if (currentImage >= 0 && currentImage < imageCount) showImage(currentImage); else if (currentGif >= 0 && currentGif < gifCount && startAnimation(currentGif)) showAnimationFrame(); else if (imageCount) showImage(0); else if (animationAvailable && startAnimation(0)) showAnimationFrame(); else { clearScreen(); drawNoImageMessage(); } }
void enterDeepSleep() {
  stopUploadWiFi();
  LevelIMU_SetEnabled(false);
  Set_Backlight(0);
  while (digitalRead(0) == LOW) delay(10); // Release first, otherwise GPIO0 would wake immediately.
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  esp_deep_sleep_start();
}
void setup() {
  Serial.begin(115200);
  loadWifiCredentials();
  pinMode(0, INPUT_PULLUP); I2C_Init(); delay(120); TCA9554PWR_Init(0x00); Set_EXIO(EXIO_PIN8, Low); Backlight_Init(); Set_Backlight(screenBrightness); LCD_Init(); if (!initLevelDisplay()) printf("RGB double frame buffer unavailable\n"); imuReady = LevelIMU_Init(); if (imuReady) LevelIMU_Update(levelAngle); if (USE_SD_CARD) { SD_MMC.setPins(2, 1, 42, -1, -1, -1); Set_EXIO(EXIO_PIN4, High); delay(10); sdReady = SD_MMC.begin("/sdcard", true) && SD_MMC.cardType() != CARD_NONE; } if (sdReady) { migrateLegacyAnimation(); scanImages(); scanAnimation(); if (imageCount) showImage(0); else if (animationAvailable && startAnimation(0)) showAnimationFrame(); else { clearScreen(); drawNoImageMessage(); } } else { clearScreen(); drawNoImageMessage(); }
  const String bootSsid = savedWifiSsid.length() ? savedWifiSsid : String(WIFI_SSID);
  const String bootPassword = savedWifiSsid.length() ? savedWifiPassword : String(WIFI_PASSWORD);
  if (bootSsid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(bootSsid.c_str(), bootPassword.c_str()); // Connect in the background; do not delay display startup.
    startWebServer();
    Serial.printf("Wi-Fi auto-connect started: %s\n", bootSsid.c_str());
  } else WiFi.mode(WIFI_OFF);
}
void loop() {
  if (serverRunning) server.handleClient();
  handleButton();
  if (autoBrakeLightEnabled) {
    const uint32_t autoCheckInterval = solarLocationValid ? 60000UL : 10000UL;
    if (!lastAutoBrakeCheck || millis() - lastAutoBrakeCheck >= autoCheckInterval) {
      lastAutoBrakeCheck = millis();
      updateAutoBrakeControl(false);
    }
  }
  if (settingsActive && wifiUploadActive && !brightnessAdjustActive) {
    const uint8_t wifiFlashPhase = (millis() / 400) & 1;
    if (wifiFlashPhase != lastWifiFlashPhase) { lastWifiFlashPhase = wifiFlashPhase; drawSettingsPage(); }
  } else lastWifiFlashPhase = 0xFF;
  const bool imuUpdated = imuReady && LevelIMU_Update(levelAngle);
  const bool newBrakeLightActive = brakeLightEnabled && LevelIMU_IsBrakeDetected();
  if (newBrakeLightActive != brakeLightActive) {
    brakeLightActive = newBrakeLightActive;
    renderedAngle = 1000.0f;
    if (animationPlaying) nextAnimationFrameAt = 0;
    else if (!settingsActive && imageCount) renderLevelFrame(true);
  }
  if (!settingsActive && animationPlaying && (int32_t)(millis() - nextAnimationFrameAt) >= 0) showAnimationFrame();
  if (imuUpdated) { if (brightnessAdjustActive) updateBrightnessFromTilt(); else if (!settingsActive && !animationPlaying) renderLevelFrame(); }
  delay(2);
}
