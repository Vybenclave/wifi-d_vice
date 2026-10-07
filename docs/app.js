'use strict';
/* WIFI D_VICE — browser simulator.
   A from-scratch re-implementation of the device's menu tree and screen
   layouts (see wifi_d_vice.ino / *.cpp in the firmware repo) on an HTML
   canvas, with fabricated data standing in for real radio scans. Nothing
   here talks to real hardware or the network beyond loading its own assets.
*/

// ---------------------------------------------------------------- geometry
const LANDSCAPE = { w: 320, h: 240 };
const PORTRAIT  = { w: 240, h: 320 };
let SCR = { ...PORTRAIT };
let isLandscape = false;

const canvas = document.getElementById('tft');
const ctx = canvas.getContext('2d');
const deviceEl = document.getElementById('device');
const ledEl = document.getElementById('statusLed');

function resizeCanvas() {
  const dpr = Math.max(1, window.devicePixelRatio || 1);
  const maxW = Math.min(window.innerWidth - 56, 560);
  const maxH = window.innerHeight * 0.62;
  let scale = Math.min(maxW / SCR.w, maxH / SCR.h);
  scale = Math.max(0.9, Math.min(scale, 2.4));
  canvas.style.width = Math.round(SCR.w * scale) + 'px';
  canvas.style.height = Math.round(SCR.h * scale) + 'px';
  canvas.width = Math.round(SCR.w * scale * dpr);
  canvas.height = Math.round(SCR.h * scale * dpr);
  ctx.setTransform(scale * dpr, 0, 0, scale * dpr, 0, 0);
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
}
window.addEventListener('resize', resizeCanvas);

// ------------------------------------------------------------------ color
const C = {
  BLACK: '#000000', WHITE: '#ffffff', RED: '#ff0000', GREEN: '#00ff00',
  CYAN: '#00ffff', YELLOW: '#ffff00', MAGENTA: '#ff00ff', ORANGE: '#ffa500',
  DARKGREY: '#7b7d7b', LIGHTGREY: '#c5c2c5', NAVY: '#00007b', BLUE: '#0000ff',
  PURPLE: '#7b007b', OLIVE: '#7b7d00', MAROON: '#7b0000', PINK: '#ff81c5',
  GREENYELLOW: '#acff29',
  NS_DOWN: '#7bceff', NS_UP: '#ff69d5', NS_GRID: '#414041', NS_LABEL: '#b4bae6',
  PEAK_DIM: '#626162', GRID_DIM: '#202020', DIM_GREY: '#838183',
};

const ACCENT = [
  { name: 'Cyan',    fill: '#29cabd', edge: '#08594a', bevel: '#8bf6ee', emboss: '#aceede', titleBar: '#00007b', label: '#00ffff' },
  { name: 'Amber',   fill: '#ffae00', edge: '#5a4000', bevel: '#ffe29c', emboss: '#ffeabd', titleBar: '#312000', label: '#ffae00' },
  { name: 'Green',   fill: '#31ff31', edge: '#105d10', bevel: '#acffac', emboss: '#c5ffc5', titleBar: '#002800', label: '#31ff31' },
  { name: 'Grey',    fill: '#b4bec5', edge: '#73757b', bevel: '#eef2f6', emboss: '#f6faff', titleBar: '#202829', label: '#dee2e6' },
  { name: 'Red',     fill: '#ff2829', edge: '#5a1010', bevel: '#ffaeac', emboss: '#ffcac5', titleBar: '#310808', label: '#ff2829' },
  { name: 'Orange',  fill: '#ff6500', edge: '#5a2400', bevel: '#ffc29c', emboss: '#ffd6bd', titleBar: '#311000', label: '#ff6500' },
  { name: 'Yellow',  fill: '#ffda00', edge: '#5a5000', bevel: '#fff29c', emboss: '#fff6bd', titleBar: '#312800', label: '#ffda00' },
  { name: 'Lime',    fill: '#acff29', edge: '#395d10', bevel: '#deffac', emboss: '#e6ffc5', titleBar: '#202c08', label: '#acff29' },
  { name: 'Blue',    fill: '#2981ff', edge: '#10305a', bevel: '#acceff', emboss: '#c5deff', titleBar: '#081831', label: '#2981ff' },
  { name: 'Indigo',  fill: '#5a44e6', edge: '#201852', bevel: '#bdbaf6', emboss: '#d5d2f6', titleBar: '#100c29', label: '#5a44e6' },
  { name: 'Purple',  fill: '#ac3cde', edge: '#391452', bevel: '#deb6ee', emboss: '#e6cef6', titleBar: '#200c29', label: '#ac3cde' },
  { name: 'Magenta', fill: '#e628c5', edge: '#52104a', bevel: '#f6aee6', emboss: '#f6caee', titleBar: '#290820', label: '#e628c5' },
  { name: 'Pink',    fill: '#ff6dac', edge: '#5a2839', bevel: '#ffc6de', emboss: '#ffdae6', titleBar: '#311420', label: '#ff6dac' },
  { name: 'Sky',     fill: '#5ac6ff', edge: '#20485a', bevel: '#bdeaff', emboss: '#d5f2ff', titleBar: '#102431', label: '#5ac6ff' },
  { name: 'White',   fill: '#e6eaee', edge: '#52555a', bevel: '#f6f6f6', emboss: '#f6faff', titleBar: '#292829', label: '#e6eaee' },
  { name: 'Rose',    fill: '#c51439', edge: '#4a0818', bevel: '#e6a5b4', emboss: '#eec2cd', titleBar: '#200408', label: '#c51439' },
];
const TH_GRAD_TOP = '#080820', TH_GRAD_BOT = '#621c7b';

// -------------------------------------------------------------- persisted
const store = {
  get(k, d) { try { const v = localStorage.getItem('dvice_' + k); return v === null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem('dvice_' + k, JSON.stringify(v)); } catch {} },
};
let THEME = store.get('theme', 1);       // 0 = Basic, 1 = Vice
let ACCENT_ID = store.get('accent', 0);
let LIST_BG = store.get('listbg', true);
let ROTATION = store.get('rotation', 1); // 0/1/2/3 like the real uiSetRotation -- 1 = 90 deg, portrait
let TZ_24H = store.get('tz24h', false);
let TZ_AUTODST = store.get('tzdst', true);
let TZ_IDX = store.get('tzidx', 4); // index into TZLIST below
let SPLASH_ON = store.get('splash', true);
let BEEP_VOL = store.get('beepvol', 70);
let DISPLAY_TIMEOUT_MS = store.get('dispTimeoutMs', 0); // 0 = Never, like the real firmware's default

function themeIsVice() { return THEME === 1; }
function accent() { return ACCENT[ACCENT_ID]; }
function accentLabel() { return accent().label; }
function accentFill() { return accent().fill; }
function accentEdge() { return accent().edge; }
function accentBevel() { return accent().bevel; }
function accentTitleBar() { return accent().titleBar; }

// ------------------------------------------------------------------ sprintf
// Minimal printf: %d %u %ld %lu %s %c %x %X %f (with .N precision) %% and
// -/0/width flags — enough to cover every format string in the firmware.
function sprintf(fmt, ...args) {
  let i = 0;
  return String(fmt).replace(/%(-)?(0)?(\d+)?(?:\.(\d+))?(l)?([sdiufxXc%])/g,
    (m, minus, zero, width, prec, _l, conv) => {
      if (conv === '%') return '%';
      let a = args[i++];
      width = width ? parseInt(width, 10) : 0;
      let out;
      if (conv === 's') {
        out = a === undefined ? '' : String(a);
        if (prec !== undefined) out = out.slice(0, parseInt(prec, 10));
      } else if (conv === 'c') {
        out = typeof a === 'number' ? String.fromCharCode(a) : String(a);
      } else if (conv === 'f') {
        const p = prec !== undefined ? parseInt(prec, 10) : 6;
        out = Number(a).toFixed(p);
      } else if (conv === 'x' || conv === 'X') {
        out = (Number(a) >>> 0).toString(16);
        if (conv === 'X') out = out.toUpperCase();
      } else { // d i u
        out = String(Math.trunc(Number(a)));
      }
      if (width && out.length < width) {
        const pad = (zero && !minus && 'sc'.indexOf(conv) < 0) ? '0' : ' ';
        out = minus ? out + pad.repeat(width - out.length) : pad.repeat(width - out.length) + out;
      }
      return out;
    });
}

// ---------------------------------------------------------------- tft shim
const UI_TOPBAR_H = 28, UI_STATUSBAR_H = 18;
const UI_ACTIONROW_Y = UI_TOPBAR_H + 1, UI_ACTIONROW_H = 26;
const UI_CONTENT_Y_PLAIN = UI_TOPBAR_H + 1;
const UI_CONTENT_Y = UI_ACTIONROW_Y + UI_ACTIONROW_H + 1;
const UI_PAGER_H = 26;
const UI_RIGHTZONE_W = 96;

function roundRectPath(x, y, w, h, r) {
  r = Math.min(r, w / 2, h / 2);
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

const tft = {
  _size: 1, _color: C.WHITE, _cx: 0, _cy: 0, _wrap: true,
  width() { return SCR.w; },
  height() { return SCR.h; },
  setTextSize(n) { this._size = n; },
  setTextColor(c) { this._color = c; },
  setTextWrap(b) { this._wrap = b; },
  setCursor(x, y) { this._cx = x; this._cy = y; },
  getCursorX() { return this._cx; },
  getCursorY() { return this._cy; },
  cellW() { return 6 * this._size; },
  cellH() { return 8 * this._size; },
  // VT323 renders visually taller than its nominal CSS px size (it has
  // loose internal ascent/descent metrics) -- these are deliberately well
  // under the 8px/size cell math the layout offsets assume, so size-2/3
  // text doesn't bleed into the line below it.
  fontPx() { return [0, 11, 16, 22, 28][this._size] || 11 * this._size; },
  _font() { return `${this.fontPx()}px 'VT323', monospace`; },
  textWidthOf(s) { ctx.font = this._font(); return ctx.measureText(s).width; },
  fillScreen(c) { ctx.fillStyle = c; ctx.fillRect(0, 0, SCR.w, SCR.h); },
  fillRect(x, y, w, h, c) { ctx.fillStyle = c; ctx.fillRect(x, y, w, h); },
  fillRoundRect(x, y, w, h, r, c) { roundRectPath(x, y, w, h, r); ctx.fillStyle = c; ctx.fill(); },
  drawRoundRect(x, y, w, h, r, c) { roundRectPath(x, y, w, h, r); ctx.strokeStyle = c; ctx.lineWidth = 1; ctx.stroke(); },
  drawRect(x, y, w, h, c) { ctx.strokeStyle = c; ctx.lineWidth = 1; ctx.strokeRect(x + 0.5, y + 0.5, w - 1, h - 1); },
  fillCircle(x, y, r, c) { ctx.beginPath(); ctx.arc(x, y, r, 0, Math.PI * 2); ctx.fillStyle = c; ctx.fill(); },
  drawCircle(x, y, r, c) { ctx.beginPath(); ctx.arc(x, y, r, 0, Math.PI * 2); ctx.strokeStyle = c; ctx.lineWidth = 1; ctx.stroke(); },
  drawFastHLine(x, y, w, c) { ctx.strokeStyle = c; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(x, y + 0.5); ctx.lineTo(x + w, y + 0.5); ctx.stroke(); },
  drawFastVLine(x, y, h, c) { ctx.strokeStyle = c; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(x + 0.5, y); ctx.lineTo(x + 0.5, y + h); ctx.stroke(); },
  drawLine(x0, y0, x1, y1, c) { ctx.strokeStyle = c; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(x0 + 0.5, y0 + 0.5); ctx.lineTo(x1 + 0.5, y1 + 0.5); ctx.stroke(); },
  _emit(s) {
    ctx.font = this._font();
    ctx.textBaseline = 'top';
    ctx.fillStyle = this._color;
    ctx.fillText(s, this._cx, this._cy - Math.round(this.fontPx() * 0.16));
    this._cx += ctx.measureText(s).width;
  },
  print(s) { this._emit(String(s)); },
  println(s) { if (s !== undefined) this._emit(String(s)); this._cx = 0; this._cy += this.cellH(); },
  printf(fmt, ...a) { this._emit(sprintf(fmt, ...a)); },
};

// --------------------------------------------------------------- assets
const ASSETS = {};
function loadImg(name, src) {
  return new Promise((res) => {
    const im = new Image();
    im.onload = () => { ASSETS[name] = im; res(im); };
    im.onerror = () => res(null);
    im.src = src;
  });
}
const assetsReady = Promise.all([
  loadImg('bgLandscape', 'assets/bg_landscape.png'),
  loadImg('bgPortrait',  'assets/bg_portrait.png'),
  loadImg('splashLandscape', 'assets/splash_landscape.png'),
  loadImg('splashPortrait',  'assets/splash_portrait.png'),
]);

// ------------------------------------------------------------- background
let bgMode = 0; // 0 = BLACK/gradient, 1 = IMAGE
function uiSetBgMode(m) { bgMode = m; }
function uiBgColorAt(y) {
  if (!themeIsVice()) return C.BLACK;
  const f = Math.max(0, Math.min(1, y / SCR.h));
  return lerpHex(TH_GRAD_TOP, TH_GRAD_BOT, f);
}
function lerpHex(a, b, f) {
  const pa = hexToRgb(a), pb = hexToRgb(b);
  const r = Math.round(pa.r + (pb.r - pa.r) * f);
  const g = Math.round(pa.g + (pb.g - pa.g) * f);
  const bch = Math.round(pa.b + (pb.b - pa.b) * f);
  return `rgb(${r},${g},${bch})`;
}
function hexToRgb(h) {
  const n = parseInt(h.slice(1), 16);
  return { r: (n >> 16) & 255, g: (n >> 8) & 255, b: n & 255 };
}
function uiClearRect(x, y, w, h) {
  if (!themeIsVice()) { ctx.fillStyle = C.BLACK; ctx.fillRect(x, y, w, h); return; }
  if (bgMode === 1) {
    const img = isLandscape ? ASSETS.bgLandscape : ASSETS.bgPortrait;
    if (img) {
      ctx.save();
      ctx.beginPath(); ctx.rect(x, y, w, h); ctx.clip();
      ctx.filter = LIST_BG || menuUsesImage ? 'blur(1.1px)' : 'none';
      ctx.drawImage(img, 0, 0, SCR.w, SCR.h);
      ctx.filter = 'none';
      ctx.restore();
      // dim overlay to match the ~55% dimmed source art's readability over text
      ctx.fillStyle = 'rgba(0,0,0,0.12)';
      ctx.fillRect(x, y, w, h);
      return;
    }
  }
  // gradient fallback / default
  const grad = ctx.createLinearGradient(0, 0, 0, SCR.h);
  grad.addColorStop(0, TH_GRAD_TOP);
  grad.addColorStop(1, TH_GRAD_BOT);
  ctx.fillStyle = grad;
  ctx.fillRect(x, y, w, h);
}
let menuUsesImage = true; // menu/button screens always use the image, per firmware
function uiClearBelow(y0) {
  uiClearRect(0, y0, SCR.w, SCR.h - y0);
  uiDrawStatusBar();
}

// ------------------------------------------------------------------ chrome
let toastMsg = '', toastAt = 0;
function uiToast(msg) { toastMsg = msg; toastAt = performance.now(); }
function uiClearToast() { toastMsg = ''; }

function uiDrawStatusBar() {
  const y = SCR.h - UI_STATUSBAR_H;
  ctx.fillStyle = C.BLACK; ctx.fillRect(0, y, SCR.w, UI_STATUSBAR_H);
  tft.drawFastHLine(0, y, SCR.w, C.WHITE);
  // toast (left) — clipped before the clock/battery zone
  if (toastMsg) {
    tft.setTextSize(1); tft.setTextColor(C.WHITE);
    tft.setCursor(3, y + 5);
    let msg = toastMsg;
    const maxW = SCR.w - UI_RIGHTZONE_W - 6;
    while (tft.textWidthOf(msg) > maxW && msg.length > 1) msg = msg.slice(0, -1);
    tft.print(msg);
  }
  // clock
  tft.setTextSize(1);
  tft.setTextColor(themeIsVice() ? accentLabel() : C.WHITE);
  const timeStr = clockString();
  const tw = tft.textWidthOf(timeStr);
  tft.setCursor(SCR.w - UI_RIGHTZONE_W + 34 - tw / 2, y + 5);
  tft.print(timeStr);
  // battery glyph
  drawBattery(SCR.w - 30, y + 3, 22, 12);
}
function clockString() {
  const now = new Date(simClockMs());
  let h = now.getHours(), m = now.getMinutes();
  let suffix = '';
  if (!TZ_24H) { suffix = h >= 12 ? 'p' : 'a'; h = h % 12; if (h === 0) h = 12; }
  return `${String(h).padStart(TZ_24H ? 2 : 1, '0')}:${String(m).padStart(2, '0')}${suffix}`;
}
let batteryPct = 78, batteryDir = -1;
function drawBattery(x, y, w, h) {
  ctx.strokeStyle = C.WHITE; ctx.lineWidth = 1;
  ctx.strokeRect(x + 0.5, y + 0.5, w - 3, h - 1);
  ctx.fillStyle = C.WHITE; ctx.fillRect(x + w - 2, y + 3, 2, h - 6);
  const innerW = w - 6;
  const fillW = Math.round(innerW * (batteryPct / 100));
  const col = batteryPct > 60 ? C.GREEN : batteryPct > 25 ? C.YELLOW : C.RED;
  ctx.fillStyle = col;
  ctx.fillRect(x + 2, y + 2, fillW, h - 4);
}

function uiDrawTopBar(title) {
  bgMode = LIST_BG ? 1 : 0;
  ctx.fillStyle = accentTitleBar();
  ctx.fillRect(0, 0, SCR.w, UI_TOPBAR_H);
  tft.drawFastHLine(0, UI_TOPBAR_H, SCR.w, C.WHITE);
  uiDrawStatusBar();
  uiDrawButton({ x: 0, y: 0, w: 60, h: UI_TOPBAR_H, label: '<<<' });
  tft.setTextColor(C.WHITE);
  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setCursor(66, 6);
  tft.print(title);
}

function contrastTextFor(hex) {
  const { r, g, b } = hexToRgb(hex);
  const luma = 0.299 * r + 0.587 * g + 0.114 * b;
  return luma > 140 ? '#0b0a14' : '#ffffff';
}
const UI_MENU_BTN_MAXSIZE = 2;
function fitTextSize(label, w, h) {
  for (let s = UI_MENU_BTN_MAXSIZE; s >= 1; s--) {
    tft.setTextSize(s);
    if (tft.textWidthOf(label) <= w - 10 && s * 8 <= h - 4) return s;
  }
  return 1;
}
function uiDrawButton(b, opts = {}) {
  const vice = themeIsVice();
  if (vice) {
    tft.fillRoundRect(b.x, b.y, b.w, b.h, 8, accentFill());
    tft.drawRoundRect(b.x, b.y, b.w, b.h, 8, accentEdge());
    tft.drawFastHLine(b.x + 4, b.y + 2, b.w - 8, accentBevel());
  } else {
    tft.drawRect(b.x, b.y, b.w, b.h, opts.color || C.WHITE);
  }
  const s = fitTextSize(b.label, b.w, b.h);
  tft.setTextSize(s);
  const tw = tft.textWidthOf(b.label);
  tft.setTextColor(vice ? (opts.textColor || '#232041') : (opts.color || C.WHITE));
  tft.setCursor(b.x + (b.w - tw) / 2, b.y + (b.h - tft.cellH()) / 2);
  tft.print(b.label);
}
function uiDrawMenuButton(b) { uiDrawButton(b); }
function uiDrawButtonDim(b) {
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 8, '#2c2a3d');
  tft.drawRoundRect(b.x, b.y, b.w, b.h, 8, '#1a1926');
  const s = fitTextSize(b.label, b.w, b.h);
  tft.setTextSize(s);
  const tw = tft.textWidthOf(b.label);
  tft.setTextColor(C.DIM_GREY);
  tft.setCursor(b.x + (b.w - tw) / 2, b.y + (b.h - tft.cellH()) / 2);
  tft.print(b.label);
}
function uiDrawActionRow(btns) {
  const n = btns.length, gap = 4;
  const w = Math.floor((SCR.w - gap * (n + 1)) / n);
  btns.forEach((b, i) => {
    b.x = gap + i * (w + gap); b.y = UI_ACTIONROW_Y; b.w = w; b.h = UI_ACTIONROW_H;
    uiDrawMenuButton(b);
  });
  return btns;
}
function uiDrawPager(y, page, pages, prevBtn, nextBtn) {
  prevBtn.x = 8; prevBtn.y = y; prevBtn.w = 70; prevBtn.h = UI_PAGER_H; prevBtn.label = '< prev';
  nextBtn.x = SCR.w - 78; nextBtn.y = y; nextBtn.w = 70; nextBtn.h = UI_PAGER_H; nextBtn.label = 'next >';
  if (page > 0) uiDrawMenuButton(prevBtn); else uiDrawButtonDim(prevBtn);
  if (page < pages - 1) uiDrawMenuButton(nextBtn); else uiDrawButtonDim(nextBtn);
  tft.setTextSize(1); tft.setTextColor(C.WHITE); tft.setTextWrap(false);
  const s = `${page + 1} / ${pages}`;
  tft.setCursor(SCR.w / 2 - tft.textWidthOf(s) / 2, y + 9);
  tft.print(s);
}
function hit(t, b) { return b && t.x >= b.x && t.x < b.x + b.w && t.y >= b.y && t.y < b.y + b.h; }

