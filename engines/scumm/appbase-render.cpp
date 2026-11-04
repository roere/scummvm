/* Optional high-density classic SCUMM presentation. GPL-3.0-or-later. */
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif
#include "scumm/appbase-render.h"
#include "scumm/appbase-render-core.h"
#include "common/endian.h"
#include "common/file.h"
#include "common/fs.h"
#include "common/textconsole.h"
namespace Scumm {
void AppbaseRender::configure(int scale, const Common::String &path) {
 if (!validAppbaseRenderScale(scale)) error("AppBase render scale must be an integer from 1 to 8");
 _scale = scale;
 // The original path neither loads HD artwork nor allocates a render surface.
 if (_scale == 1 || path.empty()) return;
 Common::File f;
 if (!f.open(Common::FSNode(Common::Path(path))) || f.size() < 8 || f.size() > 16777216)
  error("Invalid AppBase HD costume file");
 if (f.readUint32BE() != MKTAG('A','B','H','D') || f.readUint16LE() != 1)
  error("Unsupported AppBase HD costume format");
 const unsigned count = f.readUint16LE();
 if (!count || count > 512) error("Invalid AppBase HD cel count");
 for (unsigned n = 0; n < count; ++n) {
  if (f.size() - f.pos() < 12) error("Truncated AppBase HD cel header");
  Cel c; c.costume = f.readUint16LE(); c.limb = f.readByte(); c.code = f.readByte();
  c.width = f.readUint16LE(); c.height = f.readUint16LE(); c.density = f.readByte();
  const unsigned reserved = f.readByte() | f.readUint16LE();
  if (!validAppbaseHDCel(c.costume, c.limb, c.code, c.width, c.height, c.density, reserved))
   error("Invalid AppBase HD cel dimensions or identity");
  for (const Cel &old : _cels)
   if (old.costume == c.costume && old.limb == c.limb && old.code == c.code)
    error("Duplicate AppBase HD cel");
  const unsigned size = c.width * c.height * c.density * c.density;
  if (f.size() - f.pos() < size) error("Truncated AppBase HD pixels");
  c.pixels.resize(size);
  if (f.read(c.pixels.data(), size) != size || f.err()) error("Cannot read AppBase HD pixels");
  for (byte pixel : c.pixels) if (pixel > 15) error("HD pixels must use native costume indices 0..15");
  _cels.push_back(c);
 }
 if (f.pos() != f.size()) error("Trailing AppBase HD costume data");
 _pixels.resize(320 * 200 * _scale * _scale);
 _active.resize(320 * 200); _native.resize(320 * 200);
 reset();
}
void AppbaseRender::reset() {
 _drawingHD = false;
 if (!_active.empty()) memset(_active.data(), 0, _active.size());
}
void AppbaseRender::beginFrame() { reset(); }
bool AppbaseRender::drawCel(unsigned costume, unsigned limb, unsigned code,
 int width, int height, int x, int y, bool forward, byte scaleX, byte scaleY,
 byte indexX, byte indexY, const byte *scaleTable, const uint16 *palette,
 const Graphics::Surface &out, const byte *mask, int strips, int scrollOffset) {
 const Cel *cel = nullptr;
 for (const Cel &c : _cels) if (c.costume == costume && c.limb == limb && c.code == code) { cel = &c; break; }
 if (!cel) return false;
 if (cel->width != (unsigned)width || cel->height != (unsigned)height)
  error("HD cel does not match its native costume dimensions");
 const int step = forward ? 1 : -1;
 for (int sx = 0, dx = x; sx < width; ++sx) {
  byte iy = indexY;
  for (int sy = 0, dy = y; sy < height; ++sy) {
   if (scaleY != 255 && scaleTable[iy++] >= scaleY) continue;
   const int vx = dx - scrollOffset;
   if (dx >= 0 && dx < out.w && vx >= 0 && vx < 320 && dy >= 0 && dy < out.h && dy < 200 &&
       (!mask || !(mask[dy * strips + dx / 8] & (0x80 >> (dx & 7))))) {
    const unsigned cell = dy * 320 + vx;
    if (!_active[cell]) {
     _active[cell] = 1; _native[cell] = *(const byte *)out.getBasePtr(dx, dy);
     for (int yy = 0; yy < _scale; ++yy)
      memset(_pixels.data() + (dy * _scale + yy) * 320 * _scale + vx * _scale, _native[cell], _scale);
    }
    for (int yy = 0; yy < _scale; ++yy) for (int xx = 0; xx < _scale; ++xx) {
     const byte color = cel->pixels[appbaseHDSourcePixel(width, cel->density, _scale, sx, sy, xx, yy, forward)];
     if (color) _pixels[(dy * _scale + yy) * 320 * _scale + vx * _scale + xx] = palette[color];
    }
   }
   ++dy;
  }
  if (scaleX == 255 || scaleTable[indexX] < scaleX) dx += step;
  indexX += step;
 }
 _drawingHD = true; ++_drawn;
 return true;
}
void AppbaseRender::nativePixel(int x, int y, byte color) {
 if (_active.empty() || x < 0 || x >= 320 || y < 0 || y >= 200) return;
 const unsigned cell = y * 320 + x;
 if (!_active[cell]) return;
 _native[cell] = color;
 if (!_drawingHD) for (int yy = 0; yy < _scale; ++yy)
  memset(_pixels.data() + (y * _scale + yy) * 320 * _scale + x * _scale, color, _scale);
}
const byte *AppbaseRender::compose(const byte *src, int pitch, int x, int y,
 int width, int height, bool stage, int stageTop, const Graphics::Surface &text) {
 _scratch.resize(width * height * _scale * _scale);
 for (int yy = 0; yy < height; ++yy) for (int xx = 0; xx < width; ++xx) {
  const byte color = src[yy * pitch + xx];
  const int vx = x + xx, vy = y + yy - stageTop;
  const bool hd = stage && !_active.empty() && vx >= 0 && vx < 320 && vy >= 0 && vy < 200 &&
   _active[vy * 320 + vx] && color == _native[vy * 320 + vx] &&
   (x + xx < 0 || y + yy < 0 || x + xx >= text.w || y + yy >= text.h ||
    *(const byte *)text.getBasePtr(x + xx, y + yy) == 253);
  for (int sy = 0; sy < _scale; ++sy) {
   byte *dst = _scratch.data() + (yy * _scale + sy) * width * _scale + xx * _scale;
   if (hd) memcpy(dst, _pixels.data() + (vy * _scale + sy) * 320 * _scale + vx * _scale, _scale);
   else memset(dst, color, _scale);
  }
 }
 return _scratch.data();
}
}

