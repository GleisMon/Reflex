/* =============================================================================
 *  Reflex — игра на реакцию (Wemos D1 Mini / ESP8266), Arduino IDE
 *  Версия:    v0.4.0  (TFT-табло на борту, меню на P1, без Wi-Fi/веба)
 *
 *  Экран:     2.0" TFT, контроллер ST7789V, 240x320, SPI (только запись).
 *             Табло теперь на самом устройстве — смартфон больше не нужен.
 *  Звук:      пассивный пьезо на GPIO15; аппаратный Timer1 (чистый меандр).
 *
 *  Библиотеки (Менеджер библиотек Arduino IDE):
 *    - Adafruit ST7735 and ST7789 Library  (тянет Adafruit GFX + BusIO)
 *    - Adafruit GFX Library
 *    - Adafruit BusIO
 *    - LittleFS — в ядре ESP8266
 *
 *  ----------------------------- РАСПИНОВКА ---------------------------------
 *    P1 = D1/GPIO5   — игрок + МЕНЮ/СЕРВИС (короткий тап = след. пункт,
 *                       удержание = сменить значение). Игроком становится
 *                       только при полном столе (когда играют все четверо).
 *    P2 = D2/GPIO4   — игрок
 *    P3 = D0/GPIO16  — игрок  ← ВНЕШНИЙ pull-up 10к на 3V3 (у GPIO16 нет своего)
 *    P4 = D6/GPIO12  — игрок  (бывш. HSPI MISO — дисплею не нужен)
 *    Пищалка = D8/GPIO15  (в покое LOW = совпадает с boot-strap)
 *
 *    TFT SCLK = D5/GPIO14     TFT MOSI/SDA = D7/GPIO13   (аппаратный SPI)
 *    TFT CS   = D3/GPIO0      TFT DC       = D4/GPIO2    (выходы; на сбросе HIGH)
 *    TFT RST  -> 3V3 (или RST платы)      TFT BLK/LED  -> 3V3
 *
 *  Boot-strap: ни одна кнопка не сидит на GPIO0/2/15. CS/DC заняты как выходы
 *  и сами подтянуты в HIGH при сбросе, поэтому загрузке не мешают.
 *
 *  Примечание: штатный шрифт Adafruit GFX — только латиница. Подписи на экране
 *  в этой сборке латинские (бренд + EN). Кириллица RU/UK добавится отдельным
 *  GFX-шрифтом — это следующий шаг.
 * ===========================================================================*/

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <LittleFS.h>

/* ----------------------------- ПИНЫ --------------------------------------- */
#define PIN_BUZZER 15
#define TFT_CS   0
#define TFT_DC   2
#define TFT_RST -1                          // RST на пин RST платы (или 3V3) — софт-сброс в init()
#define TFT_MOSI 13                         // SDA — программный SPI
#define TFT_SCLK 14                         // SCL — программный SPI
static const uint8_t BTN_PIN[4] = {5, 4, 16, 12};   // P1..P4

/* --------------------------- ДИСПЛЕЙ -------------------------------------- */
/* Программный (бит-бэнг) SPI по тем же проводам SDA=GPIO13, SCL=GPIO14.
 * ВАЖНО: аппаратный SPI забирает себе GPIO12 (MISO), а там у нас кнопка P4 —
 * поэтому именно программный SPI, чтобы GPIO12 остался свободен под P4. */
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
#define SCR_W 320                           // ландшафт (rotation 1)
#define SCR_H 240

/* --------------------------- ТАЙМИНГИ ------------------------------------- */
#define DEBOUNCE_MS 12
#define ROUNDS_PER_SERIES 2
#define REG_HOLD_MS 3000
#define RESULT_SHOW_MS 3500
#define ACTIVE_TIMEOUT_MS 10000
#define PAUSE_MIN_MS 1000
#define PAUSE_MAX_MS 10000
#define TAPWIN_MIN_MS 2000
#define TAPWIN_MAX_MS 10000
#define MENU_HOLD_MS 600                    // P1: удержание = «сменить значение»
#define MENU_TIMEOUT_MS 12000               // авто-выход из меню без действий
#define IDLE_PAGE_MS 5000                   // период смены страниц в покое
#define DEMO_FRAME_MS 320                   // кадр анимации демо
#define DEMO_PHASES 8

/* --------------------------- РЕЖИМЫ / СОСТОЯНИЯ --------------------------- */
enum Mode  { MODE_SPEED = 0, MODE_TAPS = 1, MODE_COUNT };
enum State { ST_IDLE, ST_MENU, ST_REGISTERING, ST_SERIES_START,
             ST_ROUND_WAIT, ST_ROUND_ACTIVE, ST_ROUND_RESULT };