// -------------------------------------------------------------- sim clock
const BOOT_MS = Date.now() - 3 * 60 * 1000; // pretend it's been up a few minutes
function simClockMs() { return Date.now(); }
function fmtHM(ms) { const d = new Date(ms); return `${String(d.getHours()).padStart(2,'0')}:${String(d.getMinutes()).padStart(2,'0')}`; }
function fmtMMSS(sinceMs) {
  const s = Math.max(0, Math.floor((performance.now() - sinceMs) / 1000));
  return `${String(Math.floor(s / 60)).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`;
}

// ---------------------------------------------------------- fake data pool
const rnd = (a, b) => a + Math.random() * (b - a);
const rndi = (a, b) => Math.floor(rnd(a, b + 1));
const pick = (arr) => arr[rndi(0, arr.length - 1)];
const hex2 = (n) => n.toString(16).toUpperCase().padStart(2, '0');
function randMac(oui) {
  const o = oui || [hex2(rndi(0,255)), hex2(rndi(0,255)), hex2(rndi(0,255))];
  return [...o, hex2(rndi(0,255)), hex2(rndi(0,255)), hex2(rndi(0,255))].join(':');
}
function jitterRssi(base, amt = 3) { return Math.max(-95, Math.min(-30, Math.round(base + rnd(-amt, amt)))); }

const SSID_POOL = ['HomeNetwork_5G','CoffeeShop-WiFi','NETGEAR73','xfinitywifi','ATT-9k2Wq','Neighbor-2.4G','PrintServer_IoT','OfficeAP-East','TP-Link_9C21','FBI Surveillance Van','Pixel_9214','Marriott_GUEST','(hidden)'];
const VENDOR_POOL = ['Apple','Samsung','Espressif','Intel','TP-Link','Netgear','Sonos','Amazon','Google','Ubiquiti','Xiaomi','Ruckus'];
const ENC_POOL = ['open','WEP','WPA','WPA2','WPA/WPA2','WPA2-Ent','WPA3','WPA2/WPA3','OWE'];
const BLE_NAME_POOL = ['Flipper Devon','Ray-Ban Meta','(no name)','Galaxy Buds2','JBL Flip 6','LE-Bose-QC35','Pixel Buds Pro','Tesla Model 3','Garmin Fenix','MX Anywhere 3'];

function fakeAp(i) {
  return {
    ssid: SSID_POOL[i % SSID_POOL.length],
    bssid: randMac(),
    channel: pick([1,6,11,36,40,44,149,157]),
    rssiBase: rndi(-85, -35),
    enc: pick(ENC_POOL),
    vendor: pick(VENDOR_POOL),
  };
}
const AP_POOL = Array.from({ length: 9 }, (_, i) => fakeAp(i));
function tickAps() { AP_POOL.forEach(a => a.rssi = jitterRssi(a.rssiBase)); }
tickAps();

// ---------------------------------------------------------------- input
let touch = { pressed: false, isNewPress: false, x: 0, y: 0 };
function clientToLogical(clientX, clientY) {
  const r = canvas.getBoundingClientRect();
  return { x: (clientX - r.left) / r.width * SCR.w, y: (clientY - r.top) / r.height * SCR.h };
}
function onPointerDown(e) {
  e.preventDefault();
  const p = clientToLogical(e.clientX ?? e.touches[0].clientX, e.clientY ?? e.touches[0].clientY);
  touch = { pressed: true, isNewPress: true, x: p.x, y: p.y };
}
function onPointerUp(e) { touch.pressed = false; touch.isNewPress = false; }
canvas.addEventListener('mousedown', onPointerDown);
canvas.addEventListener('touchstart', onPointerDown, { passive: false });
window.addEventListener('mouseup', onPointerUp);
window.addEventListener('touchend', onPointerUp);

// Mirrors ledBusyTask()'s priority chain in ui.cpp: a brief green blink
// (alertDetected(), for a detector hit) preempts everything briefly;
// then a sustained alert (ledAlert, red/yellow alternating); then a
// accent-colored pulse for any continuously-scanning screen (ledBusy);
// then the idle heartbeat (a red blip every ~3s) as the default.
let greenBlinkUntil = 0;
let ledAlertOn = false;
const LED_BUSY_SCREENS = new Set([
  'WIFI_SCAN', 'BLE_SCAN', 'TRACKER', 'FLOCK', 'SKIMMER', 'SUBGHZ',
  'GPS_WARDRIVE', 'PROBE_WATCH', 'CLIENT_MAP', 'CAMERA_DET', 'DRONE_DET',
  'BLE_SPAM', 'WIFI_IDS',
]);
function ledBlinkGreen(ms) { greenBlinkUntil = performance.now() + ms; }
function ledAlert(on) { ledAlertOn = !!on; }
function ledPaint(color, opacity = 1) {
  if (!color) { ledEl.style.background = '#000'; ledEl.style.boxShadow = '0 0 0 1px #000'; ledEl.style.opacity = 1; return; }
  ledEl.style.background = color;
  ledEl.style.boxShadow = `0 0 6px 2px ${color}, 0 0 0 1px #000`;
  ledEl.style.opacity = opacity;
}
function serviceLed(now) {
  if (now < greenBlinkUntil) { ledPaint('#3cff6b'); return; }
  if (ledAlertOn) {
    // Red/yellow, each smoothly fading 0->full->0 over 250ms (125 up +
    // 125 down), alternating color every 250ms -- matches ledBusyTask().
    const cyclePos = now % 500, isRed = cyclePos < 250, t = cyclePos % 250;
    const k = t < 125 ? t / 125 : (250 - t) / 125;
    ledPaint(isRed ? '#ff3b3b' : '#ffd23b', k);
    return;
  }
  if (LED_BUSY_SCREENS.has(currentScreen)) {
    // Accent color, 500ms fade-in, then instantly off for the remaining
    // 250ms of a 750ms cycle -- matches ledBusyTask()'s "busy" pattern.
    const t = now % 750;
    ledPaint(accentFill(), t < 500 ? t / 500 : 0);
    return;
  }
  ledPaint(now % 3050 < 50 ? '#ff3b3b' : null);
}

// ------------------------------------------------------------------- beep
// Real Web Audio tone instead of a silent toast, for the handful of spots
// that already reference "beep" in the sim. One AudioContext, created
// lazily on first use so it's always inside a user-gesture handler (every
// call site here originates from a click/tap) and browsers don't block it.
let audioCtx = null;
function beep(durationMs, freqHz) {
  if (BEEP_VOL <= 0) return;
  if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  if (audioCtx.state === 'suspended') audioCtx.resume();
  const t0 = audioCtx.currentTime;
  const osc = audioCtx.createOscillator();
  const gain = audioCtx.createGain();
  osc.type = 'square';
  osc.frequency.value = freqHz;
  const peak = (BEEP_VOL / 100) * 0.2;
  gain.gain.setValueAtTime(0, t0);
  gain.gain.linearRampToValueAtTime(peak, t0 + 0.005);
  gain.gain.linearRampToValueAtTime(0, t0 + durationMs / 1000);
  osc.connect(gain); gain.connect(audioCtx.destination);
  osc.start(t0);
  osc.stop(t0 + durationMs / 1000 + 0.01);
}
// Range-finder chirp: rate + pitch scale with signal, same idea as the
// real firmware's rangeBeep(). Called on a timer from each LOCATE screen.
function rangeBeep(rssi) {
  const f = Math.max(0, Math.min(1, (rssi + 95) / 65));
  beep(40, 900 + f * 1400);
}

// -------------------------------------------------------- BOOT / RESET
// The device's two real physical buttons (GPIO0 / the EN pin) -- mirrors
// their actual firmware behavior as closely as a single mouse pointer
// allows (no real multi-touch "hold BOOT, tap RESET" on desktop).
const bootBtnEl = document.getElementById('bootBtn');
const resetBtnEl = document.getElementById('resetBtn');
let bootHeldSince = 0, bootHoldFired = false, bootPinResetArmed = false;
function bootPressStart(e) { e.preventDefault(); bootHeldSince = performance.now(); bootHoldFired = false; }
function bootPressEnd() {
  if (!bootHeldSince) return;
  const held = performance.now() - bootHeldSince;
  bootHeldSince = 0;
  if (held > 1500) {
    bootHoldFired = true;
    uiToast('touch recalibrated');
  } else {
    bootPinResetArmed = !bootPinResetArmed;
    bootBtnEl.classList.toggle('active', bootPinResetArmed);
  }
}
bootBtnEl.addEventListener('mousedown', bootPressStart);
bootBtnEl.addEventListener('touchstart', bootPressStart, { passive: false });
bootBtnEl.addEventListener('mouseup', bootPressEnd);
bootBtnEl.addEventListener('touchend', bootPressEnd);
resetBtnEl.addEventListener('click', () => {
  if (bootPinResetArmed) {
    bootPinResetArmed = false;
    bootBtnEl.classList.remove('active');
    uiToast('BOOT held: SPI pin overrides reset to defaults');
  }
  booted = false;
  currentScreen = 'MENU';
  displayBlanked = false;
  bootSequence();
});

// ------------------------------------------------------------- navigation
const Screens = {}; // id -> {enter, exit, frame(now,t), handleBack()}
let currentScreen = 'MENU';
const SUBS = ['SUB_WIFI', 'SUB_CS', 'SUB_RECON'];

const kTop = [
  { label: 'Engagement', target: 'ENGAGEMENT' },
  { label: 'WiFi', target: 'SUB_WIFI' },
  { label: 'Privacy', target: 'SUB_CS' },
  { label: 'Recon', target: 'SUB_RECON' },
  { label: 'Meshtastic', target: 'MESHTASTIC' },
];
const kWifiItems = [
  { label: 'WiFi scan', target: 'WIFI_SCAN' },
  { label: 'Net stats', target: 'NET_STATS' },
  { label: 'WiFi IDS', target: 'WIFI_IDS' },
  { label: 'Rogue AP', target: 'ROGUE_AP' },
  { label: 'Wardrive', target: 'GPS_WARDRIVE' },
];
const kCsItems = [
  { label: 'Flock detect', target: 'FLOCK' },
  { label: 'Tracker detect', target: 'TRACKER' },
  { label: 'Skimmer detect', target: 'SKIMMER' },
  { label: 'BLE scan', target: 'BLE_SCAN' },
  { label: 'SubGHz sweep', target: 'SUBGHZ' },
];
const kReconItems = [
  { label: 'Probe watch', target: 'PROBE_WATCH' },
  { label: 'Client map', target: 'CLIENT_MAP' },
  { label: 'Camera detect', target: 'CAMERA_DET' },
  { label: 'Drone detect', target: 'DRONE_DET' },
  { label: 'BLE spam watch', target: 'BLE_SPAM' },
];
function subItemsFor(s) { return { SUB_WIFI: kWifiItems, SUB_CS: kCsItems, SUB_RECON: kReconItems }[s]; }
function subTitleFor(s) { return { SUB_WIFI: 'WiFi', SUB_CS: 'Privacy', SUB_RECON: 'Recon' }[s]; }
function parentOf(s) {
  if (['WIFI_SCAN', 'NET_STATS', 'WIFI_IDS', 'ROGUE_AP', 'GPS_WARDRIVE'].includes(s)) return 'SUB_WIFI';
  if (['BLE_SCAN', 'TRACKER', 'FLOCK', 'SKIMMER', 'SUBGHZ'].includes(s)) return 'SUB_CS';
  if (['PROBE_WATCH', 'CLIENT_MAP', 'CAMERA_DET', 'DRONE_DET', 'BLE_SPAM'].includes(s)) return 'SUB_RECON';
  return 'MENU';
}