#include "scumm/scumm.h"
#include "scumm/actor.h"
namespace Scumm {
void ScummEngine::appbaseRenderCopy(const void *src, int pitch, int x, int y, int width, int height, bool stage) {
 const int scale = _appbaseRender.scale();
 if (scale == 1) { _system->copyRectToScreen(src, pitch, x, y, width, height); return; }
 if (width <= 0 || height <= 0) return;
 const byte *pixels = _appbaseRender.compose((const byte *)src, pitch, x, y, width, height,
  stage, _virtscr[kMainVirtScreen].topline - _screenTop, _textSurface);
 _system->copyRectToScreen(pixels, width * scale, x * scale, y * scale, width * scale, height * scale);
}
void ScummEngine::appbaseRenderBegin() {
 if (!_appbaseRender.hasHD()) return;
 _appbaseRender.beginFrame();
 // Draw-only refresh: do not invoke scripts, walking or animation an extra time.
 for (int i = 1; i < _numActors; ++i) { _actors[i]->_needRedraw = true; _actors[i]->_needBgReset = true; }
}
void ScummEngine::appbaseRenderPixel(int x, int y, byte color) {
 _appbaseRender.nativePixel(x - (_virtscr[kMainVirtScreen].xstart & 7), y, color);
}
}
#ifdef __EMSCRIPTEN__
extern "C" {
EMSCRIPTEN_KEEPALIVE int scumm_render_version() { return 1; }
EMSCRIPTEN_KEEPALIVE int scumm_render_get(int field) {
 if (!Scumm::g_scumm) return -1;
 const Scumm::AppbaseRender &r = Scumm::g_scumm->_appbaseRender;
 switch(field) { case 0: return r.scale(); case 1: return 320 * r.scale(); case 2: return 200 * r.scale();
 case 3: return r.frameCount(); case 4: return r.drawn(); default: return -1; }
}
}
#endif