State state = ST_IDLE;

/* Структуры объявляем ДО первой функции: Arduino IDE генерирует прототипы
 * в начале файла, и типы-параметры должны быть видны раньше. */
struct Btn  { uint8_t pin; bool stable, lastRead; uint32_t lastChange; bool pressedEdge, releasedEdge; };
struct Note { uint16_t f, d; };
struct Mel  { const Note* n; uint8_t len; };
struct Theme{ uint16_t bg, fg, dim, acc; };
Btn players[4];

/* --------------------- ДАННЫЕ (RAM + дублирование в LittleFS) ------------- */
#define DATA_MAGIC 0xCB04
#define NO_MS 0xFFFF
struct GameData {
  uint16_t magic;
  int16_t  score[MODE_COUNT][4];
  uint16_t bestMs[4];                       // лучшая реакция (режим скорости)
  uint8_t  mode, sound, soundset, lang, theme;
};
GameData data;
const char* DATA_FILE = "/reflex.bin";

void initData(){ data.magic=DATA_MAGIC;
  for(uint8_t m=0;m<MODE_COUNT;m++) for(uint8_t i=0;i<4;i++) data.score[m][i]=0;
  for(uint8_t i=0;i<4;i++) data.bestMs[i]=NO_MS;
  data.mode=MODE_SPEED; data.sound=1; data.soundset=0; data.lang=0; data.theme=0; }
void saveData(){ File f=LittleFS.open(DATA_FILE,"w"); if(f){ f.write((uint8_t*)&data,sizeof(data)); f.close(); } }
void loadData(){ if(LittleFS.exists(DATA_FILE)){ File f=LittleFS.open(DATA_FILE,"r");
    if(f){ f.read((uint8_t*)&data,sizeof(data)); f.close(); if(data.magic==DATA_MAGIC) return; } }
  initData(); saveData(); }

/* --------------- РАНТАЙМ (не сохраняется) -------------------------------- */
uint8_t  streak[MODE_COUNT][4] = {{0}};
bool     isPlayer[4] = {false};
uint8_t  roundIndex = 0;
bool     falseStarted[4] = {false}, excluded[4] = {false}, armed[4] = {false}, pressedAtStart[4] = {false};
uint16_t taps[4] = {0};
uint16_t bestTaps = 0;
uint32_t tStateEnter = 0, pauseDur = 0, tapWindow = 0, goMicros = 0;
int8_t   winnerA = -1;
uint8_t  winnerMask = 0;
uint32_t reactionUs = 0;
bool     comboShown = false;
bool     dirty = true;

/* меню / страницы покоя */
uint8_t  menuCursor = 0;
uint32_t menuLastInput = 0;
uint32_t p1DownAt = 0;
bool     p1HoldFired = false;
uint8_t  idlePage = 0;                       // 0 = таблица, 1 = демо
uint32_t idlePageAt = 0;
uint8_t  demoPhase = 0;
uint32_t demoFrameAt = 0;
uint32_t lastRegDraw = 0;

enum MenuItem { MI_MODE, MI_SOUND, MI_SNDSET, MI_THEME, MI_RESET, MI_EXIT, MI_COUNT };

/* --------------------------- ПАЛИТРА -------------------------------------- */
#define RGB565(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))
const Theme THEMES[5] = {
  /* DOOM    */ { RGB565(16,12,10),  RGB565(210,200,190), RGB565(90,80,75),   RGB565(220,70,30) },
  /* FALLOUT */ { RGB565(6,16,8),    RGB565(120,255,140), RGB565(40,110,55),  RGB565(180,255,120) },
  /* GRAY    */ { RGB565(20,22,26),  RGB565(230,232,235), RGB565(110,115,122),RGB565(150,160,175) },
  /* AMBER   */ { RGB565(12,9,2),    RGB565(255,190,60),  RGB565(120,85,20),  RGB565(255,150,20) },
  /* NEON    */ { RGB565(8,6,18),    RGB565(120,235,255), RGB565(60,70,120),  RGB565(255,70,200) },
};
const uint16_t PCOL[4] = { RGB565(230,60,50), RGB565(70,210,90), RGB565(70,140,240), RGB565(240,200,60) };
inline const Theme& TH(){ return THEMES[data.theme>4?0:data.theme]; }

/* ===========================================================================
 *                  ЗВУК: аппаратный Timer1, чистый меандр
 * ===========================================================================*/