function gearCenter() { return { cx: 18, cy: SCR.h - 18 }; }
function drawGear() {
  const { cx, cy } = gearCenter();
  tft.fillCircle(cx, cy, 16, C.BLACK);
  for (let a = 0; a < 360; a += 45) {
    const r = a * Math.PI / 180;
    tft.fillRect(cx + Math.cos(r) * 13 - 2, cy + Math.sin(r) * 13 - 2, 4, 4, C.DARKGREY);
  }
  tft.fillCircle(cx, cy, 10, C.DARKGREY);
  tft.fillCircle(cx, cy, 4, C.BLACK);
}
function touchInGear(t) { const { cx, cy } = gearCenter(); const dx = t.x - cx, dy = t.y - cy; return dx * dx + dy * dy <= 18 * 18; }

let topButtons = [];
function drawMenu() {
  bgMode = 1;
  uiClearBelow(0);
  tft.setTextColor(accentLabel()); tft.setTextSize(3);
  tft.setCursor(8, 4); tft.print('WIFI D_VICE');
  tft.setTextSize(1); tft.setTextColor('#a11e9e');
  tft.setCursor(tft.getCursorX() + 4, 17); tft.print('v0.9');
  tft.setTextSize(2); tft.setTextWrap(false);
  const cols = SCR.w > SCR.h ? 2 : 1;
  const nrows = Math.ceil(kTop.length / cols);
  const gap = 8, top = 30, bot = SCR.h - 36;
  const colW = Math.floor((SCR.w - gap * (cols + 1)) / cols);
  const rowH = Math.floor((bot - top - gap * (nrows - 1)) / nrows);
  topButtons = kTop.map((it, k) => {
    const col = k % cols, row = Math.floor(k / cols);
    const b = { x: gap + col * (colW + gap), y: top + row * (rowH + gap), w: colW, h: rowH, label: it.label, target: it.target };
    uiDrawMenuButton(b);
    return b;
  });
  drawGear();
}
let subButtons = [];
function drawSubMenu(sub) {
  uiDrawTopBar(subTitleFor(sub));
  bgMode = 1;
  const items = subItemsFor(sub);
  uiClearBelow(UI_TOPBAR_H + 1);
  const top = UI_TOPBAR_H + 6, bot = SCR.h - UI_STATUSBAR_H - 4, gap = 6;
  let bh = 36;
  const fit = Math.floor((bot - top - gap * (items.length - 1)) / items.length);
  if (fit < bh) bh = Math.max(fit, 22);
  let y = top;
  subButtons = items.map((it) => {
    const b = { x: 8, y, w: SCR.w - 16, h: bh, label: it.label, target: it.target };
    uiDrawMenuButton(b);
    y += bh + gap;
    return b;
  });
}

function enterScreen(id) {
  currentScreen = id;
  if (SUBS.includes(id)) { drawSubMenu(id); return; }
  Screens[id]?.enter?.();
}
function exitScreen(id) { uiClearToast(); Screens[id]?.exit?.(); }

const BACK_BTN = { x: 0, y: 0, w: 60, h: UI_TOPBAR_H };
let backDownAt = 0;
function handleBackButton(now) {
  const inArea = touch.pressed && hit(touch, BACK_BTN);
  if (inArea) {
    if (backDownAt === 0) backDownAt = now;
    if (now - backDownAt > 600) {
      backDownAt = 0;
      if (!SUBS.includes(currentScreen)) exitScreen(currentScreen);
      currentScreen = 'MENU';
      drawMenu();
    }
    return true;
  }
  if (backDownAt !== 0) {
    const released = !touch.pressed;
    backDownAt = 0;
    if (released) {
      if (SUBS.includes(currentScreen)) { currentScreen = 'MENU'; drawMenu(); }
      else {
        const handled = Screens[currentScreen]?.handleBack?.();
        if (!handled) {
          exitScreen(currentScreen);
          const parent = parentOf(currentScreen);
          currentScreen = parent;
          if (parent === 'MENU') drawMenu(); else drawSubMenu(parent);
        }
      }
    }
    return true;
  }
  return false;
}

function tapped(b) { return b && touch.isNewPress && hit(touch, b); }

// ------------------------------------------------------------------ boot
let booted = false, splashUntil = 0;
function bootSequence() {
  splashUntil = performance.now() + 2600;
  bgMode = 0;
}

// -------------------------------------------------------------- main loop
let lastFrame = 0;
function stepChrome() {
  batteryPct += (Math.random() < 0.5 ? -1 : 1) * 0.02;
  batteryPct = Math.max(14, Math.min(96, batteryPct));
}

// Mirrors power.cpp's powerNoteActivity()/powerServiceAutoOff(): a global
// idle clock, checked once per frame regardless of which screen is up, so
// Display Timeout behaves the same here as on real hardware.
let lastActivityAt = performance.now();
let displayBlanked = false;
function noteActivity() { lastActivityAt = performance.now(); }
function serviceAutoOff(now) {
  if (DISPLAY_TIMEOUT_MS === 0 || displayBlanked) return;
  if (now - lastActivityAt >= DISPLAY_TIMEOUT_MS) displayBlanked = true;
}

function mainLoop(now) {
  requestAnimationFrame(mainLoop);
  if (now - lastFrame < 66) return;
  lastFrame = now;
  stepChrome();

  if (!booted) {
    const img = isLandscape ? ASSETS.splashLandscape : ASSETS.splashPortrait;
    if (img) { ctx.drawImage(img, 0, 0, SCR.w, SCR.h); }
    else { tft.fillScreen('#150a24'); }
    if (touch.isNewPress || now > splashUntil) { booted = true; drawMenu(); noteActivity(); }
    touch.isNewPress = false;
    return;
  }
  serviceLed(now);

  if (touch.isNewPress) noteActivity();
  serviceAutoOff(now);
  if (displayBlanked) {
    tft.fillScreen(C.BLACK);
    if (touch.pressed) { displayBlanked = false; noteActivity(); }
    touch.isNewPress = false;
    return;
  }

  if (currentScreen === 'MENU') {
    if (touch.isNewPress) {
      if (touchInGear(touch)) { enterScreen('SYSTEM'); touch.isNewPress = false; return; }
      for (const b of topButtons) if (hit(touch, b)) { enterScreen(b.target); touch.isNewPress = false; return; }
    }
    drawMenu();
    touch.isNewPress = false;
    return;
  }
  if (SUBS.includes(currentScreen)) {
    if (handleBackButton(now)) { touch.isNewPress = false; return; }
    if (touch.isNewPress) {
      for (const b of subButtons) if (hit(touch, b)) { enterScreen(b.target); touch.isNewPress = false; return; }
    }
    drawSubMenu(currentScreen);
    touch.isNewPress = false;
    return;
  }
  if (handleBackButton(now)) { touch.isNewPress = false; return; }
  Screens[currentScreen]?.frame?.(now, touch);
  touch.isNewPress = false;
}

function setOrientation(rot) {
  isLandscape = (rot % 2 === 0);
  SCR = isLandscape ? { ...LANDSCAPE } : { ...PORTRAIT };
  canvas.width = SCR.w; // reset before resizeCanvas recomputes CSS scale
  resizeCanvas();
}

// -------------------------------------------------------------- modal input
function askText(promptText, initial, cb, opts = {}) {
  const overlay = document.createElement('div');
  overlay.style.cssText = 'position:fixed;inset:0;background:rgba(5,4,10,.75);display:flex;align-items:center;justify-content:center;z-index:9999;font-family:Sora,sans-serif;padding:16px;';
  overlay.innerHTML = `<div style="background:#151228;border:1px solid #2a2545;border-radius:12px;padding:20px;width:280px;max-width:100%;display:flex;flex-direction:column;gap:12px;box-shadow:0 20px 60px rgba(0,0,0,.5);">
    <div style="color:#eef0fa;font-size:14px;line-height:1.4;">${promptText}</div>
    <input id="askTextInput" type="${opts.password ? 'password' : (opts.numeric ? 'number' : 'text')}" style="background:#0b0a14;border:1px solid #2a2545;color:#eef0fa;border-radius:6px;padding:8px 10px;font-size:15px;width:100%;">
    <div style="display:flex;gap:8px;justify-content:flex-end;">
      <button id="askTextCancel" style="background:none;border:1px solid #2a2545;color:#9b96bd;border-radius:6px;padding:6px 14px;cursor:pointer;font:inherit;">Cancel</button>
      <button id="askTextOk" style="background:#ff2fb0;border:none;color:#150a1e;font-weight:600;border-radius:6px;padding:6px 14px;cursor:pointer;font:inherit;">OK</button>
    </div>
  </div>`;
  document.body.appendChild(overlay);
  const inp = overlay.querySelector('#askTextInput');
  inp.value = initial ?? '';
  setTimeout(() => { inp.focus(); inp.select(); }, 0);
  const done = (val) => { document.body.removeChild(overlay); if (val !== null) cb(val); };
  overlay.querySelector('#askTextOk').onclick = () => done(inp.value);
  overlay.querySelector('#askTextCancel').onclick = () => done(null);
  inp.addEventListener('keydown', (e) => { if (e.key === 'Enter') done(inp.value); if (e.key === 'Escape') done(null); });
}

// ---------------------------------------------------------------- WiFi Scan
(() => {
  let mode = 'LIST', sel = null, logging = false, locateAngle = 0, connectMsg = '', muted = false, lastChirp = 0;
  function draw() {
    uiDrawTopBar('WiFi Scan');
    bgMode = LIST_BG ? 1 : 0;
    if (mode === 'LIST') {
      uiDrawActionRow([{ label: logging ? 'log: on' : 'log: off', key: 'log' }]);
      uiClearBelow(UI_CONTENT_Y);
      let y = UI_CONTENT_Y + 2;
      const rowH = 24, maxRows = Math.max(1, Math.floor((SCR.h - UI_STATUSBAR_H - y) / rowH));
      AP_POOL.slice(0, maxRows).forEach((ap) => {
        tft.setTextSize(2); tft.setTextColor(C.WHITE); tft.setTextWrap(false);
        tft.setCursor(4, y); tft.print(sprintf('%-13.13s', ap.ssid));
        tft.setTextSize(1); tft.setTextColor(accentLabel());
        tft.setCursor(4, y + 15); tft.print(sprintf('c%-3d %ddBm  %s', ap.channel, ap.rssi, ap.enc));
        ap._row = { x: 0, y, w: SCR.w, h: rowH };
        y += rowH;
      });
    } else if (mode === 'DETAIL') {
      uiDrawActionRow([{ label: 'Connect', key: 'connect' }, { label: 'Track', key: 'track' }]);
      uiClearBelow(UI_CONTENT_Y);
      const ap = sel; let y = UI_CONTENT_Y + 6;
      tft.setTextSize(1); tft.setTextWrap(false);
      const lines = [
        ['SSID: ' + ap.ssid, C.WHITE],
        ['BSSID: ' + ap.bssid, accentLabel()],
        ['Vendor: ' + ap.vendor, accentLabel()],
        [`Channel: ${ap.channel}   Security: ${ap.enc}`, accentLabel()],
        [`RSSI: ${ap.rssi} dBm`, accentLabel()],
      ];
      lines.forEach(([s, col]) => { tft.setTextColor(col); tft.setCursor(6, y); tft.print(s); y += 16; });
      if (connectMsg) { tft.setTextColor(connectMsg.startsWith('Connected') ? C.GREEN : C.RED); tft.setCursor(6, y + 6); tft.print(connectMsg); }
    } else if (mode === 'LOCATE') {
      uiDrawActionRow([{ label: muted ? 'unmute' : 'mute', key: 'mute' }]);
      uiClearBelow(UI_CONTENT_Y);
      const ap = sel;
      tft.setTextSize(3); tft.setTextColor(accentLabel());
      const s = sprintf('%4d dBm', ap.rssi);
      tft.setCursor((SCR.w - tft.textWidthOf(s)) / 2, UI_CONTENT_Y + 20); tft.print(s);
      const bx = 30, by = UI_CONTENT_Y + 74, bw = SCR.w - 60, bh = 24;
      tft.drawRect(bx, by, bw, bh, C.WHITE);
      const pct = Math.max(0, Math.min(1, (ap.rssi + 95) / 65));
      const col = ap.rssi > -55 ? C.GREEN : ap.rssi > -75 ? C.YELLOW : C.RED;
      tft.fillRect(bx + 2, by + 2, (bw - 4) * pct, bh - 4, col);
    }
  }
  Screens.WIFI_SCAN = {
    enter() { mode = 'LIST'; connectMsg = ''; muted = false; draw(); },
    frame(now) {
      if (now % 900 < 66) tickAps();
      draw();
      if (mode === 'LIST') {
        if (touch.isNewPress) {
          for (const ap of AP_POOL) if (hit(touch, ap._row)) { sel = ap; mode = 'DETAIL'; connectMsg = ''; return; }
        }
      } else if (mode === 'DETAIL') {
        const row = [{ label: 'Connect' }, { label: 'Track' }];
        if (touch.isNewPress) {
          if (touch.y >= UI_ACTIONROW_Y && touch.y < UI_ACTIONROW_Y + UI_ACTIONROW_H) {
            const half = SCR.w / 2;
            if (touch.x < half) {
              if (sel.enc === 'open') { connectMsg = 'Connected.'; uiToast('joined ' + sel.ssid); }
              else askText(`Passphrase for "${sel.ssid}"`, '', (v) => { connectMsg = v.length >= 8 ? 'Connected.' : 'Wrong passphrase or out of range.'; }, { password: true });
            } else { mode = 'LOCATE'; }
          }
        }
      } else if (mode === 'LOCATE') {
        if (!muted && now - lastChirp > 700) { lastChirp = now; rangeBeep(sel.rssi); }
        if (tapped({ x: 4, y: UI_ACTIONROW_Y, w: SCR.w - 8, h: UI_ACTIONROW_H })) { muted = !muted; }
      }
    },
    handleBack() {
      if (mode === 'LOCATE') { mode = 'DETAIL'; return true; }
      if (mode === 'DETAIL') { mode = 'LIST'; return true; }
      return false;
    },
  };
})();

