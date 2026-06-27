/* =============================================================================
 *  Reflex — игра на реакцию (Wemos D1 Mini / ESP8266MOD), Arduino IDE
 *  Версия:    v0.3.1  (рефакторинг: Timer1-звук, новый GPIO, WebSockets, LittleFS)
 *
 *  Дисплей:   нет (табло = телефон). Сигнал старта даёт пищалка.
 *  Звук:      пассивный пьезо на GPIO15; генерация на аппаратном Timer1
 *             (чистый меандр, неблокирующий автомат, нечувствителен к Wi-Fi).
 *
 *  Библиотеки (Менеджер библиотек — оба ставятся в один клик):
 *    - WebSockets   by Markus Sattler (Links2004/arduinoWebSockets)
 *    - ESP8266WebServer, LittleFS, Ticker, ESP8266WiFi — в ядре ESP8266
 *
 *  Связь: телефон -> Wi-Fi "Reflex" -> http://192.168.4.1 (страница из gzip),
 *         состояние и события идут по WebSocket (порт 81), ESP <-> телефон.
 *
 *  ----------------------------- РАСПИНОВКА ---------------------------------
 *    P1 = D1/GPIO5   P2 = D2/GPIO4   P3 = D5/GPIO14   P4 = D6/GPIO12
 *    Сервисная = D7/GPIO13           Пищалка = D8/GPIO15
 *    Кнопки игроков НЕ на GPIO0/2/15 -> безопасная загрузка.
 * ===========================================================================*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>
#include <LittleFS.h>
#include "index_html_gz.h"

/* ----------------------------- ПИНЫ --------------------------------------- */
#define PIN_BUZZER 15
#define PIN_SERVICE 13
static const uint8_t BTN_PIN[4] = {5, 4, 14, 12};   // P1..P4

/* --------------------------- ТАЙМИНГИ ------------------------------------- */
#define DEBOUNCE_MS 12
#define ROUNDS_PER_SERIES 2
#define REG_HOLD_MS 3000
#define SVC_RESET_MS 3000
#define SVC_MUTE_MS 6000
#define SVC_SHORT_MS 1000
#define RESULT_SHOW_MS 3500
#define ACTIVE_TIMEOUT_MS 10000
#define PAUSE_MIN_MS 1000
#define PAUSE_MAX_MS 10000
#define TAPWIN_MIN_MS 2000
#define TAPWIN_MAX_MS 10000
#define BROADCAST_MS 100

const char* AP_SSID = "Reflex";
const char* AP_PASS = "";       // открытая сеть; для пароля задай 8+ символов

/* --------------------------- СЕРВЕР --------------------------------------- */
ESP8266WebServer http(80);
WebSocketsServer ws(81);

/* --------------------------- РЕЖИМЫ / СОСТОЯНИЯ --------------------------- */
enum Mode { MODE_SPEED = 0, MODE_TAPS = 1, MODE_COUNT };
enum State { ST_IDLE, ST_REGISTERING, ST_SERIES_START, ST_ROUND_WAIT,
             ST_ROUND_ACTIVE, ST_ROUND_RESULT, ST_MSG };
State state = ST_IDLE;

/* Типы объявляем заранее: Arduino IDE генерирует прототипы функций в начале
 * файла, поэтому структуры-параметры должны быть видны раньше первой функции. */
struct Btn { uint8_t pin; bool stable, lastRead; uint32_t lastChange; bool pressedEdge, releasedEdge; };
Btn players[4], service;
struct Note { uint16_t f, d; };
struct Mel  { const Note* n; uint8_t len; };