volatile bool    toneActive = false;
volatile uint8_t toneLevel  = 0;

void IRAM_ATTR onTimer1(){
  if(!toneActive) return;
  toneLevel ^= 1;
  if(toneLevel) GPOS = (1 << PIN_BUZZER);
  else          GPOC = (1 << PIN_BUZZER);
}
void toneFreq(uint16_t f){
  if(f == 0){ toneActive=false; GPOC=(1<<PIN_BUZZER); return; }
  toneActive=false;
  timer1_write(2500000UL / f);
  toneActive=true;
}

/* ----- неблокирующий проигрыватель мелодий ------------------------------- */
enum Ev { EV_BOOT, EV_ARM, EV_SERIES, EV_GO, EV_FALSE, EV_WIN, EV_COMBO, EV_MODE, EV_COUNT };

const Note D_BOOT[]={{165,120},{165,120},{330,120},{165,120},{147,120},{165,120},{196,260}};
const Note D_ARM[]={{880,50}}; const Note D_SER[]={{523,90},{659,90},{784,150}}; const Note D_GO[]={{1568,160}};
const Note D_FALSE[]={{400,120},{300,150},{200,220}}; const Note D_WIN[]={{784,100},{988,100},{1319,200}};
const Note D_COMBO[]={{523,80},{659,80},{784,80},{1047,180},{784,80},{1047,280}}; const Note D_MODE[]={{660,70},{990,90}};
const Note A_BOOT[]={{523,80},{659,80},{784,80},{1047,150}}; const Note A_ARM[]={{1318,40}};
const Note A_SER[]={{784,80},{1047,80},{1318,150}}; const Note A_GO[]={{2093,140}};
const Note A_FALSE[]={{523,100},{392,120},{262,180}}; const Note A_WIN[]={{1047,90},{1318,90},{1568,90},{2093,200}};
const Note A_COMBO[]={{1047,70},{1318,70},{1568,70},{2093,160},{1568,70},{2093,260}}; const Note A_MODE[]={{880,60},{1318,90}};
const Note B_BOOT[]={{1000,60},{0,40},{1000,60}}; const Note B_ARM[]={{1200,30}};
const Note B_SER[]={{800,60},{1000,60},{1200,120}}; const Note B_GO[]={{1800,120}};
const Note B_FALSE[]={{600,80},{400,140}}; const Note B_WIN[]={{1000,70},{1400,70},{1800,160}};
const Note B_COMBO[]={{1400,60},{1800,60},{1400,60},{1800,220}}; const Note B_MODE[]={{1000,50},{1400,70}};
const Note C_BOOT[]={{330,120},{392,120},{494,120},{659,200}}; const Note C_ARM[]={{988,40}};
const Note C_SER[]={{587,90},{740,90},{988,150}}; const Note C_GO[]={{1318,150}};
const Note C_FALSE[]={{494,110},{392,130},{294,200}}; const Note C_WIN[]={{784,90},{988,90},{1175,90},{1568,200}};
const Note C_COMBO[]={{988,70},{1175,70},{1318,70},{1760,180},{1318,70},{1760,260}}; const Note C_MODE[]={{740,60},{1109,90}};

#define M(a) { a, (uint8_t)(sizeof(a)/sizeof(Note)) }
const Mel SND[4][EV_COUNT] = {
  { M(D_BOOT),M(D_ARM),M(D_SER),M(D_GO),M(D_FALSE),M(D_WIN),M(D_COMBO),M(D_MODE) },
  { M(A_BOOT),M(A_ARM),M(A_SER),M(A_GO),M(A_FALSE),M(A_WIN),M(A_COMBO),M(A_MODE) },
  { M(B_BOOT),M(B_ARM),M(B_SER),M(B_GO),M(B_FALSE),M(B_WIN),M(B_COMBO),M(B_MODE) },
  { M(C_BOOT),M(C_ARM),M(C_SER),M(C_GO),M(C_FALSE),M(C_WIN),M(C_COMBO),M(C_MODE) },
};
const Note* melody=nullptr; uint8_t melodyLen=0, melodyIdx=0; uint32_t noteStart=0; bool melodyPlaying=false;
void startNote(){ noteStart=millis(); toneFreq(melody[melodyIdx].f); }
void play(uint8_t ev){ if(!data.sound) return; const Mel &m=SND[data.soundset][ev];
  melody=m.n; melodyLen=m.len; melodyIdx=0; melodyPlaying=true; startNote(); }
