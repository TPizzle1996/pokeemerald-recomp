#ifndef GUARD_FRAMEDRAW_H
#define GUARD_FRAMEDRAW_H

#include "global.h"

void DrawFrame(uint16_t *pixels);

#ifdef RENDERER_EASY_DRAW
/* Debug-only oracle seam for the runtime parity capture
 * (POKEEMERALD_NATIVE_PARITY=1). When gParityBGPixelsBuffer is non-NULL,
 * DrawFrame additionally renders a BG-only oracle (OBJ excluded, no
 * DMA/interrupt side effects) into the pointed-to DISPLAY_WIDTH*DISPLAY_HEIGHT
 * u16 buffer on the same live frame state; gParityBGPixelLayers (u8 buffer)
 * receives the winning layer per oracle pixel (0=backdrop, 1=BG0, 2=BG1,
 * 3=BG2, 4=BG3). gParityOracleProduced is set to 1 by DrawFrame when the oracle
 * was actually rendered this frame (buffer was non-NULL throughout), so the host
 * can tell "oracle produced" apart from "capture scheduled but seam did not run".
 * Normal builds keep both buffers NULL and DrawFrame behaves exactly as before. */
extern uint16_t *gParityBGPixelsBuffer;
extern uint8_t *gParityBGPixelLayers;
extern uint8_t gParityOracleProduced;
/* OBJ oracle seam (Stage 3B). When gParityOBJLayers is non-NULL, DrawSprites
 * copies its four per-scanline per-OBJ-priority spriteLayers into the flat
 * buffer [priority][scanline][x] (main pass only; the BG-only oracle re-render
 * skips sprites). Captured colors are the oracle's pre-composite OBJ layers
 * exactly as presented, bit 15 presence set. gParityOBJOracleProduced is set
 * once the seam copied at least one scanline this frame. Normal builds keep the
 * pointer NULL and the renderer behaves exactly as before. */
extern uint16_t *gParityOBJLayers;
/* Stage 3E pre-blend OBJ oracle seam. When gParityOBJPreBlendLayers is non-NULL
 * (and gParityOBJLayers is also non-NULL), the OBJ seam additionally copies the
 * RAW pre-blend/pre-brightness OBJ colors (bit 15 presence set) into the same
 * [priority][scanline][x] layout, so the OBJ-only harness can compare the texel
 * color the oracle sampled for semi-transparent OBJ without the final blend. */
extern uint16_t *gParityOBJPreBlendLayers;
extern uint8_t gParityOBJOracleProduced;
/* Composite oracle seam (Stage 3C). When gParityCompositeLayers is non-NULL,
 * the MAIN DrawScanline pass (BG + OBJ + backdrop, skipSprites=false) also
 * records the final-pixel winner into the flat u8 buffer per scanline:
 * 0=backdrop, 1=BG0, 2=BG1, 3=BG2, 4=BG3, 5-8=OBJ priority 0-3. This is the
 * per-pixel source map of the REAL GBA composite, comparable against the native
 * composite's winner map. gParityCompositeProduced is set to 1 once DrawFrame
 * ran with the seam active. Normal builds keep the pointer NULL and the main
 * pass behaves exactly as before. */
extern uint8_t *gParityCompositeLayers;
extern uint8_t gParityCompositeProduced;
#endif
#endif