/* --------------------- ДАННЫЕ (RAM + дублирование в LittleFS) ------------- */
#define DATA_MAGIC 0xCB03
#define NO_MS 0xFFFF
struct GameData {
  uint16_t magic;
  int16_t  score[MODE_COUNT][4];
  uint16_t bestMs[4];          // лучшая реакция (режим скорости), NO_MS = нет
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
uint32_t svcDownAt = 0; bool svcPlayerTouched = false, svcMuteDone = false;
uint32_t lastBroadcast = 0;
bool     dirty = true;

/* ===========================================================================
 *                  ЗВУК: аппаратный Timer1, чистый меандр
 *  Timer1 тикает на 5 МГц (DIV16). На частоте f переключаем вывод каждые
 *  2.5e6/f тиков -> на пине ровный прямоугольник. ISR в IRAM, высокий
 *  приоритет -> сетевая активность Wi-Fi не вызывает джиттер частоты.
 * ===========================================================================*/
volatile bool     toneActive = false;
volatile uint8_t  toneLevel  = 0;

void IRAM_ATTR onTimer1(){
  if(!toneActive) return;
  toneLevel ^= 1;
  if(toneLevel) GPOS = (1 << PIN_BUZZER);   // set
  else          GPOC = (1 << PIN_BUZZER);   // clear
}
void toneFreq(uint16_t f){
  if(f == 0){ toneActive=false; GPOC=(1<<PIN_BUZZER); return; }
  toneActive=false;                          // короткая пауза против гонки с ISR
  timer1_write(2500000UL / f);               // полупериод в тиках 5 МГц
  toneActive=true;
}

/* ----- неблокирующий проигрыватель мелодий (конечный автомат) ------------ */
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
void btnInit(Btn &b, uint8_t pin){ b.pin=pin; pinMode(pin,INPUT_PULLUP); b.stable=false; b.lastRead=false; b.lastChange=millis(); b.pressedEdge=false; b.releasedEdge=false; }
void btnUpdate(Btn &b){ b.pressedEdge=false; b.releasedEdge=false; bool raw=(digitalRead(b.pin)==LOW); uint32_t now=millis();
  if(raw!=b.lastRead){ b.lastRead=raw; b.lastChange=now; }
  if((now-b.lastChange)>=DEBOUNCE_MS && b.stable!=b.lastRead){ b.stable=b.lastRead; if(b.stable)b.pressedEdge=true; else b.releasedEdge=true; } }
void updateAllButtons(){ for(uint8_t i=0;i<4;i++) btnUpdate(players[i]); btnUpdate(service); }
bool anyPlayerHeld(){ for(uint8_t i=0;i<4;i++) if(players[i].stable) return true; return false; }

/* ===========================================================================
 *                     ТРАНСЛЯЦИЯ / ПРИЁМ ПО WebSocket
 * ===========================================================================*/
const char* sceneStr(){ switch(state){
  case ST_IDLE: return "idle"; case ST_REGISTERING: return "reg"; case ST_SERIES_START: return "series";
  case ST_ROUND_WAIT: return "wait"; case ST_ROUND_ACTIVE: return (data.mode==MODE_SPEED)?"goA":"goB";
  case ST_ROUND_RESULT: return comboShown?"combo":"result"; default: return "idle"; } }
int countdownVal(){ if(state!=ST_REGISTERING) return 0; int32_t l=REG_HOLD_MS-(int32_t)(millis()-tStateEnter); return (l<0)?0:(int)((l+999)/1000); }

void buildState(char* b, size_t n){
  uint16_t bm[4]; for(uint8_t i=0;i<4;i++) bm[i]=(data.bestMs[i]==NO_MS)?0:data.bestMs[i];
  snprintf(b,n,
    "{\"scene\":\"%s\",\"mode\":%u,\"round\":%u,\"countdown\":%d,\"winnerMask\":%u,\"reactionMs\":%lu,\"bestTaps\":%u,"
    "\"lang\":%u,\"theme\":%u,\"sound\":%u,\"soundset\":%u,"
    "\"held\":[%d,%d,%d,%d],\"isPlayer\":[%d,%d,%d,%d],\"taps\":[%u,%u,%u,%u],\"excluded\":[%d,%d,%d,%d],"
    "\"best\":[%u,%u,%u,%u],"
    "\"score0\":[%d,%d,%d,%d],\"score1\":[%d,%d,%d,%d],\"streak0\":[%u,%u,%u,%u],\"streak1\":[%u,%u,%u,%u]}",
    sceneStr(), data.mode, roundIndex, countdownVal(), winnerMask, (unsigned long)(reactionUs/1000), bestTaps,
    data.lang, data.theme, data.sound, data.soundset,
    players[0].stable,players[1].stable,players[2].stable,players[3].stable,
    isPlayer[0],isPlayer[1],isPlayer[2],isPlayer[3],
    taps[0],taps[1],taps[2],taps[3], excluded[0],excluded[1],excluded[2],excluded[3],
    bm[0],bm[1],bm[2],bm[3],
    data.score[0][0],data.score[0][1],data.score[0][2],data.score[0][3],
    data.score[1][0],data.score[1][1],data.score[1][2],data.score[1][3],
    streak[0][0],streak[0][1],streak[0][2],streak[0][3],
    streak[1][0],streak[1][1],streak[1][2],streak[1][3]);
}
void broadcast(){ if(ws.connectedClients()==0){ dirty=false; lastBroadcast=millis(); return; }
  char b[760]; buildState(b,sizeof(b)); ws.broadcastTXT(b); dirty=false; lastBroadcast=millis(); }

/* приём настроек: {"set":"theme","val":"neon"} / {"set":"mode","val":1} */
void onWsMessage(char* json){
  char key[16]={0}, sval[16]={0};
  char* ks=strstr(json,"\"set\""); char* vs=strstr(json,"\"val\"");
  if(ks){ ks=strchr(ks,':'); if(ks){ ks++; while(*ks==' '||*ks=='\"')ks++; int i=0; while(*ks&&*ks!='\"'&&*ks!=','&&*ks!='}'&&i<15) key[i++]=*ks++; key[i]=0; } }
  if(vs){ vs=strchr(vs,':'); if(vs){ vs++; while(*vs==' '||*vs=='\"')vs++; int i=0; while(*vs&&*vs!='\"'&&*vs!='}'&&*vs!=','&&i<15) sval[i++]=*vs++; sval[i]=0; } }
  if(!key[0]) return; int v=atoi(sval);
  if(!strcmp(key,"sound"))         data.sound=v?1:0;
  else if(!strcmp(key,"mode"))     data.mode=(v?MODE_TAPS:MODE_SPEED);
  else if(!strcmp(key,"soundset")){ const char* n[]={"doom","arcade","beeper","chip"}; for(uint8_t i=0;i<4;i++) if(!strcmp(sval,n[i])) data.soundset=i; }
  else if(!strcmp(key,"lang")){ const char* n[]={"ru","en","de","uk"}; for(uint8_t i=0;i<4;i++) if(!strcmp(sval,n[i])) data.lang=i; }
  else if(!strcmp(key,"theme")){ const char* n[]={"doom","fallout","gray","amber","neon"}; for(uint8_t i=0;i<5;i++) if(!strcmp(sval,n[i])) data.theme=i; }
  else return;
  saveData(); dirty=true;
}
void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t len){
  if(type==WStype_CONNECTED){ char b[760]; buildState(b,sizeof(b)); ws.sendTXT(num,b); }
  else if(type==WStype_TEXT){ payload[len]=0; onWsMessage((char*)payload); }
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

void handleServiceIdle(){
  if(service.pressedEdge){ svcDownAt=millis(); svcPlayerTouched=false; svcMuteDone=false; }
  if(service.stable){ uint32_t held=millis()-svcDownAt;
    for(uint8_t i=0;i<4;i++){ if(players[i].pressedEdge){ svcPlayerTouched=true;
        if(held>=SVC_RESET_MS){ data.score[data.mode][i]=0; streak[data.mode][i]=0; data.bestMs[i]=NO_MS; saveData(); play(EV_MODE); dirty=true; } } }
    if(!svcMuteDone && !svcPlayerTouched && held>=SVC_MUTE_MS){ data.sound=!data.sound; svcMuteDone=true; saveData(); if(data.sound)play(EV_MODE); dirty=true; } }
  if(service.releasedEdge){ uint32_t held=millis()-svcDownAt;
    if(held<SVC_SHORT_MS && !svcPlayerTouched && !svcMuteDone){ data.mode=(data.mode+1)%MODE_COUNT; saveData(); play(EV_MODE); dirty=true; } }
}

/* ===========================================================================
 *                                SETUP
 * ===========================================================================*/
void setup(){
  Serial.begin(115200); Serial.println(F("\nReflex v0.3.1"));
  for(uint8_t i=0;i<4;i++) btnInit(players[i],BTN_PIN[i]); btnInit(service,PIN_SERVICE);

  pinMode(PIN_BUZZER,OUTPUT); GPOC=(1<<PIN_BUZZER);
  timer1_isr_init(); timer1_attachInterrupt(onTimer1); timer1_enable(TIM_DIV16,TIM_EDGE,TIM_LOOP); timer1_write(2500);

  if(!LittleFS.begin()){ LittleFS.format(); LittleFS.begin(); }
  loadData();

  randomSeed(ESP.getCycleCount()^micros());

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, (AP_PASS[0]? AP_PASS : nullptr));

  http.on("/", HTTP_GET, [](){ http.sendHeader("Content-Encoding","gzip");
    http.send_P(200,"text/html",(const char*)INDEX_HTML_GZ,INDEX_HTML_GZ_LEN); });
  http.onNotFound([](){ http.sendHeader("Location","/"); http.send(302,"text/plain",""); });
  http.begin();
  ws.begin(); ws.onEvent(onWsEvent);

  play(EV_BOOT); enterState(ST_IDLE);
}