void updateMelody(){ if(!melodyPlaying) return; if((millis()-noteStart) >= melody[melodyIdx].d){ melodyIdx++;
  if(melodyIdx>=melodyLen){ toneFreq(0); melodyPlaying=false; return; } startNote(); } }

/* ===========================================================================
 *                              КНОПКИ + антидребезг
 * ===========================================================================*/
void btnInit(Btn &b, uint8_t pin){ b.pin=pin;
  if(pin==16) pinMode(pin, INPUT);              // GPIO16: внешний pull-up на 3V3
  else        pinMode(pin, INPUT_PULLUP);
  b.stable=false; b.lastRead=false; b.lastChange=millis(); b.pressedEdge=false; b.releasedEdge=false; }
void btnUpdate(Btn &b){ b.pressedEdge=false; b.releasedEdge=false; bool raw=(digitalRead(b.pin)==LOW); uint32_t now=millis();
  if(raw!=b.lastRead){ b.lastRead=raw; b.lastChange=now; }
  if((now-b.lastChange)>=DEBOUNCE_MS && b.stable!=b.lastRead){ b.stable=b.lastRead; if(b.stable)b.pressedEdge=true; else b.releasedEdge=true; } }
void updateAllButtons(){ for(uint8_t i=0;i<4;i++) btnUpdate(players[i]); }
bool anyPlayerHeld(){ for(uint8_t i=0;i<4;i++) if(players[i].stable) return true; return false; }
bool anyOtherHeld(){ for(uint8_t i=1;i<4;i++) if(players[i].stable) return true; return false; }  // P2..P4

/* ===========================================================================
 *                              ОТРИСОВКА (TFT)
 * ===========================================================================*/
const char* MODE_NAME(uint8_t m){ return m==MODE_TAPS ? "MODE B - TAPS" : "MODE A - SPEED"; }

void textCentered(const char* s, int16_t y, uint8_t size, uint16_t color){
  tft.setTextWrap(false); tft.setTextSize(size); tft.setTextColor(color);
  int16_t x1,y1; uint16_t w,h; tft.getTextBounds(s,0,y,&x1,&y1,&w,&h);
  int16_t x=(SCR_W-(int16_t)w)/2 - x1; if(x<0) x=0;
  tft.setCursor(x,y); tft.print(s);
}
void textAt(const char* s, int16_t x, int16_t y, uint8_t size, uint16_t color){
  tft.setTextWrap(false); tft.setTextSize(size); tft.setTextColor(color); tft.setCursor(x,y); tft.print(s);
}
void drawHeader(const char* sub){
  tft.fillRect(0,0,SCR_W,40,TH().bg);
  textCentered("REFLEX", 6, 3, TH().fg);
  tft.drawFastHLine(0,38,SCR_W,TH().dim);
  if(sub) textCentered(sub, 44, 1, TH().acc);
}

/* --- страница покоя: таблица рейтинга текущего режима --- */
void drawLeaderboard(bool full){
  if(!full) return;                                   // в покое таблица статична
  tft.fillScreen(TH().bg);
  drawHeader(MODE_NAME(data.mode));
  int16_t y=68; const int16_t rh=40;
  for(uint8_t i=0;i<4;i++){
    tft.drawRoundRect(10,y,SCR_W-20,rh-8,4,TH().dim);
    tft.fillRect(16,y+6,18,rh-20,PCOL[i]);
    char tag[4]; snprintf(tag,sizeof(tag),"P%u",i+1);
    textAt(tag,42,y+9,2,TH().fg);
    char sc[8]; snprintf(sc,sizeof(sc),"%d",data.score[data.mode][i]);
    textAt(sc,110,y+9,2,TH().fg);
    if(data.mode==MODE_SPEED){
      char bm[16];
      if(data.bestMs[i]==NO_MS) snprintf(bm,sizeof(bm),"best  --");
      else                      snprintf(bm,sizeof(bm),"best %ums",data.bestMs[i]);
      textAt(bm,180,y+12,1,TH().dim);
    }
    y+=rh;
  }
  textCentered("tap P1 = settings", SCR_H-14, 1, TH().dim);
}

/* --- страница покоя: демо-анимация --- */
void drawDemo(bool full){
  if(full){ tft.fillScreen(TH().bg); drawHeader("DEMO"); }
  int16_t band=96, bh=72;
  tft.fillRect(0,band,SCR_W,bh,TH().bg);
  uint8_t lit = demoPhase % 4;
  bool flash = (demoPhase >= 4);
  int16_t bw=56, gap=14, total=4*bw+3*gap, x0=(SCR_W-total)/2;
  for(uint8_t i=0;i<4;i++){
    uint16_t c = (i==lit && !flash) ? PCOL[i] : TH().dim;
    tft.fillRoundRect(x0+i*(bw+gap), band+8, bw, bh-16, 6, c);
  }
  textCentered(flash ? "PRESS!" : "GET READY", band+bh+8, 2, flash?TH().acc:TH().fg);
}

