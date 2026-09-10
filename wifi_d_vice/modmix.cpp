#include "modmix.h"
#include <math.h>

#define FRAC        14
#define PAULA_PAL   7093789.2f
#define MAXPAT      64

struct Smp {
  uint32_t off;        // byte offset of PCM into ram[]
  uint32_t len;
  uint32_t loopStart, loopLen;
  int      vol;        // 0..64
  int      finetune;   // -8..7
};

struct Chan {
  const int8_t *smp;
  uint32_t smpEnd;     // playback end (loopStart+loopLen if looping, else len)
  uint32_t loopStart, loopLen;
  uint64_t pos, inc;
  int  vol;
  int  period;         // current base period
  int  playPeriod;     // period actually feeding the increment this tick
  int  finetune;
  bool active;
  // effect state
  int  fx, param;
  int  portaTarget, portaMem;
  int  slideMem;       // 1xx/2xx memory
  int  vibPos, vibSpeed, vibDepth;
  int  offsetMem;
  int  arpX, arpY;
};

// Points straight at the (flash-resident) MOD -- no RAM copy, so a 230KB
// song is fine. ESP32 maps .rodata; 8-bit reads are direct + cached.
static const uint8_t *mod = nullptr;
static size_t   modLen = 0;
static bool     ok = false;

static Smp   smp[31];
static int   numCh = 4;
static int   numPat = 0;
static int   songLen = 0;
static uint8_t order[128];
static uint32_t patBase;      // = 1084

static int   rate = 22050;
static double tickSamples;    // samples per ProTracker tick
static double toNextTick;

static int   speed = 6;       // ticks per row
static int   tempo = 125;
static int   curOrder, curRow, tick;
static bool  pendJump, pendBreak;
static int   jumpTo, breakRow;

static Chan  ch[4];

static inline int rd8(uint32_t o) { return mod[o]; }
static inline int rd16be(uint32_t o) { return (mod[o] << 8) | mod[o + 1]; }

static void recalcTick() {
  int t = (tempo < 32) ? 125 : tempo;
  tickSamples = (double)rate * (2500.0 / t) / 1000.0;
}

void modmixSetRate(int hz) { rate = hz; recalcTick(); }

void modmixFree() {
  mod = nullptr; modLen = 0; ok = false;
}

bool modmixLoad(const uint8_t *data, size_t len) {
  modmixFree();
  if (len < 1084 + 64 * 16) return false;
  mod = data;
  modLen = len;

  // 31 sample headers at offset 20, 30 bytes each
  for (int i = 0; i < 31; i++) {
    uint32_t h = 20 + i * 30;
    smp[i].len       = (uint32_t)rd16be(h + 22) * 2;
    int ft           = mod[h + 24] & 0x0F;
    smp[i].finetune  = (ft >= 8) ? ft - 16 : ft;
    smp[i].vol       = mod[h + 25]; if (smp[i].vol > 64) smp[i].vol = 64;
    smp[i].loopStart = (uint32_t)rd16be(h + 26) * 2;
    smp[i].loopLen   = (uint32_t)rd16be(h + 28) * 2;
  }

  songLen = mod[950];
  if (songLen < 1 || songLen > 128) songLen = 128;
  memcpy(order, mod + 952, 128);

  const uint8_t tag[4] = { mod[1080], mod[1081], mod[1082], mod[1083] };
  numCh = 4;
  if (tag[0] == '6' && tag[1] == 'C') numCh = 6;
  else if (tag[0] == '8' && tag[1] == 'C') numCh = 8;
  else if (memcmp(tag, "OCTA", 4) == 0 || memcmp(tag, "CD81", 4) == 0 || memcmp(tag, "FLT8", 4) == 0) numCh = 8;
  if (numCh < 1) numCh = 4;

  int maxp = 0;
  for (int i = 0; i < songLen; i++) if (order[i] > maxp) maxp = order[i];
  numPat = maxp + 1;
  patBase = 1084;

  uint32_t sd = patBase + (uint32_t)numPat * 64 * numCh * 4;
  for (int i = 0; i < 31; i++) {
    smp[i].off = sd;
    // clamp a declared length that runs past the file
    if (sd + smp[i].len > len) smp[i].len = (sd < len) ? (len - sd) : 0;
    if (smp[i].loopStart + smp[i].loopLen > smp[i].len) smp[i].loopLen = 0;
    sd += smp[i].len;
  }

  recalcTick();
  ok = true;
  return true;
}

