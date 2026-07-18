# Primary hardware documentation

These are the authoritative sources for all register programming in L10GL.
Implementing agents: cite section numbers from these documents in code
comments, and verify claims here before writing register code — do not
rely on training-data memory of these chips.

## DB019-B_ViRGE_Integrated_3D_Accelerator_Aug1996.pdf

S3 ViRGE (86C325) databook, DB019-B, August 1996. 384 pages, clean text
layer (searchable with `pdftotext`). The datasheet's own page labels
(e.g. "15-18") don't match PDF page numbers; index of the sections this
project uses, by **absolute PDF page**:

| Topic | Datasheet § | PDF pages |
|---|---|---|
| RAMDAC color modes (8/15/16/24bpp scanout) | 8.1–8.2 | 61–63 |
| Clock synthesis, PLL programming (SR12/SR13) | 9.1–9.2 | 65–67 |
| Streams processor, double buffering | 10.1.5 | 71–73 |
| Chip wakeup, register unlock, mode setup | 13.1–13.4 | 89–101 |
| New MMIO, linear addressing | 15.1–15.2 | 105–108 |
| S3d init, autoexecute | 15.4.2–15.4.3 | 110 |
| BitBLT programming example | 15.4.4.1 | 110–120 |
| Rectangle fill example | 15.4.4.2 | 121 |
| 2D line draw example (incl. X-major ½-pixel rule) | 15.4.4.3 | 122–124 |
| 2D polygon fill example | 15.4.4.4 | 125–126 |
| 3D triangle drawing (Figure 15-6) | 15.4.5.2 | 127–129 |
| Z-buffering (compare codes, enable conditions) | 15.4.6 | 129 |
| MUX buffering | 15.4.7 | 130 |
| Texture filtering / formats / lighting modes | 15.4.8.1–.3 | 131–133 |
| Fogging, alpha blending (incl. mutual exclusion) | 15.4.8.4–.5 | 134 |
| Extended CRTC: CR31 (start addr bits, ENH MAP) | 18 | 192–193 |
| Extended CRTC: CR36 (memory size straps) | 18 | 197 |
| Extended CRTC: CR53 (MMIO select) | 18 | 205–206 |
| Extended CRTC: CR67 (color mode, streams) | 18 | 215–216 |
| 2D CMD_SET register (MMA500/A900/AD00) | 19.3 | 232–235 |
| 3D CMD_SET register (MMB100/B500) | 19.4 | 250–252 |
| 3D triangle edge registers (B560–B57C) | 19.4 | 270–274 |
| Subsystem Status/Control (MM8504, FIFO slots) | 22 | 299–301 |

Key facts verified against this document (2026-07):

- **DCLK PLL programming** (section 9.1-9.2, PDF pp.65-67):
  `fout = (M+2) * fref / ((N+2) * 2^R)`, with programmed M=1..127,
  N=1..31, R=0..3, and the pre-divider VCO constrained to 135-270 MHz.
  The normal XIN crystal is 14.318 MHz. N and R occupy SR12 bits 4-0 and
  6-5; M occupies SR13 bits 6-0. After SR12/SR13 are written and 3C2 bits
  3-2 select the programmable clock, toggling SR15 bit 5 0->1->0 loads the
  new clock immediately. P6a implements and unit-tests the calculation only;
  the load sequence is intentionally deferred to the opt-in hardware step.
- **2D line command code is 0011b** in CMD_SET bits 30-27 (example on
  PDF p. 124 writes `0001 100S...` to MMA900), with DE (bit 5) set and
  mono pattern (bit 8) *not* set. Kick register with autoexecute is
  MMA97C (Y count/direction).
- **3D 16bpp destination format is ZRGB1555 only** (PDF p. 250). The 2D
  engine's 16bpp format is "RGB1555 or RGB565" — i.e. format-agnostic
  (PDF p. 232). Scanout format is chosen by CR67 bits 7-4: Mode 9 =
  15-bit, Mode 10 = 16-bit (PDF p. 216).
- **Z compare codes** (CMD_SET bits 22-20): 000 never … 110 ≤ … 111
  always, comparing Zs <op> Zzb (PDF p. 129). Z-buffering is active iff
  ZB MODE (bits 25-24) = 00b and the compare code ≠ 000b.
- **Blend control** (bits 19-18): 10b = texture alpha, 11b = source
  alpha. Fog (bit 17) and source-alpha blending are mutually exclusive
  (PDF p. 134).
- **Maximum texture side is 512 texels** (section 19.4, PDF p. 251).
  CMD_SET bits 11-8 hold `s`, one side of the largest texture rectangle is
  `2^s`, and the document explicitly caps `s` at 9. The native ViRGE path
  therefore requires square power-of-two textures no larger than 512x512.
- **CR36 bits 7-5**: 000 = 4MB, 100 = 2MB, all other values reserved
  (PDF p. 197). Bits are strap-sampled; writable only after CR39 unlock.