/* --- регистрация / голосование --- */
void drawRegister(bool full){
  if(full){ tft.fillScreen(TH().bg); drawHeader("REGISTER"); }
  int32_t left = REG_HOLD_MS-(int32_t)(millis()-tStateEnter); if(left<0) left=0;
  int cd = (int)((left+999)/1000);
  char c[4]; snprintf(c,sizeof(c),"%d",cd);
  tft.fillRect(0,80,SCR_W,90,TH().bg);
  textCentered(c, 84, 8, TH().acc);
  // подсветка удерживаемых
  int16_t bw=56, gap=14, total=4*bw+3*gap, x0=(SCR_W-total)/2, y=186;
  for(uint8_t i=0;i<4;i++){
    bool held=players[i].stable;
    tft.fillRoundRect(x0+i*(bw+gap), y, bw, 36, 5, held?PCOL[i]:TH().bg);
    tft.drawRoundRect(x0+i*(bw+gap), y, bw, 36, 5, TH().dim);
  }
  textCentered("hold buttons together", SCR_H-12, 1, TH().dim);
}

/* --- общий баннер крупным текстом --- */
void drawBanner(const char* s, uint16_t color){
  tft.fillScreen(TH().bg);
  textCentered(s, SCR_H/2-24, 4, color);
}

/* --- фаза ожидания сигнала --- */
void drawWait(bool full){
  if(full){ tft.fillScreen(TH().bg);
    textCentered("WAIT...", SCR_H/2-40, 4, TH().fg);
    textCentered("don't press early", SCR_H/2+20, 1, TH().dim);
    char r[16]; snprintf(r,sizeof(r),"round %u/%u", roundIndex+1, ROUNDS_PER_SERIES);
    textCentered(r, SCR_H-16, 1, TH().dim);
  }
}

/* --- активная фаза --- */
void drawActive(bool full){
  if(data.mode==MODE_SPEED){
    if(full) drawBanner("PRESS!", TH().acc);
  } else {
    if(full){ tft.fillScreen(TH().bg); textCentered("TAP! TAP! TAP!", 16, 2, TH().acc); }
    int16_t cw=150, ch=80, gx=(SCR_W-2*cw-10)/2, gy=58;
    for(uint8_t i=0;i<4;i++){
      int16_t x=gx+(i%2)*(cw+10), y=gy+(i/2)*(ch+10);
      uint16_t c = isPlayer[i] ? (excluded[i]?TH().dim:PCOL[i]) : TH().dim;
      tft.fillRect(x+6,y+6,cw-12,ch-12,TH().bg);
      tft.drawRoundRect(x,y,cw,ch,6,c);
      char tag[4]; snprintf(tag,sizeof(tag),"P%u",i+1); textAt(tag,x+10,y+8,1,c);
      char n[8]; if(!isPlayer[i]) snprintf(n,sizeof(n),"-"); else if(excluded[i]) snprintf(n,sizeof(n),"X"); else snprintf(n,sizeof(n),"%u",taps[i]);
      tft.setTextSize(3); int16_t x1,y1; uint16_t w,h; tft.getTextBounds(n,0,0,&x1,&y1,&w,&h);
      textAt(n, x+(cw-(int16_t)w)/2, y+30, 3, isPlayer[i]&&!excluded[i]?TH().fg:TH().dim);
    }
  }
}

/* --- результат раунда --- */
void drawResult(bool full){
  if(!full) return;
  tft.fillScreen(TH().bg);
  if(!winnerMask){ textCentered("NO WINNER", SCR_H/2-24, 4, TH().dim); return; }
  int first=-1, cnt=0; for(uint8_t i=0;i<4;i++) if(winnerMask&(1<<i)){ if(first<0)first=i; cnt++; }
  char line[20];
  if(cnt==1) snprintf(line,sizeof(line),"P%d WINS", first+1);
  else       snprintf(line,sizeof(line),"P%d +%d WIN", first+1, cnt-1);
  textCentered(line, 60, 4, PCOL[first<0?0:first]);
  if(data.mode==MODE_SPEED){
    char r[20]; snprintf(r,sizeof(r),"%lu ms", (unsigned long)(reactionUs/1000));
    textCentered(r, 130, 3, TH().fg);
  } else {
    char r[20]; snprintf(r,sizeof(r),"%u taps", bestTaps);
    textCentered(r, 130, 3, TH().fg);
  }
  if(comboShown) textCentered("COMBO +3!", 180, 2, TH().acc);
}