// ---------------------------------------------------------------- Net stats
(() => {
  let page = 'MENU'; // MENU, SPEED, LAN, INFO
  let running = false, t0 = 0, dn = 0, up = 0, dnPeak = 0, upPeak = 0;
  function drawGraph(y0, h) {
    tft.drawRect(4, y0, SCR.w - 8, h, C.NS_GRID);
    for (let i = 1; i < 4; i++) tft.drawFastHLine(4, y0 + (h / 4) * i, SCR.w - 8, C.NS_GRID);
  }
  Screens.NET_STATS = {
    enter() { page = 'MENU'; this.draw(); },
    draw() {
      if (page === 'MENU') {
        uiDrawTopBar('Net stats');
        uiClearBelow(29);
        tft.setTextSize(1); tft.setTextColor(C.GREEN); tft.setCursor(4, 34); tft.print('on HomeNetwork_5G');
        this.btns = [];
        ['Speed test', 'LAN speed', 'Network info'].forEach((l, i) => uiDrawMenuButton(this.btns[i] = { x: 8, y: 56 + i * 46, w: SCR.w - 16, h: 38, label: l }));
      } else if (page === 'SPEED' || page === 'LAN') {
        uiDrawTopBar(page === 'SPEED' ? 'Speed Test' : 'LAN speed');
        uiDrawActionRow([{ label: running ? 'Stop' : 'Start', key: 'go' }]);
        uiClearBelow(UI_CONTENT_Y);
        tft.setTextSize(1); tft.setTextColor(accentLabel());
        const secs = running ? Math.floor((performance.now() - t0) / 1000) : 0;
        tft.setCursor(4, UI_CONTENT_Y + 2);
        tft.print(running ? sprintf('%2ds  dn %.1f  up %.1f Mbps', secs, dn, up) : (dn ? 'done' : 'idle'));
        drawGraph(UI_CONTENT_Y + 16, 110);
        tft.setTextColor(C.NS_DOWN); tft.setCursor(4, UI_CONTENT_Y + 132);
        tft.print(sprintf('Download %6.2f Mbps  peak %.1f', dn, dnPeak));
        tft.setTextColor(C.NS_UP); tft.setCursor(4, UI_CONTENT_Y + 148);
        tft.print(sprintf('Upload   %6.2f Mbps  peak %.1f', up, upPeak));
        tft.setTextColor(C.NS_LABEL); tft.setCursor(4, UI_CONTENT_Y + 166);
        tft.print("max on this ESP32's WiFi is ~30 Mbps");
      } else if (page === 'INFO') {
        uiDrawTopBar('Net stats');
        uiDrawActionRow([{ label: 'Refresh', key: 'refresh' }]);
        uiClearBelow(UI_CONTENT_Y);
        const rows = [
          ['-- internet --', null],
          ['public IP', '73.162.44.201'], ['network', 'AS7922 Comcast Cable'],
          ['location', 'Austin, US'], ['CF edge', 'DFW'], ['HTTP/TLS', 'http/2  TLSv1.3'],
          ['-- LAN --', null],
          ['local IP', '192.168.1.84'], ['gateway', '192.168.1.1'],
          ['-- NAT (STUN) --', null],
          ['UDP out', 'reachable'], ['behind NAT', 'yes'], ['mapping', 'endpoint-indep (cone)'],
        ];
        let y = UI_CONTENT_Y + 2;
        tft.setTextSize(1); tft.setTextWrap(false);
        rows.forEach(([label, val]) => {
          if (val === null) { tft.setTextColor(accentLabel()); tft.setCursor(4, y); tft.print(label); y += 14; return; }
          tft.setTextColor(C.NS_LABEL); tft.setCursor(4, y); tft.print(label);
          tft.setTextColor(label === 'UDP out' ? C.GREEN : C.WHITE); tft.setCursor(86, y); tft.print(val);
          y += 14;
        });
      }
    },
    frame(now) {
      if (running) {
        const t = (now - t0) / 1000;
        dn = Math.max(0, 22 + Math.sin(t) * 4 + rnd(-1, 1)); up = Math.max(0, 4 + Math.sin(t * 1.3) * 1 + rnd(-.3, .3));
        dnPeak = Math.max(dnPeak, dn); upPeak = Math.max(upPeak, up);
        if (t > 12) running = false;
      }
      this.draw();
      if (touch.isNewPress) {
        if (page === 'MENU') { for (const b of this.btns || []) if (hit(touch, b)) { page = b.label === 'Speed test' ? 'SPEED' : b.label === 'LAN speed' ? 'LAN' : 'INFO'; if (page === 'INFO') this.draw(); } }
        else if (page === 'SPEED' || page === 'LAN') {
          if (touch.y >= UI_ACTIONROW_Y && touch.y < UI_ACTIONROW_Y + UI_ACTIONROW_H) {
            running = !running; if (running) { t0 = now; dn = up = dnPeak = upPeak = 0; }
          }
        } else if (page === 'INFO') {
          if (touch.y >= UI_ACTIONROW_Y && touch.y < UI_ACTIONROW_Y + UI_ACTIONROW_H) uiToast('refreshed');
        }
      }
    },
    handleBack() {
      if (page !== 'MENU') { page = 'MENU'; running = false; return true; }
      return false;
    },
  };
})();

// ----------------------------------------------------------------- WiFi IDS
(() => {
  let log = [], banner = 'ok', bannerText = 'no attack indicators';
  let stats = { deauth: 0, disassoc: 0, beacon: 612, auth: 4 };
  let lastEvent = 0;
  function sevCol(s) { return s === 'ALERT' ? C.RED : s === 'watch' ? C.YELLOW : C.GREEN; }
  function pushLog(s) { log.unshift(s); if (log.length > 4) log.pop(); }
  function fireEvent(now) {
    const kind = pick(['deauth', 'beacon', 'auth', 'rogue', 'twin', 'pwn', 'calm']);
    const ts = fmtMMSS(BOOT_MS);
    if (kind === 'deauth') { banner = 'ALERT'; bannerText = 'DEAUTH-FLOOD'; pushLog(`${ts} DEAUTH FLOOD ${rndi(8,20)}/5s`); }
    else if (kind === 'beacon') { banner = 'watch'; bannerText = 'elevated -- watching'; pushLog(`${ts} BEACON ~${rndi(30,60)} rnd${rndi(0,3)}`); }
    else if (kind === 'auth') { banner = 'watch'; bannerText = 'elevated -- watching'; pushLog(`${ts} AUTH ${rndi(4,9)}/5s src~${rndi(2,5)}`); }
    else if (kind === 'rogue') { banner = 'ALERT'; bannerText = 'ROGUE-AP'; pushLog(`${ts} EVIL-TWIN? CafeWiFi`); }
    else if (kind === 'twin') { banner = 'ALERT'; bannerText = 'EVIL-TWIN?'; pushLog(`${ts} EVIL-TWIN? HomeNetwork_5G`); }
    else if (kind === 'pwn') { banner = 'watch'; bannerText = 'Pwnagotchi in range'; pushLog(`${ts} PWNAGOTCHI seen`); }
    else { banner = 'ok'; bannerText = 'no attack indicators'; }
  }
  Screens.WIFI_IDS = {
    enter() { log = []; banner = 'ok'; bannerText = 'no attack indicators'; lastEvent = performance.now(); this.draw(); },
    draw() {
      uiDrawTopBar('WiFi IDS');
      uiClearBelow(29);
      tft.setTextSize(1); tft.setTextWrap(false);
      tft.setTextColor(C.WHITE); tft.setCursor(4, 32);
      tft.print(sprintf('ch%2d  %s  seen %lu  drop 0', 6, fmtMMSS(BOOT_MS), 8000 + Math.floor(performance.now() / 50)));
      const rows = [
        ['DEAUTH', banner === 'ALERT' && bannerText === 'DEAUTH-FLOOD' ? 'ALERT' : 'ok', ` d${stats.deauth} dis${stats.disassoc}  rc0  spoof0`],
        ['BEACON', banner === 'watch' ? 'watch' : 'ok', ` uniq~46 rnd0 laa2 seq0 b${stats.beacon}`],
        ['AUTH/ASSOC', 'ok', ` req${stats.auth} src~3 laa0 ->A1B2C3`],
      ];
      let y = 46;
      rows.forEach(([name, sev, tail]) => {
        tft.setTextColor(sevCol(sev)); tft.setCursor(4, y); tft.print(`${name}  ${sev === 'ALERT' ? 'ALERT' : sev === 'watch' ? 'watch' : 'ok'}`);
        tft.setCursor(4, y + 11); tft.print(tail);
        y += 22;
      });
      tft.setTextColor(C.YELLOW); tft.setCursor(4, y); tft.print('ROGUE-AP  watching (baseline 7)');
      const by = y + 12, bh = 32;
      tft.fillRect(4, by, SCR.w - 8, bh, banner === 'ALERT' ? C.RED : C.BLACK);
      tft.setTextSize(2); tft.setTextColor(banner === 'ALERT' ? C.WHITE : banner === 'watch' ? C.YELLOW : C.GREEN);
      tft.setCursor(8, by + 3); tft.print(banner === 'ALERT' ? 'ATTACK LIKELY' : bannerText);
      if (banner === 'ALERT') { tft.setTextSize(1); tft.setCursor(8, by + 20); tft.print(bannerText); }
      let ly = by + bh + 5;
      tft.setTextSize(1);
      const maxLines = Math.max(0, Math.floor((SCR.h - UI_STATUSBAR_H - ly) / 11));
      log.slice(0, maxLines).forEach((l) => { tft.setTextColor(C.RED); tft.setCursor(4, ly); tft.print(l); ly += 11; });
    },
    frame(now) {
      if (now - lastEvent > 5000) {
        lastEvent = now; fireEvent(now); ledAlert(banner === 'ALERT');
        if (banner === 'ALERT') { uiToast('WiFi IDS: ' + bannerText); beep(400, 1200); }
      }
      this.draw();
      if (touch.isNewPress && touch.y > 140) { banner = 'ok'; bannerText = 'no attack indicators'; log = []; ledAlert(false); }
    },
    exit() { ledAlert(false); },
  };
})();

// ---------------------------------------------------------------- Rogue AP
(() => {
  let view = 'VIEW', learnPass = 0, baseline = 9;
  const rows = () => [
    { essid: 'HomeNetwork_5G', ch: 36, rssi: -44, tag: '' },
    { essid: 'HomeNetwork_5G', ch: 36, rssi: -71, tag: 'EVIL-TWIN' },
    { essid: 'OfficeAP-East', ch: 11, rssi: -58, tag: 'DOWNGRADE' },
    { essid: '(hidden)', ch: 6, rssi: -66, tag: '' },
    { essid: 'Neighbor-2.4G', ch: 1, rssi: -80, tag: '' },
    { essid: 'PrintServer_IoT', ch: 149, rssi: -52, tag: '' },
  ];
  Screens.ROGUE_AP = {
    enter() { view = 'VIEW'; learnPass = 0; this.draw(); },
    draw() {
      uiDrawTopBar('Rogue AP');
      if (view === 'VIEW') {
        uiDrawActionRow([{ label: 'Learn', key: 'learn' }, { label: 'Clear', key: 'clear' }]);
        uiClearBelow(UI_CONTENT_Y);
        tft.setTextSize(1); tft.setTextWrap(false);
        tft.setTextColor(accentLabel()); tft.setCursor(4, UI_CONTENT_Y + 2);
        tft.print(sprintf('baseline: %d nets  --  Learn to redo', baseline));
        let y = UI_CONTENT_Y + 16;
        rows().forEach((r) => {
          tft.setTextColor(r.tag === 'EVIL-TWIN' ? C.RED : r.tag === 'DOWNGRADE' ? C.YELLOW : C.WHITE);
          tft.setCursor(4, y);
          tft.print(sprintf('%-18s c%-2d %4d  %s', r.essid, r.ch, r.rssi, r.tag));
          y += 14;
        });
      } else if (view === 'LEARN') {
        uiClearBelow(29);
        tft.setTextSize(1); tft.setTextColor(C.YELLOW); tft.setCursor(6, 40);
        tft.print(sprintf('Learning baseline: pass %d/4', learnPass));
        tft.setTextColor(C.WHITE); tft.setCursor(6, 58); tft.print(`${6 + learnPass} networks seen so far`);
        tft.setTextColor(C.DIM_GREY); tft.setCursor(6, 90); tft.print('keep your real APs powered + in range');
        tft.setCursor(6, 104); tft.print('back = cancel');
      } else if (view === 'DONE') {
        uiClearBelow(29);
        tft.setTextSize(1); tft.setTextColor(C.GREEN); tft.setCursor(6, 60);
        tft.print(`Saved ${baseline} networks to /rogueap.csv`);
        tft.setTextColor(C.DIM_GREY); tft.setCursor(6, 78); tft.print('tap to return to the watch view');
      }
    },
    frame(now) {
      if (view === 'LEARN') {
        if (Math.floor(now / 900) % 1 === 0 && now - (this._t || 0) > 900) { this._t = now; learnPass++; if (learnPass >= 4) view = 'DONE'; }
      }
      this.draw();
      if (touch.isNewPress) {
        if (view === 'VIEW' && touch.y >= UI_ACTIONROW_Y && touch.y < UI_ACTIONROW_Y + UI_ACTIONROW_H) {
          if (touch.x < SCR.w / 2) { view = 'LEARN'; learnPass = 0; } else uiToast('tap Clear again to wipe baseline');
        } else if (view === 'DONE') { view = 'VIEW'; }
      }
    },
    handleBack() { if (view === 'LEARN') { view = 'VIEW'; return true; } return false; },
  };
})();

// ---------------------------------------------------------------- Flock Detect
(() => {
  let wifiOn = true, bleOn = true, hits = [];
  const POOL = [
    ['WiFi', 'FS_Cam_0417'], ['WiFi', 'Flock-ALPR-22'], ['BLE', 'Penguin-9A3C'],
    ['WiFi', 'pigvision-node4'], ['BLE', 'FS_Beacon12'],
  ];
  Screens.FLOCK = {
    enter() { hits = []; this.draw(); },
    draw() {
      uiDrawTopBar('Flock Detect');
      const tabW = SCR.w / 2;
      uiDrawMenuButton({ x: 0, y: 29, w: tabW, h: 26, label: 'WiFi' });
      uiDrawMenuButton({ x: tabW, y: 29, w: tabW, h: 26, label: 'BLE' });
      tft.fillRect(4, 53, tabW - 8, 3, wifiOn ? C.GREEN : C.RED);
      tft.fillRect(tabW + 4, 53, tabW - 8, 3, bleOn ? C.GREEN : C.RED);
      uiClearRect(0, 58, SCR.w, SCR.h - 58 - UI_STATUSBAR_H); uiDrawStatusBar();
      tft.setTextSize(1); tft.setTextWrap(false);
      if (!wifiOn && !bleOn) { tft.setTextColor(C.YELLOW); tft.setCursor(6, 66); tft.print('Both radios off -- nothing to scan.'); return; }
      if (!hits.length) { tft.setTextColor(C.GREEN); tft.setCursor(6, 66); tft.print('No Flock-pattern devices seen.'); return; }
      let y = 64;
      hits.forEach((h) => { tft.setTextColor(C.RED); tft.setCursor(6, y); tft.print(sprintf('[%s] %-16.16s %ddBm', h[0], h[1], h[2])); y += 14; });
    },
    frame(now) {
      if (now - (this._t || 0) > 3000) {
        this._t = now;
        if ((wifiOn || bleOn) && Math.random() < 0.4 && hits.length < 8) {
          const [kind, name] = pick(POOL);
          if ((kind === 'WiFi' && wifiOn) || (kind === 'BLE' && bleOn)) { hits.unshift([kind, name, rndi(-85, -45)]); uiToast('Flock detect: possible hit'); ledBlinkGreen(200); }
        }
      }
      this.draw();
      if (touch.isNewPress && touch.y >= 29 && touch.y < 55) { if (touch.x < SCR.w / 2) wifiOn = !wifiOn; else bleOn = !bleOn; }
    },
  };
})();

// -------------------------------------------------------------- Tracker Detect
(() => {
  let mode = 'LIST', sel = null, lastChirp = 0;
  const CLASSES = [
    { name: 'AirTag/FindMy', rssi: -52, dwell: '4m12s', mac: 2, follow: true },
    { name: 'SmartTag', rssi: -67, dwell: '0m38s', mac: 1, follow: false },
    { name: 'Tile', rssi: -74, dwell: '12m05s', mac: 1, follow: false },
    { name: 'Google FMDN', rssi: -61, dwell: '1m50s', mac: 3, follow: false, sep: true },
  ];
  Screens.TRACKER = {
    enter() { mode = 'LIST'; this.draw(); },
    draw() {
      uiDrawTopBar('Tracker Detect');
      if (mode === 'LIST') {
        uiDrawActionRow([{ label: 'reset dwell timers' }]);
        uiClearBelow(UI_CONTENT_Y);
        let y = UI_CONTENT_Y + 2;
        tft.setTextWrap(false);
        CLASSES.forEach((c) => {
          tft.setTextSize(2); tft.setTextColor(c.follow ? C.RED : C.WHITE);
          tft.setCursor(4, y); tft.print(c.name);
          tft.setTextSize(1); tft.setTextColor(c.follow ? C.RED : accentLabel());
          tft.setCursor(4, y + 18);
          tft.print(sprintf('%ddBm  %s  %dmac%s', c.rssi, c.dwell, c.mac, c.follow ? '  << FOLLOW' : c.sep ? '  separated' : ''));
          c._row = { x: 0, y, w: SCR.w, h: 32 };
          y += 34;
        });
      } else {
        uiDrawActionRow([{ label: '< list' }]);
        uiClearBelow(UI_CONTENT_Y);
        tft.setTextSize(2); tft.setTextColor(accentLabel());
        tft.setCursor(6, UI_CONTENT_Y + 4); tft.print(sel.name);
        tft.setTextSize(3);
        const s = sprintf('%4d dBm', sel.rssi);
        tft.setCursor((SCR.w - tft.textWidthOf(s)) / 2, UI_CONTENT_Y + 30); tft.print(s);
        const bx = 30, by = UI_CONTENT_Y + 74, bw = SCR.w - 60, bh = 24;
        tft.drawRect(bx, by, bw, bh, C.WHITE);
        const pct = Math.max(0, Math.min(1, (sel.rssi + 95) / 65));
        tft.fillRect(bx + 2, by + 2, (bw - 4) * pct, bh - 4, sel.rssi > -55 ? C.GREEN : sel.rssi > -75 ? C.YELLOW : C.RED);
      }
    },
    frame(now) {
      if (now % 1400 < 66) CLASSES.forEach((c) => { c.rssi = jitterRssi(c.rssi, 4); });
      this.draw();
      if (mode === 'LIST' && touch.isNewPress) for (const c of CLASSES) if (hit(touch, c._row)) { sel = c; mode = 'LOCATE'; }
      else if (mode === 'LOCATE' && now - lastChirp > 700) { lastChirp = now; rangeBeep(sel.rssi); }
    },
    handleBack() { if (mode === 'LOCATE') { mode = 'LIST'; return true; } return false; },
  };
})();