- **MM8504 read**: bit 13 = 1 means S3d engine *idle*; bits 12-8 = S3d
  FIFO slots free, FIFO is 16 deep (PDF p. 300).
- **Display start address**: CRC/CRD (bits 15-0) + CR31 bits 5-4 (16-17)
  + CR51 bits 1-0 (18-19); a non-zero CR69 bits 3-0 supersedes the
  CR31/CR51 extension bits entirely (PDF p. 193). **The unit is a DWORD
  (byte offset /4)** with ENH MAP set: CR31 bit 3 "causes the use of
  doubleword memory addressing mode" (PDF p. 193). Hardware-confirmed on
  DX via `fliptest` (2026-07-07): cycling the start address through
  byte divisors x1/x2/x4/x8 of the back-buffer offset, only **x4**
  produced a clean full-screen page flip (x8 sheared, x2 hit the Z
  region, x1 hit end-of-VRAM). This is the CRTC page-flip register for
  double-buffering.
- **Split-screen line compare does not require CR5E bit 6 for L10GL's fixed
  modes.** CR18 supplies bits 7-0, CR07 bit 4 supplies bit 8, CR09 bit 6
  supplies bit 9, and CR5E bit 6 supplies the ViRGE extension bit 10
  (sections 16/18, PDF pp.161/214). Programming `0x3ff` already places the
  split beyond every supported raster (all are shorter than 1024 lines), so
  the native image leaves CR5E.6 clear. This matches the live 800x600
  hardware readback (`CR5E=00`) and avoids an unnecessary extended vertical
  state change.
