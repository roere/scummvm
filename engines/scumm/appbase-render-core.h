/* Bounded, allocation-free HD coordinate helpers. GPL-3.0-or-later. */
#ifndef SCUMM_APPBASE_RENDER_CORE_H
#define SCUMM_APPBASE_RENDER_CORE_H
namespace Scumm {
inline bool validAppbaseRenderScale(int scale) { return scale >= 1 && scale <= 8; }
inline bool validAppbaseHDCel(unsigned costume, unsigned limb, unsigned code,
 unsigned width, unsigned height, unsigned density, unsigned reserved) {
 return costume >= 1 && costume <= 255 && limb <= 15 && code <= 127 && width >= 1 &&
  width <= 320 && height >= 1 && height <= 200 && density >= 1 && density <= 8 && !reserved;
}
inline unsigned appbaseHDSourcePixel(unsigned width, unsigned density, unsigned scale,
 unsigned x, unsigned y, unsigned subX, unsigned subY, bool forward) {
 const unsigned column = forward ? subX : scale - 1 - subX;
 return (y * density + subY * density / scale) * width * density + x * density + column * density / scale;
}
}
#endif