/* ===========================================================================
 *                                 LOOP
 * ===========================================================================*/
void loop(){
  http.handleClient(); ws.loop();
  updateAllButtons(); updateMelody();

  switch(state){
    case ST_IDLE:
      if(service.stable || service.releasedEdge){ handleServiceIdle(); break; }
      if(anyPlayerHeld()){ enterState(ST_REGISTERING); play(EV_ARM); }
      break;

    case ST_REGISTERING:
      if(!anyPlayerHeld()){ enterState(ST_IDLE); break; }
      if((millis()-tStateEnter)>=REG_HOLD_MS){ uint8_t cnt=0;
        for(uint8_t i=0;i<4;i++){ isPlayer[i]=players[i].stable; if(isPlayer[i])cnt++; }
        randomSeed(ESP.getCycleCount()^micros());
        if(cnt>=2) startSeries(); else { play(EV_FALSE); enterState(ST_IDLE); } }
      else if((millis()-lastBroadcast)>=BROADCAST_MS) dirty=true;   // живой отсчёт
      break;

    case ST_SERIES_START: { bool allRel=true; for(uint8_t i=0;i<4;i++) if(isPlayer[i]&&players[i].stable) allRel=false;
      if(allRel || (millis()-tStateEnter)>4000) beginRound(); } break;

    case ST_ROUND_WAIT:
      for(uint8_t i=0;i<4;i++){ if(!isPlayer[i]) continue;
        if(players[i].pressedEdge){
          if(data.mode==MODE_SPEED){ if(!falseStarted[i]){ falseStarted[i]=true; data.score[data.mode][i]-=1; saveData(); play(EV_FALSE); dirty=true; } }
          else { if(!excluded[i]){ excluded[i]=true; play(EV_FALSE); dirty=true; } } } }
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
      if(comboShown && !comboFlashed && (millis()-tStateEnter)>1400 && !melodyPlaying){ comboFlashed=true; play(EV_COMBO); dirty=true; }
      if((millis()-tStateEnter)>=RESULT_SHOW_MS){ comboFlashed=false; roundIndex++;
        if(roundIndex<ROUNDS_PER_SERIES) beginRound(); else enterState(ST_IDLE); } } break;

    case ST_MSG: if((millis()-tStateEnter)>=1600) enterState(ST_IDLE); break;
  }

  if(dirty || (millis()-lastBroadcast)>=BROADCAST_MS) broadcast();
}