// -------------------------------------------------------------- Skimmer Detect
(() => {
  let hits = [];
  const POOL = ['HC-06', 'JDY-31', '(no name) [UART]', 'HM-10', 'BT05'];
  Screens.SKIMMER = {
    enter() { hits = []; this.draw(); },
    draw() {
      uiDrawTopBar('Skimmer Detect');
      uiClearBelow(29);
      tft.setTextSize(1); tft.setTextWrap(false);
      tft.setTextColor(C.WHITE); tft.setCursor(4, 34); tft.print('BLE (HC-05/06/08 style modules):');
      if (!hits.length) { tft.setTextColor(C.GREEN); tft.setCursor(4, 52); tft.print('none seen'); return; }
      let y = 50;
      hits.forEach((n) => { tft.setTextColor(C.RED); tft.setCursor(4, y); tft.print(sprintf('  %-16.16s %ddBm', n, rndi(-80, -45))); y += 14; });
    },
    frame(now) {
      if (now - (this._t || 0) > 3500) { this._t = now; if (Math.random() < 0.3 && hits.length < 6) { hits.unshift(pick(POOL)); uiToast('Skimmer detect: possible hit'); ledBlinkGreen(400); } }
      this.draw();
    },
  };
})();

// ------------------------------------------------------------------ BLE Scan
(() => {
  let mode = 'LIST', sel = null, logging = false, muted = false, lastChirp = 0;
  function classify(name) { if (name.includes('Flipper')) return { cls: 'FLIPPER', col: C.MAGENTA }; if (name.includes('Ray-Ban')) return { cls: 'GLASSES', col: C.ORANGE }; return null; }
  const DEVS = BLE_NAME_POOL.map((n) => ({ name: n, mac: randMac(), rssi: rndi(-88, -42), vendor: pick(VENDOR_POOL) }));
  Screens.BLE_SCAN = {
    enter() { mode = 'LIST'; muted = false; this.draw(); },
    draw() {
      uiDrawTopBar('BLE Scan');
      if (mode === 'LIST') {
        uiDrawActionRow([{ label: logging ? 'log: on' : 'log: off' }]);
        const summaryY = SCR.h - UI_STATUSBAR_H - 16;
        uiClearRect(0, UI_CONTENT_Y, SCR.w, summaryY - UI_CONTENT_Y); uiDrawStatusBar();
        let y = UI_CONTENT_Y + 2;
        const rowH = 24, maxRows = Math.max(1, Math.floor((summaryY - y) / rowH));
        let flipperN = 0, glassesN = 0;
        DEVS.forEach((d) => { const c = classify(d.name); if (c?.cls === 'FLIPPER') flipperN++; if (c?.cls === 'GLASSES') glassesN++; });
        tft.setTextWrap(false);
        DEVS.slice(0, maxRows).forEach((d) => {
          const c = classify(d.name);
          if (c) tft.fillRect(0, y, 3, rowH - 4, c.col);
          tft.setTextSize(2); tft.setTextColor(c ? c.col : C.WHITE);
          tft.setCursor(6, y); tft.print(sprintf('%-13.13s', d.name));
          tft.setTextSize(1); tft.setTextColor(c ? c.col : accentLabel());
          tft.setCursor(6, y + 15); tft.print(c ? sprintf('%-7s %ddBm', c.cls, d.rssi) : sprintf('%s %ddBm', d.vendor, d.rssi));
          d._row = { x: 0, y, w: SCR.w, h: rowH };
          y += rowH;
        });
        tft.setTextSize(1); tft.setCursor(4, SCR.h - UI_STATUSBAR_H - 14);
        if (!flipperN && !glassesN) { tft.setTextColor(C.GREEN); tft.print('no Flipper / glasses seen'); }
        else { tft.setTextColor(C.MAGENTA); tft.print(`Flipper:${flipperN}  `); tft.setTextColor(C.ORANGE); tft.print(`Glasses:${glassesN}`); }
      } else if (mode === 'DETAIL') {
        uiDrawActionRow([{ label: 'Track' }]);
        uiClearBelow(UI_CONTENT_Y);
        tft.setTextSize(1); tft.setTextWrap(false); tft.setTextColor(accentLabel());
        [`Name: ${sel.name}`, `MAC:  ${sel.mac}`, `Vendor: ${sel.vendor}`, `RSSI: ${sel.rssi} dBm`].forEach((l, i) => { tft.setCursor(6, UI_CONTENT_Y + 6 + i * 16); tft.print(l); });
      } else if (mode === 'LOCATE') {
        uiDrawActionRow([{ label: muted ? 'unmute' : 'mute' }]);
        uiClearBelow(UI_CONTENT_Y);
        tft.setTextSize(2); tft.setTextColor(accentLabel()); tft.setCursor(6, UI_CONTENT_Y + 4); tft.print(sel.name);
        tft.setTextSize(3);
        const s = sprintf('%4d dBm', sel.rssi);
        tft.setCursor((SCR.w - tft.textWidthOf(s)) / 2, UI_CONTENT_Y + 30); tft.print(s);
        const bx = 30, by = UI_CONTENT_Y + 70, bw = SCR.w - 60, bh = 30;
        tft.drawRect(bx, by, bw, bh, C.WHITE);
        const pct = Math.max(0, Math.min(1, (sel.rssi + 95) / 65));
        tft.fillRect(bx + 2, by + 2, (bw - 4) * pct, bh - 4, sel.rssi > -60 ? C.GREEN : sel.rssi > -80 ? C.YELLOW : C.RED);
      }
    },
    frame(now) {
      if (now % 3000 < 66) DEVS.forEach((d) => { d.rssi = jitterRssi(d.rssi, 5); });
      this.draw();
      if (mode === 'LIST') {
        if (touch.isNewPress) {
          if (touch.y >= UI_ACTIONROW_Y && touch.y < UI_ACTIONROW_Y + UI_ACTIONROW_H) logging = !logging;
          else for (const d of DEVS) if (hit(touch, d._row)) { sel = d; mode = 'DETAIL'; }
        }
      } else if (mode === 'DETAIL' && tapped({ x: 4, y: UI_ACTIONROW_Y, w: SCR.w - 8, h: UI_ACTIONROW_H })) mode = 'LOCATE';
      else if (mode === 'LOCATE') {
        if (!muted && now - lastChirp > 700) { lastChirp = now; rangeBeep(sel.rssi); }
        if (tapped({ x: 4, y: UI_ACTIONROW_Y, w: SCR.w - 8, h: UI_ACTIONROW_H })) muted = !muted;
      }
    },
    handleBack() { if (mode === 'LOCATE') { mode = 'DETAIL'; return true; } if (mode === 'DETAIL') { mode = 'LIST'; return true; } return false; },
  };
})();

// ------------------------------------------------------------------- SubGHz
(() => {
  const BANDS = ['300-348 MHz (315 fobs, TPMS)', '387-464 MHz (433.92 ISM, alarms)', '779-928 MHz (868 EU, 915 US ISM)'];
  const CENTERS = [315.00, 318.00, 330.00, 345.00, 390.00, 418.00, 433.92, 434.42, 868.35, 915.00];
  let modeIdx = 0, band = 1, sweep = [], captured = false;
  const MODES = ['Sweep', 'Analyzer', 'Raw'];
  function regenSweep() { sweep = Array.from({ length: 60 }, () => rndi(-102, -90)); const spike = rndi(0, 59); sweep[spike] = rndi(-55, -42); }
  regenSweep();
  Screens.SUBGHZ = {
    enter() { this.draw(); },
    draw() {
      uiDrawTopBar('SubGHz');
      uiDrawActionRow([{ label: MODES[modeIdx] }, { label: MODES[modeIdx] === 'Raw' ? 'capture' : 'band' }]);
      uiClearBelow(UI_CONTENT_Y);
      tft.setTextSize(1); tft.setTextWrap(false);
      const GRAPH_TOP = UI_CONTENT_Y + 14, GRAPH_H = SCR.h - GRAPH_TOP - UI_STATUSBAR_H - 14;
      if (MODES[modeIdx] === 'Sweep') {
        tft.setTextColor(C.WHITE); tft.setCursor(4, UI_CONTENT_Y + 2); tft.print(BANDS[band]);
        const w = SCR.w / sweep.length;
        let maxV = -999, maxI = 0;
        sweep.forEach((v, i) => { if (v > maxV) { maxV = v; maxI = i; } const col = v > -55 ? C.RED : v > -75 ? C.YELLOW : C.GREEN; const hgt = Math.max(2, (v + 105) / 65 * GRAPH_H); tft.fillRect(i * w, GRAPH_TOP + GRAPH_H - hgt, Math.ceil(w), hgt, col); });
        tft.setTextColor(maxV > -55 ? C.RED : C.YELLOW);
        tft.setCursor(4, GRAPH_TOP + GRAPH_H + 4);
        tft.print(maxV > -70 ? sprintf('CARRIER @ %.2f  %ddBm', 433.94, maxV) : sprintf('quiet  floor %ddBm', maxV));
      } else if (MODES[modeIdx] === 'Analyzer') {
        let strongestI = 0, strongestV = -999;
        const vals = CENTERS.map((c, i) => { const v = i === 6 ? rndi(-50, -40) : rndi(-105, -85); if (v > strongestV) { strongestV = v; strongestI = i; } return v; });
        tft.setTextColor(strongestV > -70 ? C.RED : C.GREEN); tft.setCursor(4, UI_CONTENT_Y + 2);
        tft.print(sprintf('strongest: %.2f MHz  %ddBm', CENTERS[strongestI], strongestV));
        const rowH = GRAPH_H / CENTERS.length;
        CENTERS.forEach((c, i) => {
          const y = GRAPH_TOP + i * rowH;
          tft.setTextColor(i === strongestI ? C.RED : accentLabel());
          tft.setCursor(2, y + 1); tft.print(sprintf('%.2f', c));
          const v = vals[i]; const pct = Math.max(0, Math.min(1, (v + 105) / 65));
          tft.fillRect(60, y + 1, (SCR.w - 100) * pct, rowH - 4, v > -55 ? C.RED : v > -75 ? C.YELLOW : C.GREEN);
          tft.setTextColor(C.WHITE); tft.setCursor(SCR.w - 34, y + 1); tft.print(String(v));
        });
      } else {
        tft.setTextColor(C.WHITE); tft.setCursor(4, UI_CONTENT_Y + 2); tft.print(sprintf('OOK @ %.2f MHz   GDO0=GPIO27', 433.92));
        tft.setTextColor(accentLabel()); tft.setCursor(4, UI_CONTENT_Y + 14);
        tft.print(captured ? 'captured -- tap graph to save .sub' : 'press capture and send a signal');
        if (captured) {
          tft.setTextColor(C.GREENYELLOW);
          let x = 4, y0 = GRAPH_TOP + GRAPH_H / 2, level = 1;
          ctx.beginPath(); ctx.strokeStyle = C.GREENYELLOW;
          ctx.moveTo(x, y0 - level * 14);
          for (let i = 0; i < 40; i++) { const w2 = rndi(4, 14); ctx.lineTo(x + w2, y0 - level * 14); level *= -1; ctx.lineTo(x + w2, y0 - level * 14); x += w2; if (x > SCR.w - 4) break; }
          ctx.stroke();
          tft.setTextColor(C.WHITE); tft.setCursor(4, GRAPH_TOP + GRAPH_H + 4); tft.print('142 edges  48213 us span');
        }
      }
    },
    frame(now) {
      if (now % 500 < 66 && MODES[modeIdx] === 'Sweep') regenSweep();
      this.draw();
      if (touch.isNewPress && touch.y >= UI_ACTIONROW_Y && touch.y < UI_ACTIONROW_Y + UI_ACTIONROW_H) {
        const half = SCR.w / 2;
        if (touch.x < half) { modeIdx = (modeIdx + 1) % 3; }
        else if (MODES[modeIdx] === 'Raw') { captured = true; uiToast('captured -- tap graph to save'); }
        else {
          band = (band + 1) % 3;
          if (MODES[modeIdx] === 'Sweep') regenSweep();
          else uiToast('band: Analyzer always scans all 10 fixed ISM centres');
        }
      } else if (touch.isNewPress && MODES[modeIdx] === 'Raw' && captured && touch.y > UI_CONTENT_Y + 20) {
        uiToast('/subghz/raw_004.sub');
      }
    },
  };
})();

// ------------------------------------------------------------- Probe Watch
(() => {
  // Mirrors probe_watch.cpp: probed SSIDs vs beacons seen nearby, plus the
  // hidden-AP unmask via probe-response correlation (this session's work).
  const ROWS_DEF = [
    { ssid: 'HomeNetwork_5G', dev: 2, rssi: -48, hits: 61, state: 'present' },
    { ssid: 'CoffeeShop-WiFi', dev: 1, rssi: -70, hits: 4, state: 'present' },
    { ssid: 'OldOffice-Guest', dev: 1, rssi: -66, hits: 9, state: 'absent' },
    { ssid: 'AirportFreeWiFi', dev: 3, rssi: -81, hits: 22, state: 'absent' },
    { ssid: 'ClubHouseWiFi', dev: 1, rssi: -58, hits: 3, state: 'hidden' },
  ];
  let reqs = 4200, wildcard = 900;
  Screens.PROBE_WATCH = {
    enter() { this.draw(); },
    draw() {
      uiDrawTopBar('Probe Watch');
      bgMode = LIST_BG ? 1 : 0;
      uiClearBelow(UI_CONTENT_Y_PLAIN);
      tft.setTextSize(1); tft.setTextColor(C.WHITE); tft.setTextWrap(false);
      tft.setCursor(2, UI_CONTENT_Y_PLAIN); tft.print(sprintf('ch%2d  %lu req  %lu wildcard', 6, reqs, wildcard));
      let y = UI_CONTENT_Y_PLAIN + 14;
      ROWS_DEF.forEach((p) => {
        const flag = p.state === 'absent' ? '!' : p.state === 'hidden' ? 'H' : ' ';
        const tag = p.state === 'absent' ? ' ABSENT' : p.state === 'hidden' ? ' HIDDEN-AP' : '';
        tft.setTextColor(p.state === 'absent' ? C.RED : p.state === 'hidden' ? C.MAGENTA : C.YELLOW);
        tft.setCursor(4, y);
        tft.print(sprintf('%c%-15.15s %dd %ddBm x%u%s', flag, p.ssid, p.dev, p.rssi, p.hits, tag));
        y += 12;
      });
    },
    frame(now) {
      if (now % 1500 < 66) { reqs += rndi(1, 5); if (Math.random() < 0.3) wildcard++; ROWS_DEF.forEach((p) => { p.rssi = jitterRssi(p.rssi, 2); }); }
      this.draw();
    },
  };
})();