/* --- меню настроек --- */
void menuValue(uint8_t item, char* out, size_t n){
  switch(item){
    case MI_MODE:   snprintf(out,n,"%s", data.mode==MODE_TAPS?"TAPS":"SPEED"); break;
    case MI_SOUND:  snprintf(out,n,"%s", data.sound?"ON":"OFF"); break;
    case MI_SNDSET:{ const char* s[]={"DOOM","ARCADE","BEEP","CHIP"}; snprintf(out,n,"%s", s[data.soundset&3]); } break;
    case MI_THEME: { const char* s[]={"DOOM","FALLOUT","GRAY","AMBER","NEON"}; snprintf(out,n,"%s", s[data.theme>4?0:data.theme]); } break;
    case MI_RESET:  snprintf(out,n,"hold >"); break;
    case MI_EXIT:   snprintf(out,n,"hold >"); break;
    default:        out[0]=0;
  }
}
void drawMenu(bool full){
  if(full){ tft.fillScreen(TH().bg); drawHeader("SETTINGS"); }
  const char* lbl[MI_COUNT] = {"Mode","Sound","Sound set","Theme","Reset scores","Exit"};
  int16_t y=64; const int16_t rh=28;
  for(uint8_t i=0;i<MI_COUNT;i++){
    bool sel=(i==menuCursor);
    tft.fillRect(8,y,SCR_W-16,rh-4,sel?TH().dim:TH().bg);
    textAt(lbl[i], 18, y+6, 2, sel?TH().bg:TH().fg);
    char v[16]; menuValue(i,v,sizeof(v));
    tft.setTextSize(2); int16_t x1,y1; uint16_t w,h; tft.getTextBounds(v,0,0,&x1,&y1,&w,&h);
    textAt(v, SCR_W-18-(int16_t)w, y+6, 2, sel?TH().bg:TH().acc);
    y+=rh;
  }
  if(full) textCentered("tap = next   hold = change", SCR_H-14, 1, TH().dim);
}

/* --- диспетчер отрисовки --- */
void render(){
  static int8_t  pst=-1;
  static uint8_t ppage=255;
  dirty=false;
  bool full = (pst!=(int8_t)state) || (state==ST_IDLE && ppage!=idlePage);
  pst=(int8_t)state; ppage=idlePage;
  switch(state){
    case ST_IDLE:         if(idlePage==0) drawLeaderboard(full); else drawDemo(full); break;
    case ST_MENU:         drawMenu(full); break;
    case ST_REGISTERING:  drawRegister(full); break;
    case ST_SERIES_START: if(full) drawBanner("GET READY", TH().acc); break;
    case ST_ROUND_WAIT:   drawWait(full); break;
    case ST_ROUND_ACTIVE: drawActive(full); break;
    case ST_ROUND_RESULT: drawResult(full); break;
  }
}

/* ===========================================================================
 *                       ПЕРЕХОДЫ / ИГРОВАЯ ЛОГИКА
 * ===========================================================================*/
void enterState(State s){ state=s; tStateEnter=millis(); dirty=true; }
void beginRound(){ for(uint8_t i=0;i<4;i++){ falseStarted[i]=false; excluded[i]=false; armed[i]=false; pressedAtStart[i]=false; taps[i]=0; }
  winnerA=-1; winnerMask=0; comboShown=false; pauseDur=random(PAUSE_MIN_MS,PAUSE_MAX_MS+1); enterState(ST_ROUND_WAIT); }
void startSeries(){ roundIndex=0; play(EV_SERIES); enterState(ST_SERIES_START); }

void applyWin(uint8_t p){ data.score[data.mode][p]+=1; streak[data.mode][p]+=1;
  if(streak[data.mode][p]%3==0){ data.score[data.mode][p]+=3; comboShown=true; winnerMask|=(1<<p); } }
void resetStreaks(uint8_t keep){ for(uint8_t i=0;i<4;i++) if(isPlayer[i] && !(keep&(1<<i))) streak[data.mode][i]=0; }

