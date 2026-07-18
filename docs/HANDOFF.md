# L10GL handoff brief — state as of 2026-07-18

Audience: an implementing agent picking up this project cold. Read
`PLAN.md` (roadmap + current-state snapshot) and
`docs/datasheets/README.md` (datasheet page index + verified register
facts) before changing code. Live state: symptom 1 (diagnosed — monitor
scaling moiré, parked); symptom 2 (3D Z-buffer cutoff — RESOLVED);
double-buffering with vsync page-flip (LANDED 2026-07-07); back-face
"bleedthrough" — RESOLVED on silicon 2026-07-08 (perspective cull +
attribute-base prestep; cubefb 0 blends/misplaced AND monitor-confirmed
by David — no bleed visible). NEW symptom 2026-07-08:
black wedges out of face-corner vertices (coverage gaps — old cubefb was
blind to background-inside-face). Cause: integer-dy edge slopes vs the
fractional-Y prestep. Float-dy fix (195260c) closed the wedges but left
9/36 orientations with a coverage NOTCH at shared face diagonals — root
caused + fixed (lr=0 X-attribute-delta sign error, df35256) and VERIFIED
on silicon 2026-07-08: cubefb 36-sweep = 0 holes (see follow-up #4).
CURRENT OPEN AXIS: textured_cube — FULLY RESOLVED & VERIFIED ON SILICON
2026-07-09 (David: "the cube looks perfect"). Perspective-correct hardware
texturing works end to end: the cube renders its 6 textures with NO swim.
Three bugs were found+fixed across v15–v17: (1) driver wasn't scaling normalized
[0,1] UV by texture side (contract bug, 8813cc8); (2) the perspective command
(0101) saturates because the engine divide is texel = 128·TUS/TWS (a 2^7 factor
vs U/W) — fixed by persp ufrac 12 (TEST 17 confirmed EXACT); (3) for the divide
to recover U, supply TUS=U·W / TVS=V·W (perspective-correct, d982e42). The
datasheet's persp S10.21 is wrong for real DX (ufrac 12), just as its non-persp
S12.8.11 is (silicon wants 21). **textured_cube AXIS CLOSED.** LINEAR (bilinear)
verified silicon 2026-07-09 (texprobe TEST 18: 1-texel R-stripe at a texel
boundary blends to R15 in both U and V); cube switched NEAREST→LINEAR.
Ctrl-C console restore: ROOT-CAUSED; FIX DEFERRED (parked 2026-07-09 per David).
Symptom after the init-order fix (6221fa4): the 4MB save+restore runs (log
confirms 4194304 bytes) yet the cube still survives Ctrl-C as a 4-quadrant
color-shifted tile. Probes nailed it:
  - Console display-start is 0x0 (the console lives at VRAM offset 0).
  - saved[0x0] and VRAM[0x0] both read all-zeros via ctx->fb, even though the
    CRTC scans real console content there. So CPU reads through ctx->fb return
    0 -- the BAR0 aperture (resource0 opened O_SYNC, mmap'd MAP_SHARED) is
    write-combined. The snapshot therefore captured ZEROS, not the console, and
    the CPU memcpy restore writes zeros back -> useless. (The post-restore
    display-start reads 0xEA600 = the back buffer the last page-flip left; the
    saved console start is 0x0, restored correctly by the CRTC reg write.)
  - DECISIVE: a 2D-engine rect-fill at offset 0 DISPLACED the cube (top half
    changed color) -> the engine accesses VRAM coherently with the CRTC, the
    CPU aperture does not. (The fill was 16bpp red 0x7C00 but showed GREEN --
    the console is 32-bit BGRX, so two 16bpp pixels pack per 32bpp word and
    0x7C lands in the G byte. Same mechanism as the cube's color shift.)
DEFERRED FIX: capture+restore the console via the 2D ENGINE (BitBLT the 32-bit
console at offset 0 to a VRAM backup at takeover, BitBLT back at cleanup), NOT
CPU memcpy. Needs ~2MB VRAM backup (may require dropping double-buffer or using
the image-data-transfer port to stream through system RAM). The CRTC mode
restore itself WORKS (monitor returns to console timings); only the framebuffer
content is left as the last frame for now. Code: the CPU memcpy + saved_console_vram
skeleton is kept in virge_scanout_takeover/virge_cleanup with an explanatory
note; the diagnostic probes (VRAM dump, engine-fill marker) were removed.
See #5 for the full decode.

**Phase 3 P1 implemented 2026-07-18; native ViRGE regression verified.** The
frontend context now exposes actual byte stride plus fbdev-style packed channel
fields,
and rejects backends that return an impossible mode. `src/fbdev.c` centralizes
GET/PUT/re-read negotiation. ViRGE asks fbdev for the requested geometry and
its mandatory 15-bit RGB555 layout; MGA-1064 programs real padded pitch;
swrast uses the actual packed layout. A refused/ignored mode reports both the
request and live mode plus an example `fbset` command. `make check`, including
the new `test-mode`, passes. The primary `david-ta970` path has no `/dev/fb0`,
so its immediate hardware check was a regression of native takeover. David
reported it works with no regressions after P1. True fbdev mode-switch
acceptance remains to be run on a boot with `s3fb` or `matroxfb`.

**Phase 3 P2 complete and hardware-verified 2026-07-18.**
`src/console.c` snapshots the original fbdev mode before P1 backend init, puts
the active VT into `KD_GRAPHICS` only when it owns the target framebuffer, and
restores the mode before returning that VT to its exact prior KD state.
Backend-init failures unwind ownership; offscreen swrast and no-fbdev native
takeover remain no-ops. `test-console` covers lifecycle and failure ordering.
David's decisive run used `L10GL_BACKEND=swrast`,
`L10GL_SWRAST_FB=/dev/fb0`, `L10GL_STATIC=1`, and 800x600x32. It logged
`VT1 is in KD_GRAPHICS`, displayed the static frame correctly, and Ctrl-C
logged `restored fbdev mode and console ownership`. The companion launcher
run unbound `simple-framebuffer`, rendered the ViRGE cube for 176 frames with
P2 correctly inactive, restored native scanout, then rebound the framebuffer
driver and fbcon. No regressions were observed.

**Phase 3 P5 implemented 2026-07-18; simple-framebuffer fallback verified,
true swrast fbdev page-flip and MGA hardware sign-off pending.** swrast
offscreen mode rotates two owned color buffers, and PPM
dumps explicitly read the just-completed buffer. `test-swrast` verifies that
presentation does not occur before swap and that successive red/green frames
remain distinct.

The first fbdev implementation rendered privately and copied completed rows
into the visible mapping. David's hardware run showed the same horizontal black
bands as the earlier broken live path: scanout can observe a CPU copy in
progress, so this is not true double buffering. The corrected implementation
requests `yres_virtual = 2 * yres` and uses `FBIOPAN_DISPLAY` with
`FB_ACTIVATE_VBL` only when a second non-overlapping page lies inside both
`yres_virtual` and `smem_len`. Fixed drivers such as the target's
`simple-framebuffer` fall back to direct single-buffer rendering. David
confirmed that corrected fallback produces a clean completed static frame.
Continuous animation retains horizontal bands because the one-page scanout
observes swrast between its clear and completed draw; this is the expected
single-buffer limitation, not the earlier CPU-copy regression or a stride
error. `test-mode` covers page selection, a nonzero current page, insufficient
mapped memory, and a single-height virtual raster.

The MGA-1064 driver now reads the live 20-bit CRTC start, plans non-overlapping
front/back/Z regions using `line_length * height`, and enables flipping only
when `smem_len` can hold all three. A swap waits for engine idle and a bounded
250ms vsync, writes CRTCC and CRTCD, writes CRTCEXT0 last as required by
datasheet section 5.6.5, and exchanges YDSTORG with the old front. The original
console raster is copied aside and restored together with the exact three
start-address registers at cleanup. Insufficient VRAM and the no-fbdev fallback
remain safely single-buffered. `test-mga1064` checks 4MB 800x600x16 placement,
nonzero live-front exclusion, 32bpp capacity failure, and exact 8-byte-unit
register encoding. The full normal suite and all seven automated binaries under
ASan/UBSan pass (LeakSanitizer disabled because the command supervisor uses
ptrace). Real Mystique/`matroxfb` validation is still required.

**Phase 3 P6a implemented 2026-07-18; no hardware writes yet.**
`src/backends/virge/virge_mode.c` is a pure module containing the fixed VESA
640x480, 800x600, and 1024x768 timing table at 60/75 Hz plus the DB019-B
SR12/SR13 DCLK solver. It rejects malformed or unrepresentable timings,
enforces the documented M/N/R and 135-270 MHz VCO limits, and requires the
generated clock to be within the databook's 0.5 percent tolerance. The
hardware-independent `test-virge-mode` covers all six entries and exact 60 Hz
PLL bytes. At that checkpoint the module was deliberately not called by
`virge_init`, so it added no hardware writes.

**Phase 3 P6b implemented 2026-07-18; still no hardware writes.**
`virge_mode_encode_16bpp` converts a fixed mode into masked values for all 25
standard CRTC registers and 17 extended CRTC registers, plus SR01/SR08/SR12/
SR13/SR15, Misc Output, and the DAC mask. A nonzero mask means the writer
must snapshot and restore that register. The encoded 800x600 state matches the
working takeover's decisive bytes exactly, and tests decode all six images
back to their source width, height, and pitch. Extended state explicitly
disables interlace and clock inversion, enables sync/VCLK outputs, selects
RGB555 Mode 9 with Streams off, clears every display-start extension, and
sizes the linear window from detected VRAM. The CR3B policy holds the verified
800x600 BIOS refill time at 3 us rather than applying the databook's inadequate
"typically CR00-5" suggestion. That checkpoint was also hardware-inert.

**Phase 3 P6c first 800x600 hardware gate implemented and silicon-verified
2026-07-18.** `L10GL_MODESET=native` selects a native save/apply/restore
transaction. Its first gate was restricted to 800x600@60 intentionally:
the target already displays that raster correctly, so the first run isolates
the new programmable DCLK load (40 MHz target, computed 40.025 MHz,
SR12=49/SR13=79) and full-register restoration. The path requires `/dev/fb0`
to be absent and directs the user to `tools/l10gl-run` otherwise. It blanks via
SR01, loads SR12/SR13 and toggles SR15 bit 5 per DB019-B, installs the tested
masked CRTC image, verifies every writable CRTC plus SR12/SR13 and Misc Output,
then unblanks. Cleanup blanks again and restores all saved CRTC, sequencer,
Misc Output, and DAC bytes in lock-safe order. No console-pixel fix is claimed;
the known incoherent CPU aperture remains deferred, and launcher rebind is
still responsible for redrawing fbcon. Default no-environment behavior is
unchanged. Required test over SSH:

The first run remained synchronized but displaced the live image vertically:
about 100-115 black lines at the top, followed by a shifted/white-bottom
simplefb image after restore. Unlike the proven takeover, P6c had needlessly
rewritten every vertical VGA register during an experiment intended to isolate
the DCLK. The re-test gate preserves all live vertical/pan/addressing-control
bytes and applies only the takeover-proven horizontal/depth/pitch/start set
plus the documented PLL sequence. Restore now repeats CR0C/CR0D/CR69 after the
old PLL is loaded, waits for that start address to latch at retrace, and only
then unblanks. Expanded pre/apply/restore logs include the vertical blank and
display-start bytes needed to distinguish register state from expected VRAM
content damage (native rendering overwrites the old simplefb pixels).

**Corrective silicon result:** the live cube now fills the raster correctly;
the black top band is gone. The initially unchanged exit displacement was a
misleading baseline: an earlier failed modeset had already left the console in
that state, so exact save/restore and fbcon repaint faithfully returned to the
same bad start. After reboot restored the firmware/simplefb baseline, the same
corrected P6 run exited to a correct framebuffer console. `tools/l10gl-run`
still follows fbcon reattachment with a temporary switch away from and back to
the active VT so the kernel-retained text overwrites L10GL's stale color/Z
pixels. The 800x600 PLL and restore gate is closed; enable 640x480@60 next as
the first real resolution-change test.

**P6d 640x480@60 gate implemented and silicon-verified 2026-07-18.** The
backend now allows that mode under `L10GL_MODESET=native` and applies the complete
databook-derived CRTC image rather than the 800x600 clock gate's restricted
live-raster subset. Unit tests pin all 25 standard bytes plus CR5D/CR5E, pitch,
sync polarity, FIFO fetch, and the exact built-in 25.175MHz VGA DCLK selected
by Misc Output bits 3-2=`00`. DB019-B CR18 and its CR07/CR09 overflow fields
only require line compare
to be beyond the visible raster; using standard `0x3ff` disables split-screen
for every supported mode without unnecessarily setting ViRGE CR5E bit 6.
The first hardware attempt used programmable source `11` with a synthesized
25.255MHz DCLK and produced the monitor's out-of-range warning even though all
CRTC bytes read back. DB019-B section 9.2 explicitly provides standard
25.175MHz through fixed source `00` and requires SR15.1 during fixed-clock
selection. The corrected gate now uses that path, snapshots SR15.1, and still
restores the old SR12/SR13/SR15 state because selecting a fixed clock may
replace the PLL parameter bytes implicitly.

That corrected run also remained out of range. Readback was `Misc=e3` (clock
select bits `00`, with saved bit 5 preserved) and `SR15=03`, proving the fixed
clock path was selected. The pending diagnostic therefore measures the
generated signal rather than revising another inferred register: DB019-B
Input Status 1 bit 0 is sampled across 128 horizontal retraces and bit 3 across
three vertical retraces before and after the mode transaction. The same line
logs SR01 and CR43, the documented dot-clock divide and horizontal-counter
double controls. Use those H/V measurements to choose the next correction.

Measured hardware results were 37.854kHz / 60.279Hz before the transaction and
31.321kHz / 59.664Hz in native 640x480. Those are valid internal sync rates,
but the monitor still reported out of range until cleanup restored the old
mode. The pending gate now owns the documented external path instead: CR33.3
is set to force VCLK from internal DCLK; SR0D feature-connector direction and
DPMS sync overrides are set to normal output; and Feature Control bit 3 is
cleared to prevent VSYNC from being ORed with active display. Each added byte
is saved, read back, logged, and restored. `virge_mode_limit_first_gate`
removes these additions from the already-verified 800x600 clock-only path.

That external-path gate also remained out of sync. The inherited external
state was already normal, and forcing CR33.3 changed nothing. The decisive
behavioral reference is Linux `drivers/video/fbdev/s3fb.c`: it clears CR5D
bits 3/5 as pulse-length extensions and excludes them from the horizontal
blank/sync end-position mappings. DB019-B Table 14-1 and CR5D agree that these
bits add 64 DCLKs of blank and 32 DCLKs of HSYNC; they are not ordinary high
end-position bits. P6d's `CR5D=28` therefore made both 640x480 pulses far too
long while leaving their measured periods correct. The corrected image uses
`CR5D=00` and restores s3fb's normal `CR33.3=0`. The P6c 800x600 clock-only
limiter explicitly retains its silicon-verified `CR5D=21` byte.

That corrected image passed on the target ViRGE/DX: the monitor accepted the
mode, the rotating cube was visible, native sync measured approximately
31.33kHz / 59.67Hz, and cleanup restored the 800x600 simplefb signal. P6d is
closed; retain the diagnostic rate/routing logs for the later 75Hz and
1024x768 gates.

**P6e 640x480@75 gate implemented and hardware-verified 2026-07-18.** Set
`L10GL_REFRESH=75` alongside `L10GL_MODESET=native` to select
it; the default remains the signed-off 60Hz image. This is intentionally the
smallest next gate: resolution, stride, RGB555 format, and buffer allocation
are unchanged from P6d, while the fixed table supplies 75Hz timings and a
31.5MHz programmable DCLK. DB019-B sections 9.1-9.2 give `SR12=63`, `SR13=56`
(31.500MHz, 13ppm) and the SR15.5 immediate-load pulse already exercised at
40MHz by P6c. Tests pin the complete standard CRTC image, FIFO fetch,
`CR5D=00`, negative sync polarity, and PLL bytes. 800x600@75 and both
1024x768 modes remain rejected.

The first run accepted the signal and cleanup restored the console, but the
top half of the screen was black and only the lower half of the cube appeared.
This is the data-book CR16 wrap failure, not page-flip timing: DB019-B CR15/16
(PDF p.159) defines End Vertical Blank as blank width plus `(CR15 - 1)`, low
eight bits. The encoder used `vtotal - 1`, producing `f3` even though the
500-line raster's final counter value is `0x1f2`; blanking therefore survived
the frame wrap and cleared only when low byte `f3` recurred near line 243.
The corrected 75Hz image uses `CR16=f2`; its hardware retest displayed the
complete cube with no black upper half. The same general correction changes
640x480@60 from `0c` to `0b`; its regression retest also displayed correctly.
Both 640x480 refreshes are signed off. Open 800x600@75 next and keep both
1024x768 modes locked.

Run over SSH from a clean console baseline:

```
sudo env L10GL_BACKEND=virge L10GL_MODESET=native \
  tools/l10gl-run -- ./cube 640 480 16
```

Verified: visible 640x480 cube, no out-of-range event, and recovery of the
original 800x600 simplefb signal after Ctrl-C. P6e opens only 640x480@75;
800x600@75 and 1024x768 remain locked until their own staged gates.

Hardware-verified P6e command:

```
sudo env L10GL_BACKEND=virge L10GL_MODESET=native L10GL_REFRESH=75 \
  tools/l10gl-run -- ./cube 640 480 16
```

Verified: the complete cube is visible with no black upper half; the first
run also proved recovery of the original console. Target readback is
`CR15=df CR16=f2`. The 60Hz command above also passed its corrected
`CR16=0b` regression.

```
sudo env L10GL_BACKEND=virge L10GL_MODESET=native \
  tools/l10gl-run -- ./cube 800 600 16
```

Expected: the normal cube at 800x600 with no monitor resync/out-of-range event,
the P6 PLL/readback lines in the log, and a working console after Ctrl-C and
launcher rebind. This gate and P6d are confirmed.

## Test setup (fixed, do not re-derive)

- Machine `david-ta970`: S3 ViRGE/DX (PCI id 0x8a01), 4MB VRAM,
  x86-64 Linux, tested over SSH by the human (David). He runs the
  binaries and reports logs + photos of the monitor. You cannot see
  the screen; design every change so one run produces decisive output.
- The normal console exposes `/dev/fb0` through `simple-framebuffer` at
  800×600, 32-bit pixels, stride 3200. `tools/l10gl-run` detaches fbcon and
  unbinds that driver before launching L10GL, so the ViRGE child deliberately
  sees no `/dev/fb0` and takes over the bootloader's leftover VBE scanout
  directly (CR67 Mode 13 before takeover).
- Because of that, `virge_init` performs a **native scanout takeover**
  (`virge_scanout_takeover` in `src/backends/virge/virge.c`): switches
  scanout to Mode 9 (15-bit RGB555), doubles all horizontal CRTC
  timings (15/16bpp modes count two character clocks per 8 pixels on
  this chip family), sets CR50 bits 5-4 = 01b, programs pitch in
  quadwords (LSW = pitch/8, CR43 bit 2 clear), adopts 800×600/stride
  1600 into the ctx, and restores everything at cleanup. **This is
  hardware-verified working** as of commit `301d448`: `scantest`'s
  CPU-drawn full-screen pattern displays correctly (border, corner
  markers, color bands, 555 discriminator green).

For machines where a kernel framebuffer *is* bound, use
`sudo tools/l10gl-run -- ./cube`. The launcher detaches fbcon, unbinds the
owners of `/dev/fb0` and the selected PCI function, then restores the exact
drivers and fbcon after the child exits. `--dry-run` prints the planned sysfs
writes. The primary `david-ta970` setup needs no unbind because it has no
`/dev/fb0` or bound ViRGE driver. **Hardware-verified 2026-07-17 by David:**
the complete detach → L10GL run → driver/fbcon restoration flow works cleanly.

The `swrast` backend is complete as of 2026-07-17 and is the final,
always-available autodetection fallback after ViRGE and MGA-1064. It renders
offscreen by default; use `L10GL_SWRAST_DUMP='frame%04d.ppm'` for PPM capture,
`L10GL_FRAMES=N` for bounded cube/textured-cube runs, or opt into an existing
fbdev mode with `L10GL_SWRAST_FB=/dev/fb0`. `make check` now verifies top-left
coverage, blending, depth, perspective texturing, bilinear filtering, RGB565,
and PPM output at the pixel level. No vintage GPU is needed for this gate.

Phase 2 X1 is complete as of 2026-07-17. `src/l10gl_xform.c` provides
OpenGL-compatible column-major MODELVIEW/PROJECTION stacks (32/4), load and
post-multiply operations, translate/rotate/scale, frustum/perspective/ortho,
viewport and depth range, object-to-clip transformation, and GL-lower-left to
backend-top-left window conversion. `test-xform` covers the math and stack
bounds. It is deliberately above the existing screen-space draw API, so no
hardware backend or proven demo path changed.

Phase 2 X2 is complete as of 2026-07-17. `src/l10gl_pipeline.c` captures
current color/normal/UV state and streams TRIANGLES, TRIANGLE_STRIP,
TRIANGLE_FAN, LINES, and LINE_STRIP through X1 into the existing screen-space
backend calls. Strip winding alternates correctly; fan origin is fixed;
texture binding selects textured dispatch; CCW culling happens in NDC before
the top-left framebuffer Y conversion. `test-pipeline` uses a capture backend
to verify exact calls and vertices. X6 now exercises this path in both primary
cube demos.

Phase 2 X3 is complete as of 2026-07-17. Immediate-mode triangles are clipped
in homogeneous space against `Z + W >= 0` before perspective division and
culling. A crossing triangle emits one or two triangles with interpolated
clip coordinates, color/alpha, normal, and UV. Near-boundary snapping prevents
floating-point slivers, and projected triangles taller than 2047 scanlines are
rejected before they reach the ViRGE's 11-bit count fields. `test-pipeline`
covers one/two/all-outside cases, analytic intersections and interpolation,
the exact near boundary, shared fan vertices, conservative far/line rejection,
and both sides of the scan-height limit. X/Y still use the hardware clip
rectangle; far-plane and line clipping remain future work.

Phase 2 X4 is complete as of 2026-07-17. The immediate-mode frontend now has
opt-in per-vertex material lighting with one eye-space directional light plus
ambient. The API is `l10gl_enable_lighting`, `l10gl_light_dir`,
`l10gl_light_color`, `l10gl_light_ambient`, and `l10gl_material`. Normals use
the normalized inverse-transpose MODELVIEW matrix, including non-uniform scale
and reflections; singular normal matrices fall back to ambient only. Lit RGB
and material alpha are clamped and captured before X3 clipping. `test-pipeline`
covers full diffuse, ambient-only, invalid directions, normalization, clamping,
per-vertex material changes, non-uniform/reflected/singular transforms, and
disabled-lighting compatibility. Existing demos and all backend code remain
unchanged at the X4 landing; X6 now consumes this lighting path in `cube`.

Phase 2 X5 is complete as of 2026-07-18. The immediate-mode projection path
now emits reciprocal homogeneous clip W. Under L10GL's standard perspective
matrices this is reciprocal positive eye-space depth; orthographic W remains
1. X3-generated near-plane vertices interpolate clip W before X5 takes its
reciprocal, avoiding the incorrect interpolation of already-reciprocal values.
`test-pipeline` verifies exact W at eye depths 2/4/5, MODELVIEW-translated
depth, constant orthographic W, textured dispatch, and near-plane
intersections. The raw screen-space API and all backend code remain unchanged;
X6 now uses this generated W in `textured_cube`.

Phase 2 X6 is complete and ViRGE-verified as of 2026-07-18.
`cube` (`0886adf`) now uses matrix state, model-space immediate triangles,
pipeline culling, and X4 material lighting. `textured_cube` (`15e337d`) uses
the same matrix/immediate path and receives all perspective texture W from X5.
The legacy +Z camera maps to OpenGL eye space with a Z reflection; reversed
submission winding preserves CCW front faces, and the reflected light vector
preserves the old diffuse intensities. Both first-frame 640x480 RGB565 swrast
dumps are byte-identical to their pre-port baselines, and eight-frame ASan/
UBSan runs pass. `rawtri` (`8cb2e13`) preserves the direct screen-space
bring-up path, with `triangle` retained as a compatibility executable.
David ran the hardware acceptance set and reported that it looks good: the
pipeline `cube`, `textured_cube`, and raw `rawtri` paths all render correctly
on the real ViRGE/DX. Phase 2 is closed.

Hardware acceptance:

```sh
git pull
make clean && make -j4
make check
sudo tools/l10gl-run -- ./cube
sudo tools/l10gl-run -- ./textured_cube
```

Expect the same geometry, face lighting, bilinear textures, perspective
stability, culling, and tear-free page flips as before the ports.

## Push workflow (non-negotiable — David has corrected agents on this repeatedly)

The workspace machine has **no ViRGE card**; David tests on a separate box
(`david-ta970`) by **pulling from GitHub**. Consequence: he cannot run
anything until it is pushed.

For every change:
- Build locally first (`gcc` needs no card): `make -B <target>` to confirm it
  compiles before he ever pulls. Plain `make` builds the static library, all
  backends, every frontend demo, and every retained diagnostic.
- `git commit` with the **expected hardware observation** in the message
  (what David should see on `david-ta970`), then `git push origin main`
  **immediately**.
- **Never hold a commit "until hardware-verified" — that deadlocks him.**
  Push first; he pulls, runs on `david-ta970`, and reports logs/photos back.

The old `BACKEND=` build switch no longer exists: frontend demos autodetect at
runtime and `L10GL_BACKEND=virge|mga1064` forces one. ViRGE-only diagnostics
link the ViRGE objects from `libl10gl.a` directly. `make -B` rebuilds all of
them, preventing the stale-diagnostic problem described later in this handoff.

## Ground rules (from PLAN.md, non-negotiable)

- Register facts need citations from `docs/datasheets/` (DB019-B, by
  PDF page). Where the databook is silent (per-depth timing scaling,
  CR50), the kernel `s3fb` driver and 86Box are the documented
  behavioral references — consult, never copy code (86Box is GPL).
- One logical change per commit; never mix refactor with behavior.
  Every commit builds clean: `make -B`.
- Never write an unbounded register-poll loop (see the
  `virge_wait_vsync` latched-VSY-INT lesson in virge.c).
- State in each commit message exactly what the human should observe
  on hardware.

## Symptom 1: DIAGNOSED — monitor scaling moiré (not a driver bug; parked)

The dark vertical band ~1/3 across in CPU-drawn `scantest` content is
**monitor-side analog scaling moiré**, confirmed 2026-07-06, not a
register defect:

- It is a **dimming** — the correct content shown darker as thin
  horizontal slivers inside a ~30px-wide vertical stripe, full height.
  A brightness modulation, not corrupted/missing data, so the
  framebuffer writes are correct and the artifact is purely in display.
- "Vertical band of horizontal slivers" is the signature of analog-VGA
  sampling interference.
- The test monitor is a **1440x900 LCD**; the driver outputs 800x600,
  so the monitor scales 800->1440 horizontally (1.8x, non-integer).
  Non-integer scaling beats the source pixel rate against the panel's
  sample rate, worst at one X -> the band. A CRT would not show it.

Driver causes exonerated on hardware: monitor auto-adjust (no change);
CR3B/SFF at two values -- `new_CR00-5` = 254 and `2x BIOS SFF` = 234
(no change, commit `048bc06`); EHB/EHS re-encode verified bit-exact
against DB019-B sec.16 PDF pp.148-153. The takeover's pixel-level
timing is identical to Mode 13 (HT/HDE/sync pixel-preserved under the
char-clock doubling: 132 chars x 8 px = 264 chars x 4 px = 1056 px),
so the monitor sees the same 800x600@60 signal it always did -- the
band was simply hidden in Mode 13's garble. The earlier CR3B read-only
diagnostic is commit `2937667`. The CR3B change to `2x BIOS SFF` is
KEPT: it restores the BIOS-intended 120-px FIFO refill window, which is
more correct than `CR00-5` even though neither affects this band.

**Not driver-fixable** without driving the panel's native 1440x900 -- a
non-standard 16:10 mode needing P6 native modesetting and a PLL the
ViRGE may not cleanly hit. **Parked.** Free mitigation: the monitor's
manual coarse/fine clock & phase controls can reduce it. The scanout
itself is correct; this is cosmetic and does not block engine work.

## Symptom 2 (3D Z-buffer cutoff): RESOLVED 2026-07-06/07

The 3D cutoff (cube/triangle cut at ~2/5 height, y≈234 of 600; 2D fills
full height) was **two compounding bugs**, both fixed and verified:

1. **VIRGE_Z_FIXED scale** (commit `ae351e6`, the V10 color-scale twin).
   The Z unit compares the S16.15 *integer part* (TZS>>15) as the full
   0–65535 Z word. The old `z<<15` made every z<1.0 compare as **0**, so
   ZBC_LESS became "pass iff the fetched Z word is nonzero" (the cutoff)
   and ZUP wrote 0 (no depth ordering). Fixed: `VIRGE_Z_FIXED(x) =
   x*65535*32768`, same ×65535 on the deltas (virge.h).

2. **Z_STRIDE clobbered by 2D commands on real DX silicon** (commit
   `f0811f1`, renamed `engine_init_3d` → `program_3d_state`). On real DX
   hardware, executing 2D commands (`virge_clear_z`, `virge_fill_rect`)
   resets `VIRGE_3D_Z_STRIDE` (0xB4E8) to its all-ones default **0xFF8
   (4088)**. The clear wrote Z at stride 1600 (2D) but the 3D fetch then
   used 4088 — they disagreed, and below y≈234 the 3D unit fetched garbage
   Z and failed the test. 86Box never reproduced this (it models the 2D
   and 3D register files as separate — the "fidelity caveat"). Fix:
   re-arm `program_3d_state` (DEST/Z base, strides, clip) before **every**
   3D primitive, in both `virge_draw_triangle_gouraud` and
   `virge_draw_textured_triangle`.

**Verified:** `gltritest` ZBC matrix — LEQUAL/LESS/ALWAYS all FULL at
maxy=549, count=89899; NEVER empty. Z_BASE toggle (0xea600 vs 0x200000)
both FULL. `./cube` renders full height (cutoff gone). This DX-specific
"2D-invalidates-3D-Z_STRIDE" behavior is hardware-established knowledge
no on-hand document contains — recorded in `docs/datasheets/README.md`'s
verified-facts list.

The Z-layout probe in gltritest still prints `[MISMATCH]` (measured
stride ~1597 vs 1600) — a false-positive measurement artifact (stray
0xB332 hits in uninitialized VRAM corrupt the first/last endpoint math),
NOT a regression; the matrix is authoritative.

## Double-buffering + vsync page-flip: LANDED 2026-07-07

Tear-free animation via CRTC page flip (commits `2863663`..`7b4c67b`).
The cube/textured_cube were single-buffered and unsynced (torn); now they
render into a back buffer and `l10gl_swap_buffers` flips it to scanout at
vertical blank. Two hardware facts were settled first by `fliptest`
(`demos/fliptest.c`), both cited in `docs/datasheets/README.md`:

- **Display-start-address unit = DWORD (byte/4).** CR31 bit 3 (ENH MAP,
  which the driver sets) selects doubleword addressing (DB019-B PDF
  p.193). fliptest cycled CR0C/CR0D+CR69 through x1/x2/x4/x8 of the
  back-buffer offset; only **x4** gave a clean full-screen page flip
  (x8 sheared "red-over-green", x2 hit the Z-region garbage, x1 hit
  end-of-VRAM garbage).
- **The per-boot vsync timeout was a missing interrupt enable.**
  `virge_wait_vsync` polled the VSY INT latch (Subsystem Status bit 0),
  but that latch only reports an interrupt that is ENABLED, and the
  driver never set VSY ENB (Subsystem Control bit 8, PDF p.300). fliptest
  confirmed: VSY INT sets in ~60ms with VSY ENB, never without. 0x3DA
  bit-3 (live retrace) also works as an independent source. Fix: OR
  VSY_ENB onto the clear write (`b904aa4`); the "wait_vsync timed out"
  warning is gone.

Backend (`8527a9e`): two color buffers — buffer 0 at offset 0 (the
scanout at init), buffer 1 at stride*height; `ctx->fb_base` is always the
current render target and starts at 0, so single-buffer callers and the
offset-0 readback diagnostics are UNCHANGED. `virge_swap_buffers`:
wait_engine → set_display_start(just-drawn buffer) → wait_vsync (let the
flip land before reusing the old front) → flip fb_base. Z buffer shared
across both color buffers (cleared per frame). Z base moved 0xea600 →
0x1d4c00 (still inside 4MB). Takeover/cleanup now save+restore CR0C/CR0D/
CR69 so the console returns to offset 0 after a swapping run. Frontend
(`6e4f7f8`): `l10gl_swap_buffers(ctx)`, EGL-style, NULL-safe (mga1064
leaves the slot unset). Demos (`7b4c67b`): cube/textured_cube call it
after wait_engine each frame; `triangle.c` is static and stays
single-buffered.

**Verified on hardware (2026-07-07):** cube/textured_cube animate
tear-free. `tritest` rebuilt explicitly (`make -B BACKEND=virge tritest`)
confirms the new layout end-to-end: boot prints `FB base: 0x0 (back buf
0xea600), Z base: 0x1d4c00` with **no vsync timeout** (commit 2 — the
takeover's own wait_vsync no longer prints the 250ms warning), and the
Z-on triangle renders FULL HEIGHT (123339 px, bbox y=[21,579]) — so the
offset-0 render-target invariant holds and Z-buffering is correct at the
double-buffer layout. (gltritest still needs an explicit rebuild to re-run
its matrix, but tritest already covers 3D+Z at the new layout; the earlier
"stale gltritest" paste is moot.) tritest's own verdict line — "FULL
HEIGHT with Z ON … the cube cutoff is NOT Z" — matches the bleedthrough
diagnosis below (a culling/setup issue, not a Z-buffer failure).

## Back-face "bleedthrough": RESOLVED 2026-07-08 — VRAM-clean (cubefb 0 blends/misplaced) AND monitor-confirmed by David

(History: the "monitor scaling, closed" call of 2026-07-07 was retracted the
same day — scaling blends colors into a fringe, it does not bleed pure face
colors through. The early cubefb only checked blends at 5 orientations;
solid misplaced face color was invisible to it.)

The upgraded `cubefb` (36-orientation sweep; blends + MISPLACED + excess
signals) settled the discriminator: the bleed IS a per-frame VRAM artifact
(10/36 orientations contaminated; 9/36 after the snapped-slope + edge-X
prestep commit `25786e7`). It is NOT animation/swap/monitor.

**Diagnosis** (per-elimination): seamtest proves two front-facing faces
never generate fragments for the same pixel (watertight partition), so Z
never arbitrates between them and no front-front pair can contaminate. The
remaining fragment source inside a front face is a face that should have
been culled: the demos' `normal_view[2] >= 0` test is the ORTHOGRAPHIC
approximation, which at camera_dist=5 mis-classifies faces within ~11° of
edge-on. A barely-back-facing sliver projects INSIDE the silhouette,
overlapping its front neighbor with near-tied Z along their shared edge —
exactly the "misplaced solid face color" signal, only near edge-on. Its
visibility was amplified by a second, backend-side gap: the attribute bases
(TZS etc.) were still sampled at the raw bottom vertex after `25786e7`
moved (TXS, TYS) to the prestepped scanline position, biasing each
triangle's Z by up to one edge-walk step — many 16-bit LSBs on a steep
near-edge-on face — which inflates near-tie noise.

**Fix (perspective cull `3d4e49c` + attribute-base prestep `25786e7` +
`cubefb` z_cmd_bits pin to LESS) — VERIFIED on silicon 2026-07-08.** cubefb
36-sweep reports 0 blends / 0 misplaced / 0 holes, AND David confirms on
the live monitor (after the df35256 run) that no bleedthrough is visible
— the display path is clean, closing the gap a static VRAM readback
alone could not. The earlier "monitor scaling, closed" call was wrong
(scaling fringes; it does not bleed pure face color through); the real
fix was the cull + base prestep, and the monitor view is now the
exonerating evidence.
- Perspective-correct cull in cube/cubefb/cubediag:
  `dot(normal_view, center_view) >= 0` skips the face (exact for planar
  faces; unit-cube center == normal, so `center_view = normal_view +
  (0,0,CAMERA_DIST)`).
- Backend: attribute bases (Z, colors, and U/V/W in the textured path) are
  now prestepped along edge 02 to (TXS, TYS) — the missing half of the
  README §prestep. Textured path also gained the snapped-slope edge prestep
  and the |det| < 1 degenerate guard for parity with the Gouraud path.
- `cubefb` now pins `z_cmd_bits` to LESS (the glue state cube.c runs
  under); it previously ran on the virge_init LEQUAL default, a different
  near-tie tie-break than the demo it instruments.

Silicon facts that stand (and constrain any future theory):
- **Z pipeline correct:** ZBC matrix FULL, exact Z-writeback, TdZdX and
  TdZdY both measured/intended = 1.000 (see Symptom 2 / dztest).
- **Coverage watertight:** `seamtest` shows NO shared-edge double-draw (the
  two-triangle overlap test's boundary column did not flip with draw order)
  and the start/end fill rules combine to a clean partition in every
  configuration (left triangle fills to N−1, right from N).
- **Contamination is orientation-gated:** only near-edge-on configurations
  (Top near-parallel + Left near-perpendicular) — the cull-wedge signature.

Investigation trail:
- **Back-face cull (`3d4e49c`, test CORRECTED 2026-07-08):** the original
  `normal_view[2] >= 0` orthographic test was itself the residual-bleed
  culprit; replaced with the perspective dot-product test in all three
  cube demos.
- **LEQUAL + back-to-front sort (`d97577a`, then REVERTED `3d3a579`):** added
  on a wrong "shared-edge Z-tie" model. seamtest proved coverage watertight,
  so draw order / depth-func are irrelevant to edges; reverted to LESS +
  fixed face order + cull.
- **Depth range (`f8fb056`, KEPT):** `sz = (z_eye + camera_dist − 3)/4.0`
  (~56k Z levels) — still correct, reduced grazing-angle noise.
- **Z-gradient scale — exonerated** by `dztest` (TdZdX/TdZdY = 1.000).

Diagnostics retained: `cubediag` (interactive cube + color legend, same
LESS path as cube), `seamtest` (fill-rule / watertight proof), `cubefb`
(36-sweep contamination detector — the verdict tool for this fix).

**2026-07-08 follow-up — sweep CLEAN on silicon, cull fix confirmed; new
symptom: black vertex wedges.** With the cull + base-prestep fix the
36-sweep reports 0 blends / 0 misplaced, but David sees black triangles
growing out of some face-corner vertices. cubefb was BLIND to those: a
background pixel inside a face trips no signal (MISPLACED skips any pixel
with a bg neighbor). Cause found in setup: `compute_dxdy_snap` divided by
the INTEGER scanline count while the prestep multiplies by fractional Y —
the slope is distorted by dy_true/dy_int, which for a short shallow edge
at a corner (1–2 scanlines, large ΔX; dy_true either side of 1) mis-places
the span end by tens of px on the vertex rows: a background notch (or
spill) at the vertex. Fix: `edge_slope` divides by the TRUE float dy
(clamped to the S11.20 range — only reachable when the accumulator is
never stepped before reload), `edge_x_at` computes the prestepped edge X
by bounded interpolation between the endpoints (exact for any slope, no
overflow, bit-identical for both triangles sharing the edge), and the
attribute bases are evaluated at (TXS, TYS) via the plane equation
instead of the yf·edge-walk shortcut (bounded even where the slope
clamped). cubefb gained a verdict-driving HOLES signal: background >1.5px
inside the convex hull of the 8 projected vertices (exact silhouette for
a convex solid). UNVERIFIED on silicon — re-run the sweep AND eyeball the
spinning cube (expected: 0 holes, no wedges).

**2026-07-08 follow-up #2 — notch ROOT-CAUSE-LOCALIZED to Z=LESS, not
coverage (diagap on david-ta970).** The 9/36 coverage holes survive the
float-dy fix, so `diagap` reproduced the cube's exact Left-face shared
diagonal at 310deg in isolation and split it. Decisive result:
  - A-alone (LESS): a_left drifts INLAND of trueX (correct rows 351-364,
    then freezes at ~0.9 px/row, +40px inland by row 377).
  - B-alone (LESS): b_right = floor(trueX) every row. Correct.
  - both-LESS A-first == both-LESS B-first == 40px gap (NOT draw-order).
  - **both-ALWAYS A-first: 0px gap; a_left = ceil(trueX) every row.**
Same tri A, same geometry, only Z mode differs => **Z=LESS is rejecting
tri A's OWN pixels on its diagonal side.** Nothing competes with A in
the alone pass (fresh clear_z 1.0), so A's interpolated Z must be >= 1.0
on those columns. Tri A is lr=0 (edge-02/long on the right, the diagonal
edge-12 on the left); virge.c seeds TZS at (x_start,y_bot) on edge-02
(`virge.c:1107-1130`) and walks it up/across via
`ew_z = -dzdy + slope02*dzdx`. That Z walk diverges past 1.0 as it
reaches the diagonal, WORSE toward the edge-12 seed vertex v1=P7 (row
377) — exactly the observed growth direction. Consistent with all three
edge-stepping refutations (Z=ALWAYS is watertight => the X edges are
correct). The bug is in **Z setup** (seed and/or gradient), asymmetric
across the two coplanar tris because their vertex orderings differ (A's
diagonal = edge-12/top-half; B's = edge-01/bottom-half, fine).
**Confirm probe (commit c738f52, awaiting run):** diagap's Z-readback
pass dumps A's written Z across the scanline — prediction is gap columns
read saturated ~1.0 while red columns show a normal gradient, which
distinguishes seed-offset (uniform) vs gradient-sign/magnitude (tilts
past 1.0 mid-span). Run `sudo ./diagap` (310deg) and a clean-angle
control. Do NOT commit any TXEND12 edge perturbation — X edges are correct.

**2026-07-08 follow-up #3 — notch SOLVED; fix landed (df35256), awaiting
silicon verification.** diagap's full-span Z readback (commit 6e3afe3)
on david-ta970 pinned it: the X-direction attribute deltas were
programmed with the raw plane gradient for BOTH L/R directions, but the
engine seeds each attribute at TXS (edge 02) and iterates toward TXEND
adding +TdAdX per pixel — which runs in -X for lr=0 (R-to-L). So for
lr=0 the per-pixel step must be -dA/dx, not +dA/dx. Tri A {0,4,7} is
lr=0 with a wide span (its diagonal is edge-12); its Z gradient tilted
the WRONG WAY across the span (same magnitude ~0.0068/px, opposite
sign), climbing to 1.000 (saturated) at the diagonal where the true Z
(coplanar tri B, lr=1, correct) is ~0.086. Z=LESS rejected those
pixels => the notch. The seed (edge-02 Z ~0.70) and Y-walk were correct
— only the across-span X-slope sign was wrong. Flat-shaded cube faces
hid it in color (color X-deltas == 0); Z=ALWAYS hid it in Z; only a
wide lr=0 span with a real Z gradient under Z=LESS exposed it.
**Fix:** `sx = (lr_direction == 0) ? -1.0f : 1.0f;` applied to all
X-direction deltas (color R/G/B/A, Z, texture U/V/W) in both the
Gouraud and textured triangle paths; the Y-direction deltas (`ew_*`,
edge-walk along side 02) are lr-independent and stay raw. **Verify on
next run:** `sudo ./diagap` (310deg) → both-LESS passes 40px→watertight
and A's diagonal Z drops 1.000→~0.1 (matching B's ~0.086, no
saturation); `sudo ./cubefb` 36-sweep → 0 holes. Tri B (lr=1) is
untouched and stays correct.

**2026-07-08 follow-up #4 — VERIFIED on silicon (df35256 closes the
coverage-hole axis).** David ran `sudo ./diagap && sudo ./cubefb` on
david-ta970: diagap 310deg is watertight across ALL passes (A-alone LESS,
B-alone LESS, both-LESS A-first, both-LESS B-first, both-ALWAYS = 0px;
was 40px). The Z-readback shows A's diagonal Z now **0.127** at y=377
x442 (was saturated 1.000), climbing correctly to 0.661 at the seed
edge-02 — slope magnitude ~0.0068/px in the correct sign, matching B's
true diagonal Z ~0.086 (no saturation). cubefb 36-sweep: **0 of 36
orientations contaminated** (0 blends / 0 misplaced / 0 holes; was 9/36,
310deg worst at 1196 holes). The 9/36 coverage-hole axis is **CLOSED**.
Tri B (lr=1) untouched, stays correct. Any remaining visible artifact is
NOT in VRAM (cubefb reads VRAM directly and is clean). The back-face
"bleedthrough" axis is ALSO resolved — David confirms no bleed is
visible on the live monitor (see the section below).

**2026-07-08 follow-up #5 — OPEN: textured_cube mapping broken — UV does
not interpolate across a triangle.** With the flat cube + bleedthrough
both verified clean on silicon, David reports textured_cube "still isn't
right" — and it was the one demo NEVER silicon-verified (line 180 only
ever confirmed it animates/swaps). This is the current active axis.

SYMPTOM — `texprobe` v1 RAN on david-ta970 (commit 7fda1ca): rendered a
face-on quad (constant w=1.0, UV 0,0->1,1) with a UV-ENCODING 64x64
ARGB1555 texture (texel(x,y) color = R=x>>1, G=y>>1) and CPU-read the
framebuffer (the sampled color per pixel IS the (u,v) the engine
computed). Result: coverage 100%, but **U and V are DEAD-CONSTANT across
the quad interior** — R pinned at 13 (U~=0.41, texel 26) at every
interior x; G oscillates 1..2 (V~=0.04, texel ~3) at every y. They do
NOT interpolate 0..31. Only a start value survives; the texture
gradients are not taking effect. (So each cube face reads as ~one flat
texel color — almost certainly what David sees.)

RULED OUT (all verified against datasheet DB019-B §19.4):
  - NOT the missing `*(tex_width-1)` in `virge.c:tex_coord_fixed()` —
    that would pin R at 0 (texel 0), not 13.
  - NOT wrong register addresses — TUS=0xB538, TdUdX=0xB520, TdVdX=0xB51C,
    TdUdY=0xB52C, TdVdY=0xB528, TdWdX=0xB50C, TdWdY=0xB510, TWS=0xB514,
    TVS=0xB534, TEX_BASE=0xB4EC all match the datasheet (the 0xB524 "gap"
    is TdDdY, the mipmap D-delta).
  - NOT the `s` field — s=log2(64)=6 (datasheet: "value of 4 => 2^4=16x16").
  - NOT the UV format — `S(4+s).(27-s)` = S10.21 with perspective, exactly
    what `tex_coord_fixed` (val*2^(27-s)) produces.
  - NOT the command — 0101 = lit-texture-with-perspective.
  - NOT texture placement — TEX_BASE = z_base+z_size, valid VRAM after
    both color buffers + Z.
So it is a subtler bug than addressing/format/scale.

REMAINING SUSPECTS: (a) the per-pixel DELTAS TdUdX/TdVdX (+Y) are
computed non-zero (dudx~=0.0025 for the 400px-wide quad) but the engine
is not applying them; OR (b) the START register / perspective W-DIVIDE is
broken. Leading hypothesis = the **perspective W-divide**: U/V are S10.21
but W is **S12.19** (different fixed-point scales) and W is documented as
the "homogeneous coordinate / depth coordinate for 3D texture maps";
`virge.c`'s comment claims the engine "interpolates U/W, V/W, 1/W then
divides" but the driver feeds plain U (not U*W) and W=1/z_eye. The
datasheet W register (§19.4) adds "format S12.19, where S must be 0".
Secondary (revisit AFTER UV is fixed): `textured_cube.c` has NO back-face
cull (the perspective cull `3d4e49c` went into cube/cubefb/cubediag only),
so it draws all 12 tris on Z=LESS alone.

**86BOX REFINEMENT (2026-07-08, pre-v2-run) — sharpens the suspects above
(86Box `vid_s3_virge.c` is the behavioral reference per repo rules):**
- The engine DIVIDES U by W for perspective cmds. `tex_sample_persp_*`
  (line ~3821) computes `w_inv = 2^46 / W; u_final = (U * w_inv) >> (shift)`.
  The virge.c comment at line 1473 ("interpolates U, V, W linearly... when
  perspective enabled") is WRONG — it interpolates them, then divides U by W.
  The persp path is selected by CMD_SET bit 29 (the `|8` term in the switch
  at line ~4513); cmd 0101 sets bit 29, so it IS taken.
- CONSEQUENCE for the W=1.0 texprobe: the divide is a CONSTANT scale (W=1
  → no-op on whether U varies). A working-but-scale-mismatched delta would
  TILE, not pin. Dead-constant UV therefore ⟹ `state->u` is not advancing
  ⟹ the DELTAS are not applied (or only the START survives) — NOT the
  W-divide. So suspect (a) [deltas] is the cause of the *constancy*; the
  W-divide is a SEPARATE, real bug for the cube only (W≠1).
- W-divide feed IS wrong for the real cube (confirmed caller passes
  `sw = 1.0f/z` at textured_cube.c:65): engine computes U/W = U·z_eye; the
  correct feed is `TUS = U·w`, `TWS = w` (and `d(U·w)/dX`, `dw/dX`), giving
  (U·w)/(w) = U perspective-correct. Fix AFTER deltas (mixing muddies v2).
- DX-specific: ViRGE/DX selects `tex_sample_persp_normal_375` (line ~4548),
  whose shift is `(8 + max_d)` not the non-DX `(12 + max_d)`. Watch TEST 1's
  R/EXPECTED-R grid for a scale mismatch (tiling / wrong range) once deltas
  work — that would be a U-feed-scale sub-bug on the _375 path, separate
  from the W-divide. Not asserted yet; verify empirically from the grid.

**`texprobe` v2 (f6c635c) RAN on david-ta970 2026-07-08 — result is BIGGER
than the start-vs-delta fork: the engine is NOT reading our texture at all.**
- TEST 2 (constant-UV quads uv=(0.5,0.5) and (0.9,0.1)) both read the
  IDENTICAL pixel 0x3434... center px=0x3436 R=13 G=1 — the SAME as the
  TEST 1 gradient interior. So programming different UV changes NOTHING:
  the texture-coordinate registers (TUS/TVS + deltas) have ZERO effect.
- The rendered pixel 0x3436 is NOT any texel of our uploaded texture.
  Our texels are all `0x8000|(x>>1)<<10|(y>>1)<<5|1` (alpha=1, B=1);
  0x3436 has alpha=0, B=22. So the engine samples memory that is NOT our
  texture. (UV-irrelevant + non-texture value => the engine reads a fixed
  off-texture location regardless of UV.)
- REG dump (read back after TEST 1) confirms the WRITES land correctly:
  TdUdX=0xffffeb86=-5242 (= sx·dudx·2^21 with the lr=0 sign-flip, dudx
  =0.0025 for the 400px quad — CORRECT), TdVdX=0 (correct, dvdx=0),
  TdVdY=0xffffe4b2=-6990 (correct, ew_v), TWS=0x00080000 (=1.0·2^19,
  CORRECT), TdWdX/TdWdY=0 (W const, correct). TEX_BASE 0xB4EC reads
  0xffffffff (write-only — uninformative). (TdUdY/TUS/TVS readbacks look
  aliased/stale: TdUdY==TdVdY and TUS==TVS, both ≠ their written values —
  readback artifact, not necessarily the engine's working values.)
- So: deltas ARE programmed correctly but have no effect; output isn't
  our texture. Two surviving root-cause hypotheses, both predict "UV no
  effect + non-texture output": (H1) the upload did NOT land at TEX_BASE
  in engine-visible VRAM and the memory there is uniform 0x3436; (H2) the
  engine reads a DIFFERENT address than TEX_BASE (TEX_BASE write ignored/
  overridden, or texel addressing reads off-texture). All driver code
  reads correct on paper: tex_addr = tex_heap_next = z_base+z_size =
  0x1d4c00+0xea600 = 0x27F200 (valid <4MB), upload writes ctx->fb+tex_addr
  (linear BAR0 aperture = engine VRAM), bind writes TEX_BASE=tex_addr&~7,
  format=ARGB1555=(2<<5) (2B/texel, correct), program_3d_state does NOT
  touch TEX_BASE. So the bug is subtle / silicon-specific — must probe.

**ROOT CAUSE FOUND 2026-07-09 (texprobe v6, commit 0be1ce9): the engine
emits TEX_BDR_CLR (texture BORDER color, reg 0xB4F0) for EVERY texel — it
is NOT reading any VRAM texel at all.** Earlier theories (TEX_BASE
clobbered; engine ignores TEX_BASE / reads a fixed base; v4/v5 "tiling")
were all RED HERRINGS. Proof:
- 6a/6b: fill ALL of VRAM uniform (black `0x8000`, then white `0xBFFF`)
  and draw UV=(0,0). If the engine reads ANY texel the quad takes that
  color. White fill → output stayed the border color (NOT white) => no
  VRAM texel sampled. (6a black-fill was a false-positive verdict one run
  — the border register happened to read 0x0000/black that run, colliding
  with the fill; 6b white fill is the trustworthy signal.) NOTE: the
  border register is UNPROGRAMMED, so its incidental value varies per run
  (0x3436 in the v6 run, 0x0000 in the v7 run) — that is why TEST 1-5
  showed 0x3436 earlier and black later. Same behavior, different color.
- 6c: program each 3D color reg to white one at a time — TEX_BDR_CLR
  (0xB4F0) flips the output to white; FOG_CLR/COLOR0/COLOR1 do nothing.
  So the constant = TEX_BDR_CLR, emitted because every texture coordinate
  resolves out-of-range → border (the ViRGE "CLAMP"/`_nowrap` texel reader
  returns the border color, NOT clamp-to-edge; 86Box condition is
  `((u|v) & 0xf8000000) == 0xf8000000`, i.e. a negative/overflowed coord).

**v7 (760be61) RULED OUT the U/V fractional-bit scale.** Swept ufrac in
{2,4,6,8,10,12,14,21} at UV=(0,0) and UV=(0.5,0.5) with a solid-RED
texture + BLUE border: EVERY row = blue border (no texel read). So the U/V
encoding scale is NOT the cause. Crucially, at UV=(0,0) TUS=0 for all
ufrac → u_final=0 (in range) yet it STILL borders. **Real DX borders even
when the computed coordinate is zero** — so the trigger is NOT the U
magnitude. The driver's `tex_coord_fixed` (frac_bits=27-s_val=21, S10.21,
the datasheet format) is correct for the NON-perspective path; the
PERSPECTIVE sampler (`tex_sample_persp_normal_375`, selected by command
0101 / bit29=1) divides U by W and on real DX that path borders
unconditionally. (86Box's _375 math disagrees with silicon here — it
predicts texel 0 at UV=0 — another fidelity gap.)

**v8–v10 RULED OUT the path, the format, and the source stride:**
- v8 (ba69ed7): the NON-perspective command (0001, no W divide) ALSO borders
  — 8a gradient flat, 8b persp baseline borders. So it is NOT the perspective
  divide alone.
- v9: swept ufrac {11,13,15,17,19,21,23} under non-perspective (datasheet
  S12.8.11 affine format, texel int ~bits 30:19) — ALL border. Not the format.
- v10: fixed DEST_SRC_STR (0xB4E4) source-stride field to the texture row
  pitch (was erroneously the dest/screen stride) — STILL all border. Fix is
  correct and KEPT, but was not the cause.

**BREAKTHROUGH v11 (4170fbc, silicon 2026-07-09): the texture fetch IS
ALIVE — it just needs WRAP.** TEST 11 draws a solid-RED texture + BLUE
border under CLAMP vs WRAP (CommandSet bit26 TWE), persp u21 and non-persp
u19, at in-bounds UV (0.5,0.5) and OOB UV (70,70):
- **CLAMP borders EVERY coord** — including the provably in-range (0.5,0.5)
  AND (per v7) UV=(0,0) itself.
- **WRAP reads RED at EVERY coord** — incl. (0.5,0.5) and OOB (70,70), both
  paths.
This EXONERATES TEX_BASE, the upload, ARGB1555, s=6 and NEAREST (all proven
by the WRAP fetch reading the correct texel region). The divergence is purely
in address-range handling. The datasheet (3d_regs.txt:425-430, 786-789) ties
TEX_BDR_CLR to "wrapping disabled (bit26=0) AND the texture rectangle too
small to complete the fill" — i.e. CLAMP is clamp-to-BORDER (not
clamp-to-edge), and real DX fires it for coordinates that are in-range on
paper. (Per the source-trust hierarchy: datasheet > driver > 86Box; 86Box's
`_nowrap` border condition `((u|v)&0xf8000000)==0xf8000000` is NOT what real
DX does — real DX borders u=0, which that condition never would. Treat 86Box
as a weak tie-breaker here.)

**OPEN QUESTION (v11 could not answer): are our coordinates actually
CORRECT, or is WRAP just masking a garbage coordinate back into range?**
A SOLID texture reads RED for any in-range texel, so it can't tell which
texel WRAP fetched.

**v12 RESULT (a7ab1ee, silicon): under WRAP the address is STUCK at texel(0,0).**
12a (non-persp) and 12b (persp) gradient-under-WRAP both read R0 G0 at EVERY
grid point — i.e. every pixel fetches texel(0,0) (the only gradient texel with
R=G=0 = 0x8001). So the engine DOES fetch under WRAP (TEST 11: no border), but
the U/V coordinate does NOT select which texel — the fetch address is pinned at
TEX_BASE+0. (My "coords correct → use WRAP" prediction was WRONG — WRAP masks
the dead addressing, it does not fix it.) Datasheet grounding
(s3d_programming.txt:960-963): "the integer components of U and V generate the
memory addresses"; 816-870 confirm TUS/TVS + TdUdX/TdVdX + TdUdY/TdVdY +
TBU/TBV(=0) are the full set — we program every register it names.

**v13 RESULT (56d354c, silicon): ALL UV read 0x0000 = BORDER, incl. UV=(0,0)
(NOT texel(0,0)=0x8001). Registers ARE correct — TUS=0x01f80000 (=63<<19),
deltas=0 (constant UV), TEX_BASE=0x2bf200, CMD=0x8cc0c644 (non-persp 0001,
WRAP bit26=1, NEAREST, s=6, ARGB1555). START is correctly programmed yet still
borders.** This OVERTURNS v12's "stuck at texel(0,0)": in v12 TEX_BDR_CLR was
0, so 0x0000 (border) was indistinguishable from 0x8001 (texel 0) by R/G alone
— the same trap as TEST 6a. v12/v13 both read BORDER, not texel(0,0). **REAL
PARADOX: TEST 11's SOLID RED FETCHED under WRAP (0x7c00) but the gradient
BORDERS under WRAP** — same WRAP/path/ufrac, different texture. Fetch-vs-border
is TEXTURE-SPECIFIC, not coordinate-specific.

**v14 RESULT (266f070, silicon): TWO findings that REFRAME the whole bug.**
(1) **CLOBBER**: the startup gradient's VRAM was destroyed to 0x8000. texprobe's
own TEST 5/6 fill ALL of VRAM (vram_size/2 texels) — they clobbered the gradient
at 0x2bf200. NOT a driver bug: l10gl_clear is bounded to color+Z (g2 survived
multiple clears). So every gradient test after TEST 6 (8a, 12, 13) read a DEAD
texture — that is why they looked like "border". The "every texel is border"
saga was a dead texture under CLAMP (real DX borders under CLAMP).
(2) **UFRAC**: a FRESH gradient FETCHES under WRAP — the FIRST real texel read
ever (GRAD fresh UV=32,32→R4, UV=63,63→R7; RED→R31). But texel_index = U/4: the
engine reads the texel integer from bits 30:21 (the S(4+s).(27-s) layout, s=6);
non-persp ufrac=19 (val<<19) put the integer 2 bits low → engine saw val/4
(32→texel 8 R4; 63→texel 15 R7). ufrac=21 (val<<21) should fix it. PERSPECTIVE
already defaults to ufrac=21 (=27-s_val), so the cube's path may already be
correct — the real cube bug may be just CLAMP→border (needs WRAP). [UV=(0,0)
read 0x0000 for both gradients — secondary; border was white so not border;
TEST 15 grid will show if ufrac=21 fixes it.]

**v15 RESULT (095173e, silicon 2026-07-09) — TEXTURING CONFIRMED on the
NON-persp path; PERSPECTIVE is BROKEN.** Fresh gradient, WRAP, sweep
{persp u21, non-persp u21, non-persp u19}, varying UV 0..TEX:
  - non-persp **ufrac=21**: PERFECT — R rises 0,4,8,…,28 and G rises
    5,10,16,21,26 EXACTLY matching expected (only the UV=TEX=64 edge reads
    0 = correct WRAP). **The texture engine works.**
  - non-persp **ufrac=19**: R = expected/4 → confirms ufrac=21, texel int =
    bits 30:21.
  - **persp ufrac=21: SATURATES** — R=G=31 for every non-zero UV (texel(63,63)),
    0 only at UV=0. Even at the probe's W=1.0 (a divide-by-1 that should be a
    no-op == non-persp) it saturates → a FORMAT/SEMANTIC bug specific to the
    perspective command (0101), NOT merely the divide.

**This re-roots the cube bug.** textured_cube uses perspective (W=1/Z_eye,
textured_cube.c:65) and ALREADY sets WRAP_REPEAT (line 267) — so "CLAMP→border"
was NEVER the cube's bug. The cube's symptom IS the broken perspective path.
(The entire v6–v14 "every texel is border" saga was the texprobe dead-texture
artifact — TEST 5/6 clobbering VRAM — not a driver bug; CLOSED.)

Datasheet formats all match what we program (3d_regs.txt:1586 TWS=S12.19;
1671-1987 TUS/TVS/TdUdX/TdVdX persp = S(4+s).(27-s)=S10.21; sign bit 0) yet
persp saturates. Datasheet is SILENT on the divide algorithm; no trusted driver
reference (kernel s3virge/DDX are 2D, Mesa s3v EXCLUDED per the source-trust
hierarchy). Only behavioral hint is 86Box (weak).

**v16 FIX LANDED (2026-07-09) — "non-persp now, persp after" (David's call):**
  1. `8813cc8` virge scales normalized [0,1] UV by texture side 2^s (the l10gl.h
     contract was violated — UV=1.0 picked texel 1, not 63; texprobe v1/v2 had
     assumed this scaling).
  2. `cc30cde` virge DEFAULTS to non-perspective (tex_dbg_nopersp=1) — persp
     saturates on real DX. The persp-debug axis toggles it to 0.
  3. `f8c974e` textured_cube drops LINEAR→NEAREST (LINEAR is NOT silicon-verified).
  RUN: `git pull && make -B BACKEND=virge texprobe && sudo ./texprobe` (TEST 15
  non-persp u21 should STILL be the perfect grid — confirms the scaling fix),
  then `make BACKEND=virge textured_cube && sudo ./textured_cube` — the cube
  should render its 6 textures (affine; mild swim during rotation = cosmetic).

**v16 VERIFIED ON SILICON (2026-07-09, David):** TEST 15 non-persp ufrac=21 =
exact grid (R 0,4,8,…,28; G 5,10,16,21,26) — UV-scaling fix correct, no
regression; non-persp u19 = R/4 (re-confirms ufrac=21); persp u21 still
saturates (unchanged — the "after" axis). textured_cube RENDERS its 6 textures
with the expected affine swim (David: "definitely texture swim, but it looks as
described"). The affine/NEAREST cube is DONE.

**PERSPECTIVE DEBUG ("after" axis) — DIVIDE DECODED v17 (silicon 2026-07-09,
David): TEST 16 (b772e8a+1df1387) swept programmed W (1/16..128) × U texel at
constant V=16. ALL 32 cells (R and G) match the model `texel = 128 · TUS / TWS`
(then mod 64 for WRAP, saturating to 63 when the value ≥ 2048). E.g. U4/W64 →
128·(4<<21)/(64<<19)=32→R16 (obs 16); U4/W128 → 16→R8 (obs 8); U16/W4 →
2048→sat→R31 (obs 31); U8/W4 → 1024 mod 64 = 0→R0 (obs 0).** So the persp engine
IS a divide (R falls as W rises — not a multiply, not W-ignored); it just has a
clean **2^7 = 128× scale factor** vs the expected U/W, so at W=1 it yields
512·U (saturated). The TEST 15 "U depends on V" was just saturation overflow —
at fixed V there is NO coupling (G tracks V/W symmetrically). Regs read back
correct (TUS=U<<21, TVS=V<<21, TWS=W<<19, CMD bits 30:27=0101 persp, bit26 WRAP)
so this is engine-format-side, not a driver programming error.

**FIX CONFIRMED + LANDED (silicon 2026-07-09):** TEST 17 swept persp ufrac
{9..15} at W=1 — **ufrac=12 renders R `4 8 12 16 20 24 28` EXACT** (neighbors
bracket it: 11→R/2, 10→R/4, 13→2×+wrap). And at ufrac=12 the W-sweep tracks U/W
cleanly (W=0.5→R16, W=1→R8, W=2→R4; W=4→R1, a 1-bit rounding at the extreme,
irrelevant — the cube's W≈0.17–0.25 is in the exact region). The `21−9=12` model
is confirmed. **Driver fix pushed** (this build):
- persp path uses **ufrac 12** (was 21); non-persp stays 21. virge.c.
- persp path **pre-multiplies U,V by W** (TUS=U·W, TVS=V·W) so the engine's
  (U·W)/W = U is perspective-correct (no swim). Standard homogeneous interp;
  airtight given the proven divide. virge.c.
- **tex_dbg_nopersp default → 0** (re-enable perspective); cube drops affine.
  l10gl_virge.c.
- new **tex_dbg_nopremult** knob (default 0=premult on); texprobe TEST 16/17 set
  it to 1 to keep isolating the raw divide (premult would cancel W and hide it).
**VERIFIED ON SILICON 2026-07-09 (David: "the cube looks perfect"): no texture
swim — perspective-correct hardware texturing works end to end. textured_cube
AXIS CLOSED.** See [[source-trust-hierarchy]].

VERIFIED FACTS (still hold): upload IS correct in VRAM (v3: 5/5 texels
match at 0x2bf200); coherence is NOT the issue (BAR0 O_SYNC, scantest same
path); texture fields decode correctly (max_d=bits11:8=6, format=bits7:5=
2=ARGB1555 — matches 86Box); TEX_BASE re-arm (8588c26) is KEPT as
defensive/correct. texprobe reaches struct virge_ctx via ctx.backend_data.
Build: `make -B BACKEND=virge texprobe`.

History (superseded, kept for the record): v3 wrongly claimed TEX_BASE
clobber was root cause (re-arm landed the reg, didn't fix it). v4/v5
concluded "engine ignores TEX_BASE, reads a fixed base" — the observation
(TEX_BASE/content/UV don't matter) was right but the REASON was wrong: it
borders everything, so of course none of those matter. v6 corrected it.

## Established engine facts (verified against 86Box, 2026-07-06)

Register-file architecture, from the MMIO decode in 86Box
`vid_s3_virge.c` (behavioral reference per repo rules — consult,
never copy):

- The three **2D banks (0xA4xx BitBLT / 0xA8xx line / 0xACxx poly)
  are aliases of ONE shared register file**; the **3D banks
  (0xB0xx line / 0xB4xx triangle) have their own separate file**. A
  2D kick does not invalidate 3D dest/stride/clip/Z state; one-time
  `engine_init_3d` programming is architecturally sound.
- **3D kick semantics:** with autoexecute OFF, the triangle launches
  on the **CMD_SET (0xB500) write** — all parameters must be written
  before it. 0xB57C (TY01/TY12 + L/R in bit 31) merely latches, and
  kicks only when AE is ON. L10GL's order (params → TY01_Y12 →
  CMD_SET last) is correct.
- **3D clip is dead state with HC off** (the rasterizer applies
  clip_t/b/l/r only when the triangle's CMD_SET has HC set; L10GL
  disables HC everywhere). The clip registers cannot cause cutoffs.
- **Write masks** (values silently truncated): base addresses
  `& 0x3FFFF8` on 4MB cards; strides `& 0xFF8`; clip fields and
  TY01/TY12 counts 11-bit.
- **Z pipeline:** span start `z = (base_z > 0) ? base_z<<1 : 0`
  (clamped non-negative); compare uses `z >> 16` as unsigned 16-bit
  vs the fetched word; ZBC codes match DB019-B p.129 / virge.h; on
  pass with ZUP the **new Zs is written back** (fail writes nothing);
  Z addresses = `z_base + y*z_str`, top-down, plain linear VRAM.
  Scale quirk: `TdZdX` accumulates in the post-shift domain
  (effectively 16 fractional bits) while TZS/`TdZdY` are pre-shift
  (15) — matters once real Z gradients are programmed.
- 86Box renders the current failing case full-screen under LESS — the
  observed DX behavior is **off the emulator's map**; from here only
  hardware probes are authoritative.

## Diagnostic inventory

- `sudo ./scantest` — phase 1: three CPU-drawn layout-hypothesis strips
  (C = 555/1600 must be the clean one); phase 2: full-screen CPU
  pattern at adopted geometry. No engine involvement.
- `sudo ./rawtri` (or compatibility `./triangle`), `./cube` — engine paths
  (2D clear + Z-clear + 3D).
- `sudo ./filltest` — 2D-fill readback: programs known rects via
  `virge_fill_rect`, CPU-reads VRAM back at corners/center, prints pass/fail
  + a bounding-box locater if a color lands wrong. PASSES as of `385d11d`
  (2D engine exonerated).
- `sudo ./tritest` — raw-path 3D triangle + Z-buffer readback (chip driver
  only, no frontend).
- `sudo ./gltritest` — the symptom-2 workhorse: full demo path
  (l10gl_create → clear → draw) with ZBC compare-code matrix, Z-clear
  coverage probe, and Z-profile-under-LESS/ALWAYS readbacks. Now the Z
  regression test: LESS row must be FULL. **Rebuild caveat: gltritest is
  NOT in `DEMOS`, so `make -B BACKEND=virge` does NOT rebuild it — use
  `make -B BACKEND=virge gltritest` explicitly.** A stale binary still
  reports `Z base: 0xea600` (pre-double-buffer layout) and the old vsync
  timeout.
- `sudo ./fliptest` — double-buffer groundwork probe (virge.c only): CPU-
  draws two patterns and cycles the CRTC start address through x1/x2/x4/x8
  divisors to settle the display-start unit on silicon, plus reports a
  working vsync source (0x3DA retrace vs VSY INT latch w/ VSY ENB).
  Settled x4/dword + VSY ENB on 2026-07-07.
- `sudo ./dztest` — per-pixel Z-gradient probe (virge.c only): draws a
  z=f(X) ramp triangle (dzdy=0) and a z=f(Y) ramp triangle (dzdx=0),
  CPU-reads the Z buffer, and reports the rendered per-pixel Z-word slope
  vs intended for BOTH axes (TdZdX and TdZdY). BOTH verified 1.000 on
  2026-07-07 — the Z-gradient pipeline is silicon-correct. NOT in `DEMOS`
  — build explicitly: `make -B BACKEND=virge dztest`.
- `sudo ./cubediag [angle_deg]` — rotating cube + per-face color legend
  (front-end path, same as cube.c). Full-saturation flat face colors + a
  static color-key on the right, to identify which face is which while
  inspecting the (monitor-side) bleed. An angle arg freezes one orientation
  and prints which faces are visible; otherwise slow-rotates. Build:
  `make -B BACKEND=virge cubediag`.
- `sudo ./seamtest` — measures the S3d triangle span fill rule on silicon
  (start/end edge, both L/R directions, integer vs half-integer X) and runs
  a two-triangle shared-edge overlap check. Result: coverage is WATERTIGHT
  (no double-draw) -- the end edge is exclusive for L/R=1, inclusive for
  L/R=0, combining to a clean partition at every shared edge. Refutes the
  "double-draw" bleed hypothesis. Build: `make -B BACKEND=virge seamtest`.
- `sudo ./cubefb [N]` — sweeps N (default 36) orientations, CPU-reads the
  framebuffer back (bypassing the monitor), and reports contamination
  signals per angle: BLENDS, MISPLACED (solid face color inside another
  face — the "blue on teal" detector), HOLES (background >1.5px inside
  the convex hull of the projected vertices — the "black vertex wedge" /
  coverage-gap detector), and EXCESS (info only). History: 10/36
  contaminated before `25786e7`, 9/36 after, 0/36 after the cull +
  base-prestep fix (but holes were not yet a signal); now the verdict
  tool for the float-dy slope fix. Build: `make -B BACKEND=virge cubefb`.
- `sudo ./diagap [angle-index]` (default 31 = 310°, cubefb's worst) —
  reproduces the cube's exact Left-face shared diagonal (vert0-vert7;
  tri A {0,4,7}=red with the diagonal as edge-12, tri B {0,7,3}=green
  with it as edge-01) in isolation, same rotation+projection as cubefb.
  Five passes (A-alone/B-alone/both-LESS×2 orders/both-ALWAYS) print the
  per-row gap between the green and red runs, then a 6th Z-readback pass
  draws A alone under Z=ALWAYS+Z-update and dumps A's written Z across
  the scanline. This pinned the notch to Z, not coverage (see below).
  Build: `make -B BACKEND=virge diagap`.
- `sudo ./fbtest` — fbdev-based pattern; useless on this machine (no
  /dev/fb0), kept for machines that have one.
- Boot log prints: FB/fbdev status, "CRTC raw"/"CRTC truth" dump
  (pre-takeover), takeover geometry + register readbacks, engine
  banner. Always ask for the full log with any photo.
- Reference sources: `docs/datasheets/DB019-B_*.pdf` (use `pdftotext -f
  <page> -l <page> -layout`), kernel `drivers/video/fbdev/s3fb.c`
  (fetch from kernel.org), 86Box `src/video/vid_s3_virge.c`.

## Recent commit trail (context for git archaeology)

`b155aaa` CRTC truth dump · `d3cca61` scantest v1 · `266ac9f`
wait_vsync latch fix · `bc0fc30` takeover v1 (CR67-only — put the
monitor out of range) · `50cab91` demos adopt geometry · `301d448`
takeover v2 (hmul=2 + quadword pitch — works; read its message for
the full timing-scaling story) · `048bc06` CR3B for doubled scanout ·
`c7c96f0` symptom 1 parked (monitor moiré) · `93e92ed`/`b0059e2`/
`385d11d` filltest + 2D fill direction fix (2D exonerated) ·
`e7b66bc`/`52ae126` tritest (cutoff correlates with Z on) ·
`7fa057e`..`01c220f` gltritest: ZBC matrix (LESS cuts at y≈234),
Z-clear coverage verified 0xFFFF, ZUP write-back missing from CPU
view — leading to the "Z unit addressing" analysis above.
`ae351e6` VIRGE_Z_FIXED ×65535 (symptom-2 fix #1) · `f0811f1` re-arm
program_3d_state per primitive (symptom-2 fix #2: 2D clobbers Z_STRIDE
to 0xFF8 on DX) — cutoff resolved · `2863663` fliptest (×4 start unit +
VSY ENB) · `b904aa4` wait_vsync: enable VSY interrupt · `8527a9e`
backend double-buffer primitives (2 color buffers, page flip) ·
`6e4f7f8` l10gl_swap_buffers frontend API · `7b4c67b` cube/textured_cube
swap each frame (tear-free). Open after this: back-face bleedthrough
(tiny cube depth range), Ctrl-C console restore (#7).