// -------------------------------------------------------------- Client Map
(() => {
  const STATIONS = Array.from({ length: 6 }, () => ({ mac: randMac(), vendor: pick(VENDOR_POOL), ap: rndi(0, 1) ? randMac().slice(-5) : '', rssi: rndi(-85, -40), frames: rndi(2, 30), fresh: Math.random() < 0.5, random: Math.random() < 0.2 }));
  let frames = 812;
  Screens.CLIENT_MAP = {
    enter() { this.draw(); },
    draw() {
      uiDrawTopBar('Client Map');
      uiClearBelow(29);
      tft.setTextSize(1); tft.setTextColor(C.WHITE); tft.setTextWrap(false);
      tft.setCursor(4, 34); tft.print(sprintf('ch%2d  %d stations  %lu frames', 6, STATIONS.length, frames));
      let y = 50;
      STATIONS.forEach((s) => {
        tft.setTextColor(s.random ? C.DIM_GREY : s.fresh ? C.YELLOW : accentLabel());
        tft.setCursor(4, y);
        const mac6 = s.mac.split(':').slice(-3).join('');
        tft.print(sprintf('%s %-14.14s %s%s %ddBm x%u', mac6, s.random ? '(random)' : s.vendor, s.ap ? '>' : ' ', s.ap || '    ', s.rssi, s.frames));
        y += 14;
      });
    },
    frame(now) {
      if (now % 1200 < 66) { frames += rndi(1, 6); STATIONS.forEach((s) => { s.rssi = jitterRssi(s.rssi, 3); }); }
      this.draw();
      if (touch.isNewPress) { frames = 0; }
    },
  };
})();

// ------------------------------------------------------------- Camera Detect
(() => {
  let mode = 'LIST', sel = null, lastChirp = 0;
  const ROWS = [
    { vendor: 'Hikvision', bssid: 'A1:22:9F', ch: 6, rssi: -58, ssid: 'IPCAM_Front_Door' },
    { vendor: 'Dahua', bssid: '3F:B0:77', ch: 11, rssi: -66, ssid: '(hidden)' },
    { vendor: 'Reolink', bssid: 'C8:1F:66', ch: 1, rssi: -49, ssid: 'Reolink-Backyard' },
    { vendor: 'Wyze', bssid: '2C:AA:8E', ch: 6, rssi: -72, ssid: 'WyzeCam_Garage' },
    { vendor: 'Ezviz', bssid: '98:D8:63', ch: 3, rssi: -80, ssid: 'EZVIZ_C3X_Porch' },
  ];
  Screens.CAMERA_DET = {
    enter() { mode = 'LIST'; this.draw(); },
    draw() {
      uiDrawTopBar('Camera Detect');
      if (mode === 'LIST') {
        uiClearBelow(29);
        tft.setTextSize(1); tft.setTextWrap(false);
        let y = 34;
        ROWS.forEach((r) => {
          tft.setTextColor(C.RED); tft.setCursor(4, y); tft.print(sprintf('%-9s %s ch%-2d %ddBm', r.vendor, r.bssid, r.ch, r.rssi));
          tft.setTextColor(accentLabel()); tft.setCursor(4, y + 12); tft.print(r.ssid);
          r._row = { x: 0, y, w: SCR.w, h: 26 };
          y += 28;
        });
        tft.setTextColor(C.DIM_GREY); tft.setCursor(4, y + 2); tft.print('tap a row to direction-find');
      } else {
        uiDrawActionRow([{ label: '< list' }]);
        uiClearBelow(UI_CONTENT_Y);
        tft.setTextSize(2); tft.setTextColor(C.RED); tft.setCursor(6, UI_CONTENT_Y + 4); tft.print(sel.vendor);
        tft.setTextSize(1); tft.setCursor(6, UI_CONTENT_Y + 24); tft.print(sprintf('%s  ch%d  %s', sel.bssid, sel.ch, sel.ssid));
        tft.setTextSize(3);
        const s = sprintf('%4d dBm', sel.rssi);
        tft.setCursor((SCR.w - tft.textWidthOf(s)) / 2, UI_CONTENT_Y + 46); tft.print(s);
        const bx = 30, by = UI_CONTENT_Y + 92, bw = SCR.w - 60, bh = 24;
        tft.drawRect(bx, by, bw, bh, C.WHITE);
        const pct = Math.max(0, Math.min(1, (sel.rssi + 95) / 65));
        tft.fillRect(bx + 2, by + 2, (bw - 4) * pct, bh - 4, sel.rssi > -55 ? C.GREEN : sel.rssi > -75 ? C.YELLOW : C.RED);
      }
    },
    frame(now) {
      if (now % 2000 < 66) ROWS.forEach((r) => { r.rssi = jitterRssi(r.rssi, 3); });
      this.draw();
      if (mode === 'LIST' && touch.isNewPress) for (const r of ROWS) if (hit(touch, r._row)) { sel = r; mode = 'LOCATE'; }
      else if (mode === 'LOCATE' && now - lastChirp > 700) { lastChirp = now; rangeBeep(sel.rssi); }
    },
    handleBack() { if (mode === 'LOCATE') { mode = 'LIST'; return true; } return false; },
  };
})();

// -------------------------------------------------------------- Drone Detect
(() => {
  let tab = 'WiFi';
  const WIFI_ROWS = [
    { src: 'W', id: '1SEEZ1234A56B7890CD', rssi: -54, hits: 41, pos: '37.77490, -122.41940' },
    { src: 'W', id: '1A2B3C4D5E6F7081920X', rssi: -70, hits: 9 },
    { src: 'W', id: 'WiFi-drone', rssi: -77, hits: 2 },
  ];
  const BLE_ROWS = [
    { src: 'B', id: 'DJI-drone', rssi: -61, hits: 120 },
    { src: 'B', id: '1SEEZ9988776655443322', rssi: -48, hits: 63, pos: '40.71280, -74.00600' },
  ];
  Screens.DRONE_DET = {
    enter() { this.draw(); },
    draw() {
      uiDrawTopBar('Drone Detect');
      const tabW = SCR.w / 2;
      uiDrawMenuButton({ x: 0, y: 29, w: tabW, h: 26, label: 'WiFi' });
      uiDrawMenuButton({ x: tabW, y: 29, w: tabW, h: 26, label: 'BLE' });
      tft.fillRect(tab === 'WiFi' ? 4 : tabW + 4, 53, tabW - 8, 3, C.GREEN);
      uiClearRect(0, 58, SCR.w, SCR.h - 58 - UI_STATUSBAR_H); uiDrawStatusBar();
      const rows = tab === 'WiFi' ? WIFI_ROWS : BLE_ROWS;
      tft.setTextSize(1); tft.setTextWrap(false); tft.setTextColor(C.WHITE);
      tft.setCursor(4, 64); tft.print(sprintf('%s   %lu RID msgs', tab === 'WiFi' ? 'WiFi beacon/NaN' : 'BLE 0xFFFA/DJI', rows.reduce((a, r) => a + r.hits, 0)));
      let y = 80;
      rows.forEach((r) => {
        tft.setTextColor(C.RED); tft.setCursor(4, y); tft.print(sprintf('[%c] %-20.20s %ddBm x%u', r.src, r.id, r.rssi, r.hits));
        y += 12;
        if (r.pos) { tft.setTextColor(C.YELLOW); tft.setCursor(10, y); tft.print('op ' + r.pos); y += 12; }
      });
    },
    frame(now) {
      if (now % 1500 < 66) { WIFI_ROWS.concat(BLE_ROWS).forEach((r) => { r.rssi = jitterRssi(r.rssi, 3); r.hits++; }); }
      this.draw();
      if (touch.isNewPress && touch.y >= 29 && touch.y < 55) tab = touch.x < SCR.w / 2 ? 'WiFi' : 'BLE';
    },
  };
})();

// ------------------------------------------------------------ BLE Spam Watch
(() => {
  let rate = 38, addr = 9, continuity = 1, swiftpair = 0, fastpair = 0, sev = 'ok', spikeUntil = 0;
  function sevCol(s) { return s === 'ALERT' ? C.RED : s === 'watch' ? C.YELLOW : C.GREEN; }
  Screens.BLE_SPAM = {
    enter() { this.draw(); },
    draw() {
      uiDrawTopBar('BLE Spam Watch');
      uiClearBelow(29);
      tft.setTextSize(1); tft.setTextWrap(false);
      tft.setTextColor(C.WHITE); tft.setCursor(4, 34); tft.print(sprintf('adv/s %lu   distinct addr ~%d', rate, addr));
      tft.setTextColor(accentLabel()); tft.setCursor(4, 48); tft.print(sprintf('Apple popup-spam frames: %lu', continuity));
      tft.setCursor(4, 62); tft.print(sprintf('SwiftPair: %lu   FastPair: %lu', swiftpair, fastpair));
      const by = 82, bh = 60;
      if (sev === 'ALERT') {
        tft.fillRect(4, by, SCR.w - 8, bh, C.RED);
        tft.setTextSize(2); tft.setTextColor(C.WHITE); tft.setCursor(10, by + 8); tft.print('BLE SPAM');
        tft.setTextSize(1); tft.setCursor(10, by + 30); tft.print(continuity >= 15 ? 'Apple proximity popup flood' : 'advertisement flood');
      } else {
        tft.setTextSize(2); tft.setTextColor(sevCol(sev)); tft.setCursor(10, by + 16);
        tft.print(sev === 'watch' ? 'elevated advertisement activity' : 'no advertisement flood');
      }
    },
    frame(now) {
      if (now < spikeUntil) { rate = rndi(260, 380); continuity = rndi(40, 65); }
      else { rate = rndi(20, 60); continuity = rndi(0, 4); swiftpair = rndi(0, 2); fastpair = rndi(0, 2); addr = rndi(6, 14); }
      if (now - (this._t || 0) > 9000) { this._t = now; if (Math.random() < 0.35) { spikeUntil = now + 3500; ledBlinkGreen(350); uiToast('BLE spam watch: alert'); } }
      sev = rate >= 250 || continuity >= 40 || (continuity >= 15 && addr >= 20) ? 'ALERT' : (rate >= 110 || continuity >= 12) ? 'watch' : 'ok';
      this.draw();
      if (touch.isNewPress) { spikeUntil = 0; greenBlinkUntil = 0; }
    },
  };
})();

// ------------------------------------------------------------------ Meshtastic
(() => {
  let phase = 'SCAN', scanT0 = 0, tab = 'Nodes', feed = [];
  const NODES = [
    { name: 'BASE', id: '!a1b2c3', snr: 8, hop: 0, batt: 91, v: 4.05, age: 12, self: true },
    { name: 'RLY1', id: '!7f0012', snr: -3, hop: 1, batt: 62, v: 3.81, age: 47 },
    { name: 'N3F2', id: '!05e9aa', snr: 2, hop: 2, batt: null, v: null, age: 190 },
  ];
  const FEED_POOL = ['BASE: all clear on ridge', 'RLY1  78%  3.94V', '!05e9aa: testing 1 2 3', 'BASE: repeater holding'];
  Screens.MESHTASTIC = {
    enter() { phase = 'SCAN'; scanT0 = performance.now(); feed = []; this.draw(); },
    draw() {
      uiDrawTopBar('Meshtastic');
      uiClearBelow(29);
      tft.setTextSize(1); tft.setTextWrap(false);
      if (phase === 'SCAN') {
        const left = Math.max(0, 6 - Math.floor((performance.now() - scanT0) / 1000));
        tft.setTextColor(accentLabel()); tft.setCursor(6, 40); tft.print(sprintf('Scanning for nodes...  %lus  (2)', left));
        tft.setTextColor(C.DIM_GREY); tft.setCursor(6, 56); tft.print('tap to stop early   back = leave');
      } else if (phase === 'PICK') {
        tft.setTextColor(accentLabel()); tft.setCursor(6, 36); tft.print('Pick a node:');
        this._picks = [];
        ['D_VICE-Base (RAK4631)', 'Relay-North (T-Beam)'].forEach((n, i) => uiDrawMenuButton(this._picks[i] = { x: 8, y: 50 + i * 40, w: SCR.w - 16, h: 32, label: n }));
        uiDrawMenuButton(this._forget = { x: 8, y: SCR.h - UI_STATUSBAR_H - 40, w: SCR.w - 16, h: 30, label: 'forget BLE bond' });
      } else if (phase === 'LIVE') {
        const tabW = SCR.w / 2;
        uiDrawMenuButton({ x: 0, y: 29, w: tabW, h: 26, label: 'Nodes' });
        uiDrawMenuButton({ x: tabW, y: 29, w: tabW, h: 26, label: 'Feed' });
        tft.fillRect(tab === 'Nodes' ? 4 : tabW + 4, 53, tabW - 8, 2, accentFill());
        uiClearRect(0, 58, SCR.w, SCR.h - 58 - UI_STATUSBAR_H); uiDrawStatusBar();
        if (tab === 'Nodes') {
          tft.setTextColor(C.WHITE); tft.setCursor(4, 64); tft.print(sprintf('my !a1b2c3   nodes: %d', NODES.length));
          let y = 80;
          NODES.forEach((n) => {
            tft.setTextColor(n.self ? C.GREEN : C.WHITE);
            tft.setCursor(4, y); tft.print(sprintf('%-4.4s s%s%.0f h%d', n.name, n.snr >= 0 ? '+' : '', n.snr, n.hop));
            tft.setCursor(110, y); tft.print(n.batt != null ? sprintf('%d%% %.1fV', n.batt, n.v) : '');
            tft.setCursor(190, y); tft.print(sprintf('%ds', n.age));
            y += 16;
          });
        } else {
          if (!feed.length) { tft.setTextColor(C.YELLOW); tft.setCursor(6, 68); tft.print('waiting for mesh traffic...'); }
          let y = 64;
          feed.forEach((l) => { tft.setTextColor(C.WHITE); tft.setCursor(4, y); tft.print(l); y += 14; });
        }
      }
    },
    frame(now) {
      if (phase === 'SCAN' && now - scanT0 > 2600) phase = 'PICK';
      if (phase === 'LIVE' && now - (this._ft || 0) > 6000) { this._ft = now; feed.unshift(pick(FEED_POOL)); if (feed.length > 6) feed.pop(); NODES.forEach((n) => { n.age = 0; }); }
      this.draw();
      if (touch.isNewPress) {
        if (phase === 'SCAN') { phase = 'PICK'; }
        else if (phase === 'PICK') {
          if (this._picks?.some((b) => hit(touch, b))) askText('Node BLE PIN', '123456', () => { phase = 'LIVE'; tab = 'Nodes'; });
          else if (hit(touch, this._forget)) uiToast('BLE bond forgotten');
        } else if (phase === 'LIVE' && touch.y >= 29 && touch.y < 55) tab = touch.x < SCR.w / 2 ? 'Nodes' : 'Feed';
      }
    },
  };
})();

// ----------------------------------------------------------------- Engagement
(() => {
  let armed = false, client = '', tester = '', hasKey = false, blePaired = false;
  Screens.ENGAGEMENT = {
    enter() { this.draw(); },
    draw() {
      uiDrawTopBar('Engagement');
      uiClearBelow(29);
      tft.setTextSize(1); tft.setTextWrap(false);
      const rows = [
        ['BLE Pair', blePaired ? 'paired' : 'not paired', blePaired ? C.GREEN : C.YELLOW],
        ['Client', client || '<tap to pick>', C.WHITE],
        ['Tester', tester || '<tap to set>', C.WHITE],
        ['Passphrase', hasKey ? '(set)' : '', C.WHITE],
        ['Export BLE', 'OFF', C.YELLOW],
        ['WiFi download', 'SoftAP + HTTP', C.WHITE],
      ];
      let y = 34;
      this._rows = rows.map(([label, val, col]) => {
        tft.drawRect(6, y, SCR.w - 12, 24, C.WHITE);
        tft.setTextColor(accentLabel()); tft.setCursor(10, y + 6); tft.print(label);
        tft.setTextColor(col); tft.setCursor(140, y + 6); tft.print(val);
        const r = { x: 6, y, w: SCR.w - 12, h: 24, label };
        y += 28;
        return r;
      });
      y += 4;
      uiDrawMenuButton(this._arm = { x: 6, y, w: (SCR.w - 12) * 0.62, h: 30, label: armed ? 'DISARM' : 'ARM' });
      uiDrawMenuButton(this._clear = { x: 6 + (SCR.w - 12) * 0.62 + 4, y, w: (SCR.w - 12) * 0.38 - 4, h: 30, label: 'Clear data' });
      tft.setTextColor(armed ? C.GREEN : C.WHITE); tft.setCursor(6, y + 36);
      tft.print(sprintf('%s  clock: %s', armed ? 'ARMED ' : '', clockString()));
    },
    frame() {
      this.draw();
      if (touch.isNewPress) {
        if (hit(touch, this._arm)) {
          if (!armed) { if (!blePaired) uiToast('pair a BLE second factor first'); else if (!client || !tester || !hasKey) uiToast('need client + tester + passphrase'); else { armed = true; uiToast('ARMED'); } }
          else { armed = false; uiToast('disarmed (no prompt at next boot)'); }
        } else if (hit(touch, this._clear)) { askText('Type WIPE to confirm', '', (v) => { if (v === 'WIPE') { client = tester = ''; hasKey = false; uiToast('data cleared'); } }); }
        else for (const r of this._rows || []) if (hit(touch, r)) {
          if (r.label === 'BLE Pair') { blePaired = true; uiToast('BLE paired'); }
          else if (r.label === 'Client') askText('Client name', client, (v) => { client = v; if (v) hasKey = true; });
          else if (r.label === 'Tester') askText('Tester name', tester, (v) => { tester = v; });
          else if (r.label === 'Passphrase') askText('Set passphrase', '', (v) => { hasKey = v.length >= 8; uiToast(hasKey ? 'unlocked' : 'too short'); }, { password: true });
        }
      }
    },
  };
})();