- **VSY INT latch needs VSY ENB.** Subsystem Status bit 0 (VSY INT) is
  interrupt *status* that only reports an interrupt that is *enabled*;
  Subsystem Control bit 8 (VSY ENB) must be set or the latch never sets
  and a clear-then-poll vsync wait spins to its timeout every call
  (PDF pp. 299-301). Hardware-confirmed on DX via `fliptest`: VSY INT
  sets within ~60ms with VSY ENB, never without. 0x3DA bit 3 (Input
  Status #1, live vertical retrace) works as an independent vsync
  source and is already covered by the driver's `ioperm(0x3C0,0x20,1)`.
- **Hardware-established (NOT in the datasheet): on DX silicon, 2D
  command kicks reset the 3D `VIRGE_3D_Z_STRIDE` (0xB4E8) to its
  all-ones default 0xFF8 (4088).** `virge_clear_z` and `virge_fill_rect`
  (2D BitBLT bank) invalidate the 3D triangle bank's Z stride, so the
  one-time-at-init programming does not survive the 2D ops between init
  and the first 3D draw — the 3D Z fetch then uses stride 4088 while the
  2D Z clear wrote stride 1600, cutting the image at the row where the
  mismatch starts failing the Z test. 86Box does not reproduce this (it
  models the 2D and 3D register files as separate). Fix in L10GL: re-arm
  the 3D dest/Z/stride/clip state before every 3D primitive.
- The exact 3D triangle sub-pixel setup formulas are **not** in this
  document — §15.4.5.2 states setup code "will be provided by S3 to
  customers". Register descriptions define TXEND01/12 as the X of the
  *last pixel drawn* on the side (S11.20, sign must be 0), PDF
  pp. 270-271. For derivation, consult the XFree86 `s3virge` driver
  sources and validate on hardware.

## DB025-A_ViRGE_VX_Integrated_3D_Accelerator_Jun1996.pdf

S3 ViRGE/VX databook, DB025-A, June 1996. Clean text layer
(`pdftotext -layout`). The VX is a VRAM-based sibling of the base
ViRGE, and — importantly — its CR36 "Configuration 1" register lays the
memory-size field out *differently* from the base 86C325 (DB019-B).

Key facts verified against this document (2026-07), by section label
(the datasheet's own labels, not PDF page numbers):

- **CR36 MEM SIZE is bits 6-5 on the VX — NOT bits 7-5 as on the base
  86C325.** From §18 "Extended CRTC Register Descriptions" (CR36
  "Configuration 1", p.18-7) and Table 5-1 (PD[28:0] strap definitions):
  00 = 2MB, 01 = 4MB, 10 = 6MB, 11 = 8MB. Bit 7 is a *separate* strap
  — "8-C — 8-Column Block Write Support" (0/1) — not part of MEM SIZE,
  which is why the field is 2 bits wide vs. the base chip's 3-bit field.
  A naive bits-7-5 read mis-decodes the VX: 86Box programs VX CR36 =
  0x00/0x20/0x60 for 2/4/8MB, which decode correctly as 00/01/11 in
  bits 6-5 but as 000/001/011 in bits 7-5 (the wrong field).
- **VRAM/DRAM split (§7, Table 7-1).** The VX frame buffer may be
  VRAM-only or mixed VRAM+DRAM; the *total* is CR36 bits 6-5, and the
  DRAM off-screen portion (used for 3D Z-buffering) is CR37 bits 6-5.
  L10GL's `virge_detect_vram` reports the CR36 total only.
- **CR36 is writable only after CR39 = 0xA5** (same unlock convention as
  the base chip); the power-on strap is readable without it.

No DX/GX datasheet is on hand (DB019-B is base-only, DB025-A is
VX-only), so the DX/GX MEM SIZE field — assumed bits 7-5 from 86Box,
matching the base table — must be confirmed by reading CR36 on real
DX/GX hardware. GX2/MX/MX+ layouts are undocumented here and in 86Box,
so those device ids fall back to the conservative 2MB assumption.

## Behavioral reference: 86Box ViRGE emulation

The 86Box emulator (https://github.com/86Box/86Box, file
`src/video/vid_s3_virge.c`) implements the S3d engine accurately enough
to run period S3 drivers and games, which makes it executable
documentation for the semantics DB019-B withholds ("code will be
provided by S3 to customers"). **License caution: 86Box is GPL-2.0 and
L10GL is MIT — consult it for register semantics, never copy code.**

Triangle rasterization semantics derived from its `s3_virge_triangle()`
/ `tri()` (paraphrased, verified 2026-07 against master):

- The engine walks scanlines **upward** from TYS, decrementing Y. It
  draws SCAN_LINE_COUNT_01 lines using TdXdY01 for the span-end edge,
  then **reloads the span-end accumulator with TXEND12** and draws
  SCAN_LINE_COUNT_12 more lines using TdXdY12. The span-start
  accumulator starts at TXS, steps by TdXdY02 every line, and is never
  reloaded.
- Therefore the registers mean: TXS = X of edge 02 **at the first
  (bottom) scanline**; TXEND01 = X of edge 01 **at that same first
  scanline** (≈ bottom-vertex X, *not* the middle vertex); TXEND12 = X
  of edge 12 **at the middle vertex's scanline** (*not* the top vertex).
  Read the datasheet's "X of the last pixel drawn for the side" as "the
  per-scanline span-end X, whose accumulator this side owns".
- Span extent per scanline: first pixel = ceil(x_start) (computed as
  `(x + 0xFFFFF) >> 20`), end-exclusive at ceil(x_end). For
  right-to-left rendering (L/R = 0) both bounds shift by −1 and
  iteration runs downward.
- Attribute base values (TZS, TRS/TGS/TBS/TAS, TUS/TVS/TWS/TDS) are
  sampled at the (TXS, TYS) position. Each scanline the engine adds the
  programmed Y deltas to these bases; within a span it additionally
  adjusts the base by the sub-pixel distance from the edge-02
  accumulator to the first pixel, using the top 5 fractional bits of
  the S11.20 X (delta × frac/32). **X-direction sub-pixel correction is
  done by the hardware** — the driver must not pre-add it.
- Consequently the programmed **Y deltas are edge-walk deltas along
  side 02, not plane ∂/∂y**: with y-down screen coordinates and plane
  gradients (dA/dx, dA/dy), program
  `TdAdY = −dA/dy + slope02 · dA/dx`, where slope02 = TdXdY02 / 2^20 —
  the base moves one scanline up *and* along edge 02 each step.
- Driver-side sub-pixel prestep (the part the hardware does NOT do):
  TYS is an integer scanline; TXS and TXEND01 must be pre-stepped from
  their vertex X by their edge slopes times the fractional Y distance
  from the bottom vertex to that scanline, and the attribute bases
  likewise along edge 02.
- Z start (TZS) is treated as S16.15 clamped non-negative; the compare
  uses the integer part (high 16 bits after a 1-bit left shift).
- **Color scale: the integer part of the S8.7 color format is the 8-bit
  channel value (0–255)** — the pixel pipeline computes
  `channel = value >> 7` and clamps to 0–255
  (`dest_pixel_gouraud_shaded_triangle`). Full intensity is programmed
  as 255·128 = 32640, *not* 128; there is no internal normalization.
  The modulate texture-blend path multiplies the 0–255 light value with
  the texel and shifts right by 8.
- Command dispatch confirms the datasheet: 3D Gouraud = 0, lit texture
  = 1/5, unlit = 2/6 (perspective variants select perspective-correct
  sampling); texture blend field = {reflection, modulate, decal}. 2D:
  BitBLT = 0, RectFill = 2, **Line = 3**, Polygon = 5, NOP = 15 —
  independently confirming the line-command finding above.

Fidelity caveats: it is an emulator — bilinear filtering is an optional
emulator feature there, and DX/GX-specific texture-format differences
are modeled per chip. Treat it as strong evidence to be confirmed by
one hardware test, not as ground truth equal to silicon.

## MGA-1064sg_199702.pdf

Matrox MGA-1064SG (Mystique) specification, February 1997. 332 pages,
text layer present. Covers the drawing engine (DWGCTL, pages ~56-59 and
the register reference), the color/texture interpolation data registers
DR0-DR15 (PDF pp. 72-78 and 261-269), and CRTC extensions CRTCEXT0-6
(from PDF p. 134) used for display start address / native modesetting.