void finishRound(){
  if(data.mode==MODE_SPEED){
    if(winnerA>=0){ winnerMask=(1<<winnerA); uint32_t rms=reactionUs/1000; if(rms>NO_MS-1)rms=NO_MS-1; uint16_t ms=(uint16_t)rms;
      if(ms<data.bestMs[winnerA]||data.bestMs[winnerA]==NO_MS) data.bestMs[winnerA]=ms;
      applyWin(winnerA); resetStreaks(winnerMask); }
    else { winnerMask=0; resetStreaks(0); }
  } else {
    uint16_t best=0; for(uint8_t i=0;i<4;i++) if(isPlayer[i]&&!excluded[i]&&taps[i]>best) best=taps[i]; bestTaps=best;
    if(best==0){ winnerMask=0; resetStreaks(0); }
    else { uint8_t mask=0; for(uint8_t i=0;i<4;i++) if(isPlayer[i]&&!excluded[i]&&taps[i]==best) mask|=(1<<i);
      winnerMask=mask; for(uint8_t i=0;i<4;i++) if(mask&(1<<i)) applyWin(i); resetStreaks(mask); }
  }
  saveData(); if(winnerMask) play(EV_WIN); enterState(ST_ROUND_RESULT);
}

/* ===========================================================================
 *                       МЕНЮ НА P1 (короткий тап / удержание)
 * ===========================================================================*/
void enterMenu(){ menuCursor=0; menuLastInput=millis(); p1HoldFired=false; play(EV_MODE); enterState(ST_MENU); }

void menuApply(){
  switch(menuCursor){
    case MI_MODE:   data.mode=(data.mode+1)%MODE_COUNT; play(EV_MODE); break;
    case MI_SOUND:  data.sound=!data.sound; if(data.sound) play(EV_MODE); break;
    case MI_SNDSET: data.soundset=(data.soundset+1)&3; play(EV_MODE); break;
    case MI_THEME:  data.theme=(data.theme+1)%5; play(EV_MODE); break;
    case MI_RESET:  for(uint8_t m=0;m<MODE_COUNT;m++) for(uint8_t i=0;i<4;i++){ data.score[m][i]=0; streak[m][i]=0; }
                    for(uint8_t i=0;i<4;i++) data.bestMs[i]=NO_MS; play(EV_FALSE); break;
    case MI_EXIT:   saveData(); enterState(ST_IDLE); return;
  }
  saveData(); dirty=true;
}

void handleMenu(){
  if(players[0].pressedEdge){ p1DownAt=millis(); p1HoldFired=false; }
  if(players[0].stable && !p1HoldFired && (millis()-p1DownAt)>=MENU_HOLD_MS){
    p1HoldFired=true; menuLastInput=millis();
    bool wasTheme=(menuCursor==MI_THEME);
    menuApply();
    if(state==ST_MENU) drawMenu(wasTheme);          // тема -> полная перерисовка, иначе обновить список
  }
  if(players[0].releasedEdge){
    if(!p1HoldFired && (millis()-p1DownAt)<MENU_HOLD_MS){
      menuCursor=(menuCursor+1)%MI_COUNT; menuLastInput=millis(); play(EV_ARM); drawMenu(false);
    }
  }
  if((millis()-menuLastInput)>=MENU_TIMEOUT_MS){ saveData(); enterState(ST_IDLE); }
}

/* ===========================================================================
 *                                SETUP
 * ===========================================================================*/
void setup(){
  Serial.begin(115200); Serial.println(F("\nReflex v0.4.0 (TFT)"));

  for(uint8_t i=0;i<4;i++) btnInit(players[i],BTN_PIN[i]);

  pinMode(PIN_BUZZER,OUTPUT); GPOC=(1<<PIN_BUZZER);
  timer1_isr_init(); timer1_attachInterrupt(onTimer1); timer1_enable(TIM_DIV16,TIM_EDGE,TIM_LOOP); timer1_write(2500);

  if(!LittleFS.begin()){ LittleFS.format(); LittleFS.begin(); }
  loadData();

  randomSeed(ESP.getCycleCount()^micros());

  tft.init(240,320);            // физический контроллер 240x320
  tft.setRotation(1);           // -> ландшафт 320x240
  tft.fillScreen(TH().bg);
  textCentered("REFLEX", SCR_H/2-30, 4, TH().fg);
  textCentered("v0.4.0", SCR_H/2+12, 2, TH().acc);

  play(EV_BOOT);
  // Заставку держим, но во время неё продолжаем крутить звуковой автомат:
  // иначе первая нота джингла «зависает» на всю задержку и звук кривой.
  uint32_t tSplash=millis();
  while((millis()-tSplash) < 900){ updateMelody(); delay(1); }

  idlePageAt=millis(); demoFrameAt=millis();
  enterState(ST_IDLE);
}