// ---------------------------------------------------------------- Wardrive
(() => {
  let logging = false, rows = 0, sats = rndi(6, 11), lat = 30.2672, lon = -97.7431;
  Screens.GPS_WARDRIVE = {
    enter() { this.draw(); },
    draw() {
      uiDrawTopBar('Wardrive');
      uiDrawActionRow([{ label: 'start / stop logging' }]);
      uiClearBelow(UI_CONTENT_Y);
      tft.setTextSize(1); tft.setTextWrap(false); tft.setTextColor(C.WHITE);
      let y = UI_CONTENT_Y + 4;
      tft.setCursor(4, y); tft.print(sprintf('Fix: yes  sats: %lu', sats)); y += 16;
      tft.setCursor(4, y); tft.print(sprintf('lat %.6f  lon %.6f', lat, lon)); y += 16;
      tft.setCursor(4, y); tft.print('SD: ok'); y += 16;
      tft.setTextColor(C.YELLOW); tft.setCursor(4, y); tft.print('engagement: not armed (plaintext!)'); tft.setTextColor(C.WHITE); y += 16;
      tft.setCursor(4, y); tft.print(sprintf('logging: %s', logging ? 'ON' : 'off')); y += 16;
      tft.setCursor(4, y); tft.print(sprintf('%lu rows -> %s', rows, logging ? 'SD' : '-'));
    },
    frame(now) {
      if (logging && now % 1000 < 66) { rows++; lat += rnd(-0.0004, 0.0004); lon += rnd(-0.0004, 0.0004); }
      if (now % 4000 < 66) sats = rndi(5, 12);
      this.draw();
      if (touch.isNewPress && touch.y >= UI_ACTIONROW_Y && touch.y < UI_ACTIONROW_Y + UI_ACTIONROW_H) { logging = !logging; if (logging) rows = 0; }
    },
  };
})();