static float finetunedPeriod(int period, int ft) {
  if (period <= 0) return 0;
  return period * powf(2.0f, -ft / 96.0f);   // ~ProTracker finetune, analytic
}

static void chNote(Chan &c, int period, bool porta) {
  int p = (int)(finetunedPeriod(period, c.finetune) + 0.5f);
  if (porta) { c.portaTarget = p; return; }
  c.period = p;
  c.pos = 0;
  c.active = true;
}

static void recomputeInc() {
  for (int i = 0; i < 4; i++) {
    Chan &c = ch[i];
    int per = c.playPeriod > 0 ? c.playPeriod : c.period;
    if (per < 27) per = 27;
    float hz = PAULA_PAL / (2.0f * per);
    c.inc = (uint64_t)((double)hz / rate * (1u << FRAC));
  }
}

static void procRow() {
  pendJump = pendBreak = false;
  int pat = order[curOrder];
  uint32_t rowp = patBase + ((uint32_t)pat * 64 + curRow) * (numCh * 4);

  for (int i = 0; i < 4; i++) {
    Chan &c = ch[i];
    const uint8_t *e = mod + rowp + i * 4;
    int smpNum = (e[0] & 0xF0) | (e[2] >> 4);
    int period = ((e[0] & 0x0F) << 8) | e[1];
    int fx = e[2] & 0x0F;
    int prm = e[3];
    c.fx = fx; c.param = prm;
    c.playPeriod = 0;

    if (smpNum >= 1 && smpNum <= 31) {
      Smp &s = smp[smpNum - 1];
      c.smp = (const int8_t *)(mod + s.off);
      c.loopStart = s.loopStart;
      c.loopLen   = s.loopLen;
      c.smpEnd    = (s.loopLen > 2) ? (s.loopStart + s.loopLen) : s.len;
      c.vol       = s.vol;
      c.finetune  = s.finetune;
    }

    bool porta = (fx == 3 || fx == 5);
    if (period) chNote(c, period, porta);

    switch (fx) {
      case 0x0: c.arpX = prm >> 4; c.arpY = prm & 0x0F; break;
      case 0x1: case 0x2: if (prm) c.slideMem = prm; break;
      case 0x3: if (prm) c.portaMem = prm; break;
      case 0x4: case 0x6:
        if (prm >> 4)   c.vibSpeed = prm >> 4;
        if (prm & 0x0F) c.vibDepth = prm & 0x0F;
        break;
      case 0x9:
        if (prm) c.offsetMem = prm;
        if (period) { c.pos = ((uint64_t)c.offsetMem * 256) << FRAC; }
        break;
      case 0xA: break;                         // vol slide -- handled per tick
      case 0xB: pendJump = true; jumpTo = prm; break;
      case 0xC: c.vol = prm > 64 ? 64 : prm; break;
      case 0xD: pendBreak = true; breakRow = (prm >> 4) * 10 + (prm & 0x0F); if (breakRow > 63) breakRow = 0; break;
      case 0xF:
        if (prm == 0) {}
        else if (prm < 0x20) speed = prm;
        else { tempo = prm; recalcTick(); }
        break;
      default: break;
    }
  }
}

