/* Optional high-density classic SCUMM presentation. GPL-3.0-or-later. */
#ifndef SCUMM_APPBASE_RENDER_H
#define SCUMM_APPBASE_RENDER_H
#include "common/array.h"
#include "common/str.h"
#include "graphics/surface.h"
namespace Scumm {
class AppbaseRender {
 struct Cel { unsigned costume, limb, code, width, height, density; Common::Array<byte> pixels; };
 int _scale = 1;
 Common::Array<Cel> _cels;
 Common::Array<byte> _pixels, _active, _native, _scratch;
 bool _drawingHD = false;
 unsigned _drawn = 0;
public:
 int scale() const { return _scale; }
 unsigned frameCount() const { return _cels.size(); }
 unsigned drawn() const { return _drawn; }
 bool hasHD() const { return !_cels.empty(); }
 void configure(int scale, const Common::String &path);
 void beginFrame();
 void reset();
 bool drawCel(unsigned costume, unsigned limb, unsigned code, int width, int height,
  int x, int y, bool forward, byte scaleX, byte scaleY, byte indexX, byte indexY,
  const byte *scaleTable, const uint16 *palette, const Graphics::Surface &out,
  const byte *mask, int strips, int scrollOffset);
 void endCel() { _drawingHD = false; }
 void nativePixel(int x, int y, byte color);
 const byte *compose(const byte *src, int pitch, int x, int y, int width, int height,
  bool stage, int stageTop, const Graphics::Surface &text);
};
}
#endif