// -------------------------------------------------------------------- System
(() => {
  const TZLIST = ['UTC-12', 'UTC-10 (Hawaii)', 'UTC-8 (US Pacific)', 'UTC-5 (US Eastern)', 'UTC+0 (UTC/London)', 'UTC+1 (Central EU)', 'UTC+5:30 (India)', 'UTC+9 (Japan/Korea)', 'UTC+9:30 (C. Aust.)', 'UTC+12 (NZ)'];
  const MODULES = ['WiFi scan', 'Net stats', 'WiFi IDS', 'Wardrive', 'BLE scan', 'Tracker detect', 'Flock detect', 'Skimmer detect', 'SubGHz sweep', 'Meshtastic', 'Engagement', 'Rogue AP', 'Probe watch', 'Client map', 'Camera detect', 'Drone detect', 'BLE spam watch'];
  let modHidden = MODULES.map(() => false);
  let modPage = 0, modPages = 1;
  let view = 'ROOT';
  let pins = { cc1101: true, radio24: 0, cs1101: 5, cs24: 27, irq24: 26, gdo0: 4 };
  let battMv = 3850, battFactor = 1.015;
  let tzSel = TZ_IDX;
  let stack = [];
  let rootPage = 0, rootPages = 1;
  let powerMenuOpen = false, powerConfirmOpen = false, poweredOff = false;
  let powerPopupBtns = null, powerConfirmBtns = null;

  const TIMEOUT_OPTIONS_MS = [30000, 60000, 180000, 300000, 600000, 900000];
  const TIMEOUT_LABELS = ['30 seconds', '1 minute', '3 minutes', '5 minutes', '10 minutes', '15 minutes'];
  function timeoutIdxFromMs(ms) { const i = TIMEOUT_OPTIONS_MS.indexOf(ms); return i < 0 ? 0 : i; }
  let timeoutNever = DISPLAY_TIMEOUT_MS === 0;
  let timeoutIdx = timeoutIdxFromMs(DISPLAY_TIMEOUT_MS);

  function push(v) { stack.push(view); view = v; }
  function popView() { view = stack.pop() || 'ROOT'; }

  // Same bottom-left corner and puck/ring/stem style as the main menu's
  // gear icon (drawGear()/touchInGear() above) and the real firmware's
  // drawPowerIcon() in system_screen.cpp.
  function powerIconCenter() { return { cx: 18, cy: SCR.h - 18 }; }
  function drawPowerIcon() {
    const { cx, cy } = powerIconCenter();
    const R = 10, PUCK_R = R + 6, RING_R = PUCK_R - 2;
    tft.fillCircle(cx, cy, PUCK_R, C.BLACK);
    for (let rr = RING_R - 2; rr <= RING_R; rr++) tft.drawCircle(cx, cy, rr, C.DARKGREY);
    tft.fillRect(cx - 4, cy - RING_R - 1, 8, 6, C.BLACK);
    tft.fillRect(cx - 1, cy - RING_R - 1, 3, RING_R - 1, C.DARKGREY);
  }
  function touchInPowerIcon(t) {
    const { cx, cy } = powerIconCenter();
    const dx = t.x - cx, dy = t.y - cy, rr = 10 + 8;
    return dx * dx + dy * dy <= rr * rr;
  }
  function drawPowerPopup() {
    const boxW = Math.min(220, SCR.w - 60), boxH = 110;
    const bx = (SCR.w - boxW) / 2, by = (SCR.h - boxH) / 2;
    tft.fillRect(bx, by, boxW, boxH, C.BLACK);
    tft.drawRect(bx, by, boxW, boxH, C.WHITE);
    const sleepBtn = { x: bx + 10, y: by + 12, w: boxW - 20, h: 34, label: 'Sleep' };
    const offBtn = { x: bx + 10, y: by + 56, w: boxW - 20, h: 34, label: 'Power off' };
    uiDrawMenuButton(sleepBtn);
    uiDrawMenuButton(offBtn);
    tft.setTextSize(1); tft.setTextColor(C.DARKGREY); tft.setTextWrap(false);
    const msg = 'tap outside to cancel';
    tft.setCursor(bx + (boxW - tft.textWidthOf(msg)) / 2, by + boxH - 14);
    tft.print(msg);
    powerPopupBtns = { box: { x: bx, y: by, w: boxW, h: boxH }, sleepBtn, offBtn };
  }
  function drawPowerOffConfirm() {
    uiClearBelow(0);
    tft.setTextColor(C.YELLOW); tft.setTextSize(2); tft.setTextWrap(false);
    tft.setCursor(8, 40); tft.print('Power off');
    tft.setTextColor(C.WHITE); tft.setTextSize(1);
    tft.setCursor(8, 76); tft.print('Lowest power mode.');
    tft.setCursor(8, 92); tft.print('Press RESET to power on.');
    const goBtn = { x: 8, y: 140, w: SCR.w - 16, h: 34, label: 'Power off now' };
    const noBtn = { x: 8, y: 184, w: SCR.w - 16, h: 34, label: 'cancel' };
    uiDrawMenuButton(goBtn);
    uiDrawMenuButton(noBtn);
    powerConfirmBtns = { goBtn, noBtn };
  }
  function drawPoweredOff() {
    tft.fillScreen(C.BLACK);
    tft.setTextColor(C.DARKGREY); tft.setTextSize(1); tft.setTextWrap(false);
    const msg = 'Powered off -- tap to restart the demo';
    tft.setCursor((SCR.w - tft.textWidthOf(msg)) / 2, SCR.h / 2 - 4);
    tft.print(msg);
  }

  function rowList(title, items, onTap, footer) {
    uiDrawTopBar(title);
    bgMode = 1;
    uiClearBelow(29);
    let y = 38;
    const btns = items.map((label) => { const b = { x: 8, y, w: SCR.w - 16, h: 26, label }; uiDrawMenuButton(b); y += 30; return b; });
    if (footer) { tft.setTextSize(1); tft.setTextColor(C.WHITE); tft.setTextWrap(false); tft.setCursor(8, y + 6); tft.print(footer); }
    return btns;
  }

  const ROOT_ITEMS = ['Display', 'Hardware', 'Modules', 'Beep volume', 'Run setup wizard', 'About'];
  const draws = {
    ROOT() {
      if (poweredOff) { drawPoweredOff(); return; }
      if (powerConfirmOpen) { drawPowerOffConfirm(); return; }
      // Standard button size (36/6), paginated like the real firmware --
      // 6 standard-size rows don't fit above the status bar in landscape.
      uiDrawTopBar('System'); bgMode = 1; uiClearBelow(29);
      const y0 = 34, rowH = 36, gap = 6, bottomMargin = 36, pagerGap = 6;
      let rowsPerPage = Math.max(1, Math.floor((SCR.h - bottomMargin - UI_PAGER_H - pagerGap - y0 + gap) / (rowH + gap)));
      rowsPerPage = Math.min(rowsPerPage, ROOT_ITEMS.length);
      rootPages = Math.ceil(ROOT_ITEMS.length / rowsPerPage);
      if (rootPage >= rootPages) rootPage = rootPages - 1;
      if (rootPage < 0) rootPage = 0;
      const base = rootPage * rowsPerPage;
      let y = y0;
      this._btns = ROOT_ITEMS.slice(base, base + rowsPerPage).map((label) => {
        const b = { x: 8, y, w: SCR.w - 16, h: rowH, label };
        uiDrawMenuButton(b);
        y += rowH + gap;
        return b;
      });
      const pagerY = y0 + rowsPerPage * (rowH + gap) - gap + pagerGap;
      this._prevBtn = {}; this._nextBtn = {};
      uiDrawPager(pagerY, rootPage, rootPages, this._prevBtn, this._nextBtn);
      drawPowerIcon();
      if (powerMenuOpen) drawPowerPopup();
    },
    ABOUT() {
      uiDrawTopBar('About');
      uiClearBelow(29);
      const qs = 84, qx = (SCR.w - qs) / 2, qy = 40;
      tft.fillRect(qx, qy, qs, qs, C.WHITE);
      ctx.save(); ctx.fillStyle = '#000';
      for (let r = 0; r < 12; r++) for (let c = 0; c < 12; c++) if ((r * 7 + c * 13 + r * c) % 5 === 0) ctx.fillRect(qx + 6 + c * 6, qy + 6 + r * 6, 5, 5);
      ctx.restore();
      this._qr = { x: qx, y: qy, w: qs, h: qs };
      tft.setTextSize(1); tft.setTextWrap(false);
      let y = qy + qs + 8;
      const lines = [['WIFI D_VICE  v0.9', accentLabel()], ['MIT license', C.WHITE], ['github.com/Vybenclave/wifi-d_vice', C.WHITE],
        [sprintf('battery: %d mV  ~%d%%', Math.round(battMv * battFactor), Math.round((battMv * battFactor - 3300) / 9)), C.WHITE],
        ['scan the code for source + license', C.YELLOW]];
      lines.forEach(([s, col]) => { tft.setTextColor(col); tft.setCursor((SCR.w - tft.textWidthOf(s)) / 2, y); tft.print(s); y += 13; });
    },
    MODULES() {
      // Paginated like the real systemShowModules() -- at MOD_N=17 and a
      // 25px row pitch, more than fit on a 240px-tall screen used to just
      // draw off the bottom, unreachable by touch.
      uiDrawTopBar('Modules'); bgMode = 1; uiClearBelow(29);
      const y0 = 38, rowH = 22, gap = 3, bottomMargin = UI_STATUSBAR_H + 4, pagerGap = 6;
      let rowsPerPage = Math.max(1, Math.floor((SCR.h - bottomMargin - UI_PAGER_H - pagerGap - y0 + gap) / (rowH + gap)));
      rowsPerPage = Math.min(rowsPerPage, MODULES.length);
      modPages = Math.ceil(MODULES.length / rowsPerPage);
      if (modPage >= modPages) modPage = modPages - 1;
      if (modPage < 0) modPage = 0;
      const base = modPage * rowsPerPage;
      this._modBase = base;
      let y = y0;
      this._rows = MODULES.slice(base, base + rowsPerPage).map((name, i) => {
        const idx = base + i;
        const b = { x: 8, y, w: SCR.w - 16, h: rowH, label: name };
        uiDrawMenuButton(b);
        tft.fillCircle(SCR.w - 26, y + rowH / 2, modHidden[idx] ? 2 : 6, modHidden[idx] ? C.DIM_GREY : C.GREEN);
        y += rowH + gap;
        return b;
      });
      const pagerY = y0 + rowsPerPage * (rowH + gap) - gap + pagerGap;
      this._modPrevBtn = {}; this._modNextBtn = {};
      uiDrawPager(pagerY, modPage, modPages, this._modPrevBtn, this._modNextBtn);
    },
    HARDWARE() { this._btns = rowList('Hardware', ['SPI / IRQ pins', 'Battery Info', 'Test GPS', 'Format SD card']); },
    PINS() {
      uiDrawTopBar('SPI / IRQ pins'); uiClearBelow(29);
      tft.setTextSize(1); tft.setTextWrap(false);
      tft.drawRect(8, 32, 14, 14, C.WHITE);
      if (pins.cc1101) { tft.drawLine(9, 39, 12, 44, C.GREEN); tft.drawLine(12, 44, 21, 30, C.GREEN); }
      tft.setTextColor(C.WHITE); tft.setCursor(26, 34); tft.print('CC1101 sub-GHz radio installed');
      tft.setTextColor(accentLabel()); tft.setCursor(8, 54); tft.print('2.4 GHz:');
      const opts = ['none', 'nRF24', 'CC2500'];
      this._radio24 = opts.map((label, i) => { const b = { x: 70 + i * 80, y: 52, w: 76, h: 20, label }; if (i === pins.radio24) uiDrawButton(b); else uiDrawButtonDim(b); return b; });
      const rows = [['CC1101 CS', pins.cs1101], ['2.4GHz CS', pins.cs24], ['2.4GHz IRQ', pins.irq24], ['GDO0', pins.gdo0]];
      let y = 84;
      this._pinRows = rows.map(([name, val]) => {
        tft.setTextColor(accentLabel()); tft.setCursor(8, y + 4); tft.print(name);
        tft.setTextColor(C.WHITE); tft.setCursor(110, y + 4); tft.print(val < 0 ? 'none' : 'GPIO ' + val);
        const b = { x: SCR.w - 48, y, w: 40, h: 18, label: 'set', field: name };
        uiDrawButton(b);
        y += 22;
        return b;
      });
      uiDrawMenuButton(this._reset = { x: 8, y: y + 4, w: SCR.w - 16, h: 26, label: 'reset to defaults' });
      tft.setTextColor(C.YELLOW); tft.setCursor(8, y + 36); tft.print('Reboot to apply. BOOT on power-up resets.');
    },
    BATTERY() {
      uiDrawTopBar('Battery Info'); uiClearBelow(29);
      tft.setTextSize(1); tft.setTextColor(C.WHITE); tft.setTextWrap(false);
      const corrected = Math.round(battMv * battFactor);
      const pct = Math.max(0, Math.min(100, Math.round((corrected - 3300) / 9)));
      tft.setCursor(8, 40); tft.print(sprintf('raw:       %4d mV', battMv));
      tft.setCursor(8, 56); tft.print(sprintf('corrected: %4d mV  (~%d%%)', corrected, pct));
      tft.setCursor(8, 72); tft.print(sprintf('factor:    x%.3f', battFactor));
      uiDrawButton(this._minus = { x: 8, y: 100, w: 60, h: 44, label: '-' });
      uiDrawButton(this._plus = { x: SCR.w - 68, y: 100, w: 60, h: 44, label: '+' });
      uiDrawMenuButton(this._cal = { x: 8, y: 154, w: SCR.w - 16, h: 34, label: 'Calibrate V_bat' });
      uiDrawMenuButton(this._resetCal = { x: 8, y: 192, w: SCR.w - 16, h: 26, label: 'Reset cal' });
    },
    GPSTEST() {
      uiDrawTopBar('Test GPS'); uiClearBelow(29);
      tft.setTextSize(1); tft.setTextWrap(false);
      const rows = [['RX pin', 'GPIO35', C.WHITE], ['serial', '412 bytes', C.GREEN], ['NMEA ok', '38', C.GREEN],
        ['sats', '7', C.GREEN], ['fix', 'yes', C.GREEN], ['lat', '30.267200', C.WHITE], ['lon', '-97.743100', C.WHITE],
        ['alt m', '178.4', C.WHITE], ['HDOP', '1.1', C.WHITE], ['UTC', clockString(), C.WHITE], ['dev clock', devTimeStr(), C.GREEN]];
      let y = 34;
      rows.forEach(([k, v, col]) => { tft.setTextColor(accentLabel()); tft.setCursor(4, y); tft.print(k); tft.setTextColor(col); tft.setCursor(100, y); tft.print(v); y += 15; });
    },
    DISPLAY() { this._btns = rowList('Display', ['Screen orientation', 'Theme & Color', 'Timezone', 'Recalibrate touch', SPLASH_ON ? 'Boot splash (on)' : 'Boot splash (off)', 'Display Timeout']); },
    TIMEOUT() {
      uiDrawTopBar('Display Timeout'); bgMode = 0; uiClearBelow(29);
      tft.setTextColor(C.WHITE); tft.setTextSize(1); tft.setTextWrap(false);
      tft.setCursor(8, 38); tft.print('Turn off the display after:');
      const rowBtn = { x: 8, y: 56, w: SCR.w - 16, h: 30, label: TIMEOUT_LABELS[timeoutIdx] };
      const neverBtn = { x: 8, y: 94, w: SCR.w - 16, h: 30, label: timeoutNever ? 'Never (on)' : 'Never (off)' };
      const applyBtn = { x: 8, y: 140, w: SCR.w - 16, h: 34, label: 'Apply' };
      if (timeoutNever) uiDrawButtonDim(rowBtn); else uiDrawMenuButton(rowBtn);
      uiDrawMenuButton(neverBtn);
      uiDrawMenuButton(applyBtn);
      this._timeoutRow = rowBtn; this._timeoutNever = neverBtn; this._timeoutApply = applyBtn;
    },
    TIMEOUTPICK() {
      uiDrawTopBar('Timeout'); uiClearBelow(29);
      let y = 34;
      this._tp = TIMEOUT_LABELS.map((label, i) => {
        const b = { x: 8, y, w: SCR.w - 16, h: 30, label, idx: i };
        uiDrawButton(b);
        if (i === timeoutIdx) tft.drawRoundRect(b.x - 2, b.y - 2, b.w + 4, b.h + 4, 8, C.GREEN);
        y += 34;
        return b;
      });
    },
    ROTATION() {
      uiClearRect(0, 0, SCR.w, SCR.h); uiDrawStatusBar();
      tft.setTextSize(1); tft.setTextColor(C.WHITE); tft.setCursor(4, 4); tft.print('Pick orientation');
      const labels = ['0', '90', '180', '270'];
      const cw = (SCR.w - 24) / 2, ch = (SCR.h - 40 - UI_STATUSBAR_H) / 2;
      this._rot = labels.map((label, i) => {
        const col = i % 2, row = Math.floor(i / 2);
        const b = { x: 8 + col * (cw + 8), y: 20 + row * (ch + 8), w: cw, h: ch, label };
        tft.drawRect(b.x, b.y, b.w, b.h, C.WHITE);
        tft.setTextSize(2); tft.setTextColor(accentLabel());
        tft.setCursor(b.x + b.w / 2 - tft.textWidthOf(label) / 2, b.y + b.h / 2 - 8); tft.print(label);
        return b;
      });
    },
    THEME() {
      uiDrawTopBar('Theme & Color'); uiClearBelow(29);
      uiDrawButton(this._themeBtn = { x: 8, y: 40, w: SCR.w - 16, h: 40, label: 'Theme: ' + (themeIsVice() ? 'Vice' : 'Basic') });
      uiDrawButton(this._accentBtn = { x: 8, y: 88, w: SCR.w - 16, h: 40, label: 'Accent: ' + accent().name });
      const sw = 24, sx = this._accentBtn.x + this._accentBtn.w - sw - 10, sy = this._accentBtn.y + 8;
      if (themeIsVice()) { tft.fillRoundRect(sx, sy, sw, sw, 4, accentFill()); tft.drawRoundRect(sx, sy, sw, sw, 4, accentEdge()); } else tft.drawRect(sx, sy, sw, sw, accentFill());
      uiDrawButton(this._listBgBtn = { x: 8, y: 136, w: SCR.w - 16, h: 40, label: LIST_BG ? 'List screens: blurred scene bg' : 'List screens: gradient bg' });
    },
    ACCENTPICK() {
      uiDrawTopBar('Accent Color'); uiClearBelow(29);
      const cols = 2, gap = 8, cell = 34;
      let y = 34;
      this._sw = ACCENT.map((a, i) => {
        const col = i % cols, row = Math.floor(i / cols);
        const b = { x: 8 + col * ((SCR.w - 16 - gap) / cols + gap), y: y + row * (cell + gap), w: (SCR.w - 16 - gap) / cols, h: cell, id: i };
        if (themeIsVice()) { tft.fillRoundRect(b.x, b.y, b.w, b.h, 6, a.fill); tft.drawRoundRect(b.x, b.y, b.w, b.h, 6, a.edge); }
        else tft.drawRect(b.x, b.y, b.w, b.h, a.fill);
        if (i === ACCENT_ID) tft.drawRoundRect(b.x + 2, b.y + 2, b.w - 4, b.h - 4, 5, C.GREEN);
        tft.setTextSize(1); tft.setTextColor(contrastTextFor(a.fill)); tft.setTextWrap(false);
        tft.setCursor(b.x + b.w / 2 - tft.textWidthOf(a.name) / 2, b.y + b.h / 2 - 6); tft.print(a.name);
        return b;
      });
    },
    TIMEZONE() {
      uiDrawTopBar('Timezone'); uiClearBelow(29);
      const rowH = 22, gap = 3, rowsShown = 5;
      const startPage = Math.floor(tzSel / rowsShown);
      let y = 34;
      this._tzRows = [];
      for (let i = startPage * rowsShown; i < Math.min(TZLIST.length, startPage * rowsShown + rowsShown); i++) {
        const b = { x: 8, y, w: SCR.w - 16, h: rowH, idx: i, label: TZLIST[i] };
        uiDrawButton(b);
        if (i === tzSel) tft.drawRoundRect(b.x, b.y, b.w, b.h, 8, C.CYAN);
        this._tzRows.push(b);
        y += rowH + gap;
      }
      y += 4;
      uiDrawPager(y, startPage, Math.ceil(TZLIST.length / rowsShown), this._prev = {}, this._next = {});
      y += UI_PAGER_H + 4;
      uiDrawMenuButton(this._h24 = { x: 8, y, w: SCR.w - 16, h: 26, label: TZ_24H ? '24-hour clock' : '12-hour clock' });
      y += 30;
      uiDrawMenuButton(this._dst = { x: 8, y, w: SCR.w - 16, h: 26, label: TZ_AUTODST ? 'Auto DST: on' : 'Auto DST: off' });
      y += 32;
      uiDrawMenuButton(this._apply = { x: 8, y, w: SCR.w - 16, h: 30, label: tzSel === TZ_IDX ? 'Apply (no change)' : 'Apply' });
    },
    BEEP() {
      uiDrawTopBar('Beep volume'); uiClearBelow(29);
      tft.setTextSize(3); tft.setTextColor(C.WHITE); tft.setTextWrap(false);
      const s = sprintf('%3d%%', BEEP_VOL);
      tft.setCursor((SCR.w - tft.textWidthOf(s)) / 2, 50); tft.print(s);
      uiDrawButton(this._minus = { x: 8, y: 110, w: 60, h: 44, label: '-' });
      uiDrawButton(this._plus = { x: SCR.w - 68, y: 110, w: 60, h: 44, label: '+' });
      uiDrawMenuButton(this._test = { x: 8, y: 164, w: SCR.w - 16, h: 40, label: 'test beep' });
    },
  };

  function devTimeStr() { const d = new Date(); return d.toISOString().slice(0, 19).replace('T', ' '); }

  Screens.SYSTEM = {
    enter() {
      view = 'ROOT'; stack = []; rootPage = 0;
      powerMenuOpen = false; powerConfirmOpen = false; poweredOff = false;
      timeoutNever = DISPLAY_TIMEOUT_MS === 0; timeoutIdx = timeoutIdxFromMs(DISPLAY_TIMEOUT_MS);
      this.draw();
    },
    draw() { (draws[view] || draws.ROOT).call(this); },
    frame() {
      this.draw();
      if (!touch.isNewPress) return;
      const t = touch;
      switch (view) {
        case 'ROOT':
          if (poweredOff) { poweredOff = false; break; }
          if (powerConfirmOpen) {
            if (hit(t, powerConfirmBtns.noBtn)) powerConfirmOpen = false;
            else if (hit(t, powerConfirmBtns.goBtn)) poweredOff = true;
            break;
          }
          if (powerMenuOpen) {
            if (hit(t, powerPopupBtns.sleepBtn)) {
              powerMenuOpen = false;
              currentScreen = 'MENU';   // "Sleep" wakes to the main menu, not back here -- same as the real firmware
              displayBlanked = true;    // the MENU dispatch below redraws on its own once this clears
            } else if (hit(t, powerPopupBtns.offBtn)) {
              powerMenuOpen = false; powerConfirmOpen = true;
            } else if (!hit(t, powerPopupBtns.box)) {
              powerMenuOpen = false;   // tapped outside the box -- cancel
            }
            break;
          }
          if (touchInPowerIcon(t)) { powerMenuOpen = true; break; }
          if (hit(t, this._prevBtn) && rootPage > 0) { rootPage--; break; }
          if (hit(t, this._nextBtn) && rootPage < rootPages - 1) { rootPage++; break; }
          for (const b of this._btns) if (hit(t, b)) {
            if (b.label === 'Display') push('DISPLAY');
            else if (b.label === 'Hardware') push('HARDWARE');
            else if (b.label === 'Modules') { modPage = 0; push('MODULES'); }
            else if (b.label === 'Beep volume') push('BEEP');
            else if (b.label === 'Run setup wizard') uiToast('setup wizard (skipped in sim)');
            else if (b.label === 'About') push('ABOUT');
          }
          break;
        case 'ABOUT':
          if (this._qr && hit(t, this._qr)) uiToast('splash art (tap to dismiss)');
          break;
        case 'MODULES':
          if (hit(t, this._modPrevBtn) && modPage > 0) { modPage--; break; }
          if (hit(t, this._modNextBtn) && modPage < modPages - 1) { modPage++; break; }
          for (let i = 0; i < this._rows.length; i++) if (hit(t, this._rows[i])) { const idx = this._modBase + i; modHidden[idx] = !modHidden[idx]; }
          break;
        case 'HARDWARE':
          for (const b of this._btns) if (hit(t, b)) {
            if (b.label === 'SPI / IRQ pins') push('PINS');
            else if (b.label === 'Battery Info') push('BATTERY');
            else if (b.label === 'Test GPS') push('GPSTEST');
            else if (b.label === 'Format SD card') askText('Type ERASE to wipe the SD card', '', (v) => { if (v === 'ERASE') uiToast('SD card erased'); });
          }
          break;
        case 'PINS':
          if (hit(t, { x: 8, y: 32, w: 14, h: 14 })) pins.cc1101 = !pins.cc1101;
          for (const b of this._radio24 || []) if (hit(t, b)) pins.radio24 = ['none', 'nRF24', 'CC2500'].indexOf(b.label);
          for (const b of this._pinRows || []) if (hit(t, b)) askText(`${b.field} GPIO (-1 = none)`, '', (v) => { const n = parseInt(v, 10); if (!isNaN(n) && n >= -1 && n <= 39) uiToast('pin set'); else uiToast('GPIO must be -1..39'); });
          if (this._reset && hit(t, this._reset)) uiToast('pins reset to defaults');
          break;
        case 'BATTERY':
          if (hit(t, this._minus)) battFactor = Math.max(0.5, battFactor - 0.005);
          else if (hit(t, this._plus)) battFactor += 0.005;
          else if (hit(t, this._cal)) askText('Pack mV read on a multimeter (4200 = full)', '4200', (v) => { const mv = parseInt(v, 10); if (mv >= 2500 && mv <= 4400) battFactor = mv / battMv; });
          else if (hit(t, this._resetCal)) battFactor = 1.0;
          break;
        case 'DISPLAY':
          for (const b of this._btns) if (hit(t, b)) {
            if (b.label === 'Screen orientation') push('ROTATION');
            else if (b.label === 'Theme & Color') push('THEME');
            else if (b.label === 'Timezone') { tzSel = TZ_IDX; push('TIMEZONE'); }
            else if (b.label === 'Recalibrate touch') uiToast('touch recalibrated');
            else if (b.label === 'Display Timeout') {
              timeoutNever = DISPLAY_TIMEOUT_MS === 0; timeoutIdx = timeoutIdxFromMs(DISPLAY_TIMEOUT_MS); push('TIMEOUT');
            } else { SPLASH_ON = !SPLASH_ON; store.set('splash', SPLASH_ON); }
          }
          break;
        case 'TIMEOUT':
          if (hit(t, this._timeoutRow) && !timeoutNever) push('TIMEOUTPICK');
          else if (hit(t, this._timeoutNever)) timeoutNever = !timeoutNever;
          else if (hit(t, this._timeoutApply)) {
            DISPLAY_TIMEOUT_MS = timeoutNever ? 0 : TIMEOUT_OPTIONS_MS[timeoutIdx];
            store.set('dispTimeoutMs', DISPLAY_TIMEOUT_MS);
            noteActivity();
            popView();
          }
          break;
        case 'TIMEOUTPICK':
          for (const b of this._tp || []) if (hit(t, b)) { timeoutIdx = b.idx; popView(); }
          break;
        case 'ROTATION':
          for (const b of this._rot || []) if (hit(t, b)) { const idx = ['0', '90', '180', '270'].indexOf(b.label); ROTATION = idx; store.set('rotation', idx); setOrientation(idx); popView(); }
          break;
        case 'THEME':
          if (hit(t, this._themeBtn)) { THEME = THEME ? 0 : 1; store.set('theme', THEME); }
          else if (hit(t, this._accentBtn)) push('ACCENTPICK');
          else if (hit(t, this._listBgBtn)) { LIST_BG = !LIST_BG; store.set('listbg', LIST_BG); }
          break;
        case 'ACCENTPICK':
          for (const b of this._sw || []) if (hit(t, b)) { ACCENT_ID = b.id; store.set('accent', b.id); popView(); }
          break;
        case 'TIMEZONE':
          for (const b of this._tzRows || []) if (hit(t, b)) tzSel = b.idx;
          if (this._h24 && hit(t, this._h24)) { TZ_24H = !TZ_24H; }
          else if (this._dst && hit(t, this._dst)) { TZ_AUTODST = !TZ_AUTODST; }
          else if (this._apply && hit(t, this._apply)) { TZ_IDX = tzSel; store.set('tzidx', tzSel); store.set('tz24h', TZ_24H); store.set('tzdst', TZ_AUTODST); uiToast('timezone applied'); }
          else if (this._prev && hit(t, this._prev) && tzSel >= 5) tzSel -= 5;
          else if (this._next && hit(t, this._next) && tzSel + 5 < TZLIST.length) tzSel += 5;
          break;
        case 'BEEP':
          if (hit(t, this._minus)) { BEEP_VOL = Math.max(0, BEEP_VOL - 10); store.set('beepvol', BEEP_VOL); beep(200, 1800); }
          else if (hit(t, this._plus)) { BEEP_VOL = Math.min(100, BEEP_VOL + 10); store.set('beepvol', BEEP_VOL); beep(200, 1800); }
          else if (hit(t, this._test)) beep(200, 1800);
          break;
      }
    },
    handleBack() { if (view === 'ROOT') return false; popView(); return true; },
  };
})();

// -------------------------------------------------------------------- boot
setOrientation(ROTATION);
resizeCanvas();
bootSequence();
assetsReady.then(() => {});
requestAnimationFrame(mainLoop);