static void procTick(int t) {
  for (int i = 0; i < 4; i++) {
    Chan &c = ch[i];
    c.playPeriod = c.period;
    int fx = c.fx, prm = c.param;

    if (fx == 0x0 && (c.arpX || c.arpY)) {
      int n = (t % 3 == 0) ? 0 : (t % 3 == 1) ? c.arpX : c.arpY;
      if (n) c.playPeriod = (int)(c.period * powf(0.5f, n / 12.0f) + 0.5f);
    } else if (t > 0 && fx == 0x1) {
      c.period -= c.slideMem * 4; if (c.period < 113) c.period = 113;
      c.playPeriod = c.period;
    } else if (t > 0 && fx == 0x2) {
      c.period += c.slideMem * 4; if (c.period > 856) c.period = 856;
      c.playPeriod = c.period;
    } else if (t > 0 && (fx == 0x3 || fx == 0x5) && c.portaTarget) {
      int step = c.portaMem * 4;
      if (c.period < c.portaTarget) { c.period += step; if (c.period > c.portaTarget) c.period = c.portaTarget; }
      else if (c.period > c.portaTarget) { c.period -= step; if (c.period < c.portaTarget) c.period = c.portaTarget; }
      c.playPeriod = c.period;
    } else if ((fx == 0x4 || fx == 0x6) && c.vibDepth) {
      if (t > 0) c.vibPos = (c.vibPos + c.vibSpeed) & 63;
      float d = sinf(c.vibPos * 0.09817477f) * c.vibDepth * 2.0f;
      c.playPeriod = c.period + (int)d;
    }

    if (t > 0 && (fx == 0xA || fx == 0x5 || fx == 0x6)) {
      int x = prm >> 4, y = prm & 0x0F;
      if (x) c.vol += x; else c.vol -= y;
      if (c.vol < 0) c.vol = 0; if (c.vol > 64) c.vol = 64;
    }
  }
  recomputeInc();
}

static void advanceRow() {
  if (pendJump) {
    curOrder = jumpTo;
    curRow   = pendBreak ? breakRow : 0;
  } else if (pendBreak) {
    curOrder++;
    curRow = breakRow;
  } else if (++curRow > 63) {
    curRow = 0;
    curOrder++;
  }
  if (curOrder >= songLen) curOrder = 0;
}

static void doTick() {
  if (tick == 0) procRow();
  procTick(tick);
  if (++tick >= (speed < 1 ? 6 : speed)) { tick = 0; advanceRow(); }
}

void modmixStart() {
  if (!ok) return;
  speed = 6; tempo = 125; recalcTick();
  curOrder = curRow = tick = 0;
  toNextTick = 0;
  for (int i = 0; i < 4; i++) {
    Chan &c = ch[i];
    memset(&c, 0, sizeof(c));
    c.vibSpeed = 0; c.vibDepth = 0;
  }
}

static inline uint8_t mixOne() {
  int32_t acc = 0;
  for (int i = 0; i < 4; i++) {
    Chan &c = ch[i];
    if (!c.active || !c.smp || c.inc == 0) continue;
    uint32_t ip = (uint32_t)(c.pos >> FRAC);
    if (ip >= c.smpEnd) { c.active = false; continue; }
    int s0 = c.smp[ip];
    uint32_t nip = ip + 1;
    int s1 = (nip < c.smpEnd) ? c.smp[nip]
             : (c.loopLen > 2 ? c.smp[c.loopStart] : s0);
    int fr = (int)(c.pos & ((1u << FRAC) - 1));
    int s = s0 + (((s1 - s0) * fr) >> FRAC);
    acc += s * c.vol;
    c.pos += c.inc;
    if ((uint32_t)(c.pos >> FRAC) >= c.smpEnd) {
      if (c.loopLen > 2) {
        uint64_t loopBytes = (uint64_t)c.loopLen << FRAC;
        while ((uint32_t)(c.pos >> FRAC) >= c.smpEnd) c.pos -= loopBytes;
      } else {
        c.active = false;
      }
    }
  }
  int v = (acc >> 8) + 128;                 // acc max ~4*128*64=32768 -> +/-128
  if (v < 0) v = 0; else if (v > 255) v = 255;
  return (uint8_t)v;
}

void modmixRender(uint8_t *out, int n) {
  if (!ok) { memset(out, 128, n); return; }
  int i = 0;
  while (i < n) {
    if (toNextTick <= 0.0) {
      doTick();
      toNextTick += tickSamples;
    }
    int chunk = n - i;
    int avail = (int)toNextTick; if (avail < 1) avail = 1;
    if (chunk > avail) chunk = avail;
    for (int k = 0; k < chunk; k++) out[i++] = mixOne();
    toNextTick -= chunk;
  }
}
