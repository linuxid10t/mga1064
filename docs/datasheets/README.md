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
- **CR36 bits 7-5**: 000 = 4MB, 100 = 2MB, all other values reserved
  (PDF p. 197). Bits are strap-sampled; writable only after CR39 unlock.
- **MM8504 read**: bit 13 = 1 means S3d engine *idle*; bits 12-8 = S3d
  FIFO slots free, FIFO is 16 deep (PDF p. 300).
- **Display start address**: CRC/CRD (bits 15-0) + CR31 bits 5-4 (16-17)
  + CR51 bits 1-0 (18-19); a non-zero CR69 bits 3-0 supersedes the
  CR31/CR51 extension bits entirely (PDF p. 193).
- The exact 3D triangle sub-pixel setup formulas are **not** in this
  document — §15.4.5.2 states setup code "will be provided by S3 to
  customers". Register descriptions define TXEND01/12 as the X of the
  *last pixel drawn* on the side (S11.20, sign must be 0), PDF
  pp. 270-271. For derivation, consult the XFree86 `s3virge` driver
  sources and validate on hardware.

## MGA-1064sg_199702.pdf

Matrox MGA-1064SG (Mystique) specification, February 1997. 332 pages,
text layer present. Covers the drawing engine (DWGCTL, pages ~56-59 and
the register reference), the color/texture interpolation data registers
DR0-DR15 (PDF pp. 72-78 and 261-269), and CRTC extensions CRTCEXT0-6
(from PDF p. 134) used for display start address / native modesetting.