/* ===========================================================================
 *                                 LOOP
 * ===========================================================================*/
void loop(){
  updateAllButtons(); updateMelody();

  switch(state){
    case ST_IDLE:
      // чередование страниц покоя
      if((millis()-idlePageAt)>=IDLE_PAGE_MS){ idlePage^=1; idlePageAt=millis(); dirty=true; }
      if(idlePage==1 && (millis()-demoFrameAt)>=DEMO_FRAME_MS){ demoPhase=(demoPhase+1)%DEMO_PHASES; demoFrameAt=millis(); dirty=true; }
      // старт регистрации/голосования: держат любую из P2..P4 (НЕ P1)
      if(anyOtherHeld()){ enterState(ST_REGISTERING); play(EV_ARM); break; }
      // P1 короткий тап -> меню
      if(players[0].pressedEdge) p1DownAt=millis();
      if(players[0].releasedEdge && (millis()-p1DownAt)<MENU_HOLD_MS) enterMenu();
      break;

    case ST_MENU:
      handleMenu();
      break;

    case ST_REGISTERING:
      if(!anyPlayerHeld()){ enterState(ST_IDLE); break; }
      if((millis()-tStateEnter)>=REG_HOLD_MS){
        for(uint8_t i=1;i<4;i++) isPlayer[i]=players[i].stable;           // P2..P4 по факту удержания
        isPlayer[0]=players[0].stable && isPlayer[1] && isPlayer[2] && isPlayer[3]; // P1 — только полный стол
        uint8_t cnt=0; for(uint8_t i=0;i<4;i++) if(isPlayer[i])cnt++;
        randomSeed(ESP.getCycleCount()^micros());
        if(cnt>=2) startSeries(); else { play(EV_FALSE); enterState(ST_IDLE); }
      } else if((millis()-lastRegDraw)>=90){ lastRegDraw=millis(); dirty=true; }  // живой отсчёт ~11 кадров/с      break;

    case ST_SERIES_START: { bool allRel=true; for(uint8_t i=0;i<4;i++) if(isPlayer[i]&&players[i].stable) allRel=false;
      if(allRel || (millis()-tStateEnter)>4000) beginRound(); } break;

    case ST_ROUND_WAIT:
      for(uint8_t i=0;i<4;i++){ if(!isPlayer[i]) continue;
        if(players[i].pressedEdge){
          if(data.mode==MODE_SPEED){ if(!falseStarted[i]){ falseStarted[i]=true; data.score[data.mode][i]-=1; saveData(); play(EV_FALSE); } }
          else { if(!excluded[i]){ excluded[i]=true; play(EV_FALSE); } } } }
      if((millis()-tStateEnter)>=pauseDur){
        if(data.mode==MODE_SPEED){ play(EV_GO); goMicros=micros();
          for(uint8_t i=0;i<4;i++){ pressedAtStart[i]=players[i].stable; armed[i]=isPlayer[i]&&!players[i].stable; } enterState(ST_ROUND_ACTIVE); }
        else { tapWindow=random(TAPWIN_MIN_MS,TAPWIN_MAX_MS+1); play(EV_GO); enterState(ST_ROUND_ACTIVE); } }
      break;

    case ST_ROUND_ACTIVE:
      if(data.mode==MODE_SPEED){
        for(uint8_t i=0;i<4;i++){ if(!isPlayer[i]) continue;
          if(!armed[i]&&players[i].releasedEdge) armed[i]=true;
          if(armed[i]&&players[i].pressedEdge){ winnerA=i; reactionUs=micros()-goMicros; finishRound(); break; } }
        if(state==ST_ROUND_ACTIVE && (millis()-tStateEnter)>ACTIVE_TIMEOUT_MS) finishRound();
      } else {
        for(uint8_t i=0;i<4;i++) if(isPlayer[i]&&!excluded[i]&&players[i].pressedEdge){ if(taps[i]<9999) taps[i]++; dirty=true; }
        if((millis()-tStateEnter)>=tapWindow) finishRound();
      }
      break;

    case ST_ROUND_RESULT: {
      static bool comboFlashed=false;
      if(comboShown && !comboFlashed && (millis()-tStateEnter)>1400 && !melodyPlaying){ comboFlashed=true; play(EV_COMBO); }
      if((millis()-tStateEnter)>=RESULT_SHOW_MS){ comboFlashed=false; roundIndex++;
        if(roundIndex<ROUNDS_PER_SERIES) beginRound(); else enterState(ST_IDLE); } } break;
  }

  if(dirty) render();
}
