/**
 * Holofoil — browser demo.
 *
 * Trading-card holographic foil. The one idea, from `AGENTS.md`: **the foil is
 * a reflection grating**. Light of wavelength λ reaches the eye off it in order
 * m only where the in-plane part of (v + l) is mλ/d along the grating's
 * direction g, so every pixel's colour is the grating equation's answer for the
 * eye, the lamp and the grating there, read through the CIE 1931 matching
 * functions. The lamp is a Gaussian in direction, so each order is a spectral
 * line with a centre and a width, and a table holds the colour of every such
 * line.
 *
 * Two halves, and they are not equally faithful here:
 *
 *   **The GPU half is the plugin's own GLSL.** `shaders.js` is the four
 *   `R"( ... )"` pieces of `source/Shaders.cpp` plus kVersion, spliced by
 *   `demo/tools/check_shaders.py --write` and never typed; the same script
 *   compares them character for character, and checks they are joined as
 *   `Assemble()` and InitGL join them, and `tools/verify.sh` runs it. They run
 *   as the plugin's two passes: the table (2048 x 129, built only when Groove
 *   Depth changes) and the foil, into the page's framebuffer. The table is the
 *   plugin's RGBA32F, sampled linearly, which needs EXT_color_buffer_float to
 *   draw and OES_texture_float_linear to filter; a browser without the second
 *   gets RGBA16F, and the status line under the picture says which.
 *
 *   **The CPU half is a port.** Between the PORT markers below: Controls.cpp
 *   (every mapping, the names, groups, order and defaults) and Card.cpp (the
 *   eye and the lamp in the card's frame, the lamp's covariance, the motion and
 *   its phase, the colour matching fit and the XYZ-to-display matrix), in
 *   double. Between the FRAME markers: the CPU half of Holofoil.cpp --
 *   ProcessOpenGL and ensureTable -- which turns the controls and the clock
 *   into each pass's uniforms. `demo/tools/crosscheck/` renders the plugin's
 *   own harness (`hftest --pipe`) and this page over the same frames and
 *   compares the pictures; it needs Chrome and a GPU, so it is run by hand
 *   (AGENTS.md).
 *
 * The page adds one clip to the kit's: `card`, a GLSL copy of the harness's
 * own test card (`buildCard()` in tools/hftest/main.cpp), because the defaults
 * put the art window exactly where that card has its art. It is pushed onto the
 * kit's SOURCES list at load, not written into the vendored kit.
 *
 * What the page cannot do as the plugin does, all said on the page: take its
 * clock and tempo from a host (the page's clock is seconds from frame deltas,
 * and Motion Sync's Beat and Bar run on the plugin's own 120 BPM, which is what
 * it does in a host that sends none).
 */

import { Program, PassBuffer, bindTexture } from './vendor/gl.js';
import { mountDemo } from './vendor/demo.js';
import { SOURCES } from './vendor/sources.js';
import * as S from './shaders.js';

const f32 = Math.fround;
const clampTo = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

// ==== BEGIN PORT ===========================================================
//
// Controls.h / Controls.cpp. Every ranged host parameter is 0..1 (an option
// holds its element's index), mapped here into the card's units: angles in
// radians, lengths on the card in picture widths, the grating's pitch and
// groove depth in nanometres.

const PT = {
  AREA: 0, PATTERN: 1,
  TILT_X: 2, TILT_Y: 3, MOTION: 4, MOTION_DEPTH: 5, MOTION_RATE: 6, MOTION_SYNC: 7, VIEWER: 8,
  LAMP_X: 9, LAMP_Y: 10, LAMP_DISTANCE: 11, LAMP_SIZE: 12, LAMP_LENGTH: 13, LAMP_BRIGHTNESS: 14,
  PITCH: 15, ANGLE: 16, GROOVE: 17, SCALE: 18, FACET: 19, ROUGHNESS: 20,
  INK: 21, BASE: 22,
  WINDOW_X: 23, WINDOW_Y: 24, WINDOW_WIDTH: 25, WINDOW_HEIGHT: 26, CORNER: 27, SOFTNESS: 28, THRESHOLD: 29,
  MIX: 30,
  ABOUT_FIRST: 31,
};

const Kind = { Standard: 0, Option: 1, XPos: 2, YPos: 3 };

const Area = { ArtWindow: 0, AroundArt: 1, WholeCard: 2, BrightInk: 3, DarkInk: 4, ClipAlpha: 5, Count: 6 };
const Pattern = { Rainbow: 0, Crosshatch: 1, Cosmos: 2, CrackedIce: 3, Sparkle: 4, Count: 5 };
const Motion = { Hold: 0, Tip: 1, Swing: 2, Circle: 3, LampOrbit: 4, Count: 5 };
const MotionSync = { Free: 0, Beat: 1, Bar: 2, Count: 3 };

const kPi = 3.14159265358979323846;
const kDegree = kPi / 180.0;

/** std::lround on the values a host sends: half away from zero. */
const lround = (v) => (v < 0 ? -Math.round(-v) : Math.round(v));
const OptionIndex = (value, count) => clampTo(lround(value), 0, count - 1);

const clamp01 = (v) => clampTo(v, 0.0, 1.0);
const geometric = (v, low, high) => low * Math.pow(high / low, clamp01(v));
function inverseGeometric(value, low, high) {
  const lo = Math.min(low, high);
  const hi = Math.max(low, high);
  return f32(Math.log(clampTo(value, lo, hi) / low) / Math.log(high / low));
}
/** -most .. +most, linearly, the middle at 0. */
const symmetric = (v, most) => (2.0 * clamp01(v) - 1.0) * most;
const inverseSymmetric = (value, most) => f32(clampTo(0.5 + 0.5 * value / most, 0.0, 1.0));
const inverseLinear = (value, most) => f32(clampTo(value / most, 0.0, 1.0));
/** 1/distance = (1 - v)^2 / closest. */
function inverseDistance(v, closest) {
  const r = 1.0 - clamp01(v);
  return r * r / closest;
}
function inverseOfInverseDistance(distance, closest) {
  if (!(distance > 0.0) || !Number.isFinite(distance)) return 1.0;
  return f32(clampTo(1.0 - Math.sqrt(clampTo(closest / distance, 0.0, 1.0)), 0.0, 1.0));
}

const kTiltMost = 40.0 * kDegree;
const kDepthMost = 40.0 * kDegree;
const kRateLow = 0.02, kRateHigh = 4.0;
const kViewerClosest = 0.6;
const kLampClosest = 0.4;
const kLampAngleMost = 80.0 * kDegree;
const kSizeLow = 0.2 * kDegree, kSizeHigh = 20.0 * kDegree;
const kLengthLow = 1.0, kLengthHigh = 20.0;
const kBrightnessMost = 4.0;
const kPitchLow = 500.0, kPitchHigh = 3000.0;
const kGrooveMost = 400.0;
const kScaleLow = 0.25, kScaleHigh = 4.0;
const kFacetMost = 20.0 * kDegree;
const kRoughMost = 10.0 * kDegree;
const kCornerMost = 0.25;
const kFeatherMost = 0.1;
const kInkBandMost = 0.5;

const kAreaNames = ['Art Window', 'Around Art', 'Whole Card', 'Bright Ink', 'Dark Ink', 'Clip Alpha'];
const kPatternNames = ['Rainbow', 'Crosshatch', 'Cosmos', 'Cracked Ice', 'Sparkle'];
const kMotionNames = ['Hold', 'Tip', 'Swing', 'Circle', 'Lamp Orbit'];
const kSyncNames = ['Free', 'Beat', 'Bar'];

const TiltFromParam = (value) => symmetric(value, kTiltMost);
const ParamFromTilt = (radians) => inverseSymmetric(radians, kTiltMost);

const MotionDepthFromParam = (value) => clamp01(value) * kDepthMost;
const ParamFromMotionDepth = (radians) => inverseLinear(radians, kDepthMost);

const MotionRateFromParam = (value) => geometric(value, kRateLow, kRateHigh);
const ParamFromMotionRate = (cycles) => inverseGeometric(cycles, kRateLow, kRateHigh);

const InverseViewerFromParam = (value) => inverseDistance(value, kViewerClosest);
const ParamFromViewer = (widths) => inverseOfInverseDistance(widths, kViewerClosest);
const InverseLampFromParam = (value) => inverseDistance(value, kLampClosest);
const ParamFromLampDistance = (widths) => inverseOfInverseDistance(widths, kLampClosest);

const LampAngleFromParam = (value) => symmetric(value, kLampAngleMost);
const ParamFromLampAngle = (radians) => inverseSymmetric(radians, kLampAngleMost);

const LampSizeFromParam = (value) => geometric(value, kSizeLow, kSizeHigh);
const ParamFromLampSize = (radians) => inverseGeometric(radians, kSizeLow, kSizeHigh);

const LampLengthFromParam = (value) => geometric(value, kLengthLow, kLengthHigh);
const ParamFromLampLength = (ratio) => inverseGeometric(ratio, kLengthLow, kLengthHigh);

const LampBrightnessFromParam = (value) => clamp01(value) * kBrightnessMost;
const ParamFromLampBrightness = (gain) => inverseLinear(gain, kBrightnessMost);

const PitchFromParam = (value) => geometric(value, kPitchLow, kPitchHigh);
const ParamFromPitch = (nanometres) => inverseGeometric(nanometres, kPitchLow, kPitchHigh);

const GratingAngleFromParam = (value) => clamp01(value) * kPi;
const ParamFromGratingAngle = (radians) => inverseLinear(radians, kPi);

const GrooveFromParam = (value) => clamp01(value) * kGrooveMost;
const ParamFromGroove = (nanometres) => inverseLinear(nanometres, kGrooveMost);

const PatternScaleFromParam = (value) => geometric(value, kScaleLow, kScaleHigh);
const ParamFromPatternScale = (factor) => inverseGeometric(factor, kScaleLow, kScaleHigh);

const FacetTiltFromParam = (value) => clamp01(value) * kFacetMost;
const ParamFromFacetTilt = (radians) => inverseLinear(radians, kFacetMost);

const RoughnessFromParam = (value) => clamp01(value) * kRoughMost;
const ParamFromRoughness = (radians) => inverseLinear(radians, kRoughMost);

const CornerFromParam = (value) => clamp01(value) * kCornerMost;
const WindowSoftnessFromParam = (value) => clamp01(value) * kFeatherMost;
const InkSoftnessFromParam = (value) => clamp01(value) * kInkBandMost;

const standard = (id, name, group, value) => ({ id, name, group, kind: Kind.Standard, defaultValue: f32(value), options: null });
const option = (id, name, group, value, names) => ({ id, name, group, kind: Kind.Option, defaultValue: f32(value), options: names });
const of = (id, name, group, kind, value) => ({ id, name, group, kind, defaultValue: f32(value), options: null });

// The defaults: a holo, a plain rainbow grating in the art window, on a card
// tipped gently toward and away at arm's length under a strip light above the
// viewer's head. Every number is in AGENTS.md with why.
const kTable = [
  option(PT.AREA, 'Foil Area', 'Finish', 0.0, kAreaNames),
  option(PT.PATTERN, 'Pattern', 'Finish', 0.0, kPatternNames),

  standard(PT.TILT_X, 'Tilt X', 'Card', 0.5),
  standard(PT.TILT_Y, 'Tilt Y', 'Card', 0.5),
  option(PT.MOTION, 'Motion', 'Card', 1.0, kMotionNames),
  standard(PT.MOTION_DEPTH, 'Motion Depth', 'Card', ParamFromMotionDepth(12.0 * kDegree)),
  standard(PT.MOTION_RATE, 'Motion Rate', 'Card', ParamFromMotionRate(0.2)),
  option(PT.MOTION_SYNC, 'Motion Sync', 'Card', 0.0, kSyncNames),
  standard(PT.VIEWER, 'Viewer Distance', 'Card', ParamFromViewer(2.5)),

  standard(PT.LAMP_X, 'Lamp X', 'Lamp', 0.5),
  standard(PT.LAMP_Y, 'Lamp Y', 'Lamp', ParamFromLampAngle(35.0 * kDegree)),
  standard(PT.LAMP_DISTANCE, 'Lamp Distance', 'Lamp', ParamFromLampDistance(3.0)),
  standard(PT.LAMP_SIZE, 'Lamp Size', 'Lamp', ParamFromLampSize(2.0 * kDegree)),
  standard(PT.LAMP_LENGTH, 'Lamp Length', 'Lamp', ParamFromLampLength(8.0)),
  standard(PT.LAMP_BRIGHTNESS, 'Lamp Brightness', 'Lamp', ParamFromLampBrightness(1.5)),

  standard(PT.PITCH, 'Pitch', 'Foil', ParamFromPitch(900.0)),
  standard(PT.ANGLE, 'Grating Angle', 'Foil', ParamFromGratingAngle(90.0 * kDegree)),
  standard(PT.GROOVE, 'Groove Depth', 'Foil', ParamFromGroove(130.0)),
  standard(PT.SCALE, 'Pattern Scale', 'Foil', ParamFromPatternScale(1.0)),
  standard(PT.FACET, 'Facet Tilt', 'Foil', ParamFromFacetTilt(6.0 * kDegree)),
  standard(PT.ROUGHNESS, 'Roughness', 'Foil', ParamFromRoughness(0.5 * kDegree)),

  standard(PT.INK, 'Ink', 'Print', 0.6),
  standard(PT.BASE, 'Base', 'Print', 0.7),

  of(PT.WINDOW_X, 'Window X', 'Area', Kind.XPos, 0.5),
  of(PT.WINDOW_Y, 'Window Y', 'Area', Kind.YPos, 0.64),
  standard(PT.WINDOW_WIDTH, 'Window Width', 'Area', 0.84),
  standard(PT.WINDOW_HEIGHT, 'Window Height', 'Area', 0.42),
  standard(PT.CORNER, 'Corner', 'Area', 0.04),
  standard(PT.SOFTNESS, 'Softness', 'Area', 0.0),
  standard(PT.THRESHOLD, 'Threshold', 'Area', 0.5),

  standard(PT.MIX, 'Mix', 'Output', 1.0),
];
if (kTable.length !== PT.ABOUT_FIRST) throw new Error('every control needs a row in kTable');

const InfoOf = (id) => kTable[Math.min(id, PT.ABOUT_FIRST - 1)];

// Card.h / Card.cpp: the card, the eye and the lamp, as numbers. Everything is
// in the CARD's frame: x across the picture, y up it, z out of it toward the
// viewer, lengths in picture widths from its centre. Tilting the card is the
// same as moving the eye and the lamp round a card that stays put.

/** r_card = Ry( Tx ) Rx( -Ty ) r_room. */
function toCard(tiltX, tiltY, x, y, z) {
  const cy = Math.cos(tiltY), sy = Math.sin(tiltY);
  const y1 = y * cy + z * sy;
  const z1 = -y * sy + z * cy;
  const cx = Math.cos(tiltX), sx = Math.sin(tiltX);
  const x2 = x * cx + z1 * sx;
  const z2 = -x * sx + z1 * cx;
  return [x2, y1, z2];
}

/** A point (w = 1) or, at inverse distance 0, a direction (w = 0). */
function placed([x, y, z], inverseDistance) {
  if (inverseDistance > 0.0) return { x: x / inverseDistance, y: y / inverseDistance, z: z / inverseDistance, w: 1.0 };
  return { x, y, z, w: 0.0 };
}

const EyeInCard = (tiltX, tiltY, inverseDistance) => placed(toCard(tiltX, tiltY, 0.0, 0.0, 1.0), inverseDistance);

function LampInCard(tiltX, tiltY, lampX, lampY, inverseDistance) {
  const x = Math.cos(lampY) * Math.sin(lampX);
  const y = Math.sin(lampY);
  const z = Math.cos(lampY) * Math.cos(lampX);
  return placed(toCard(tiltX, tiltY, x, y, z), inverseDistance);
}

/** How far along its length a strip may reach, either side: 60 degrees, as a direction cosine. */
const kLongestStrip = 0.866;

function LampSpread(tiltX, tiltY, lampX, lampY, size, length) {
  // The two ways the lamp extends, as room directions at right angles to it:
  // level (along the strip) and up.
  const [ax, ay] = toCard(tiltX, tiltY, Math.cos(lampX), 0.0, -Math.sin(lampX));
  const [bx, by] = toCard(tiltX, tiltY, -Math.sin(lampY) * Math.sin(lampX), Math.cos(lampY), -Math.sin(lampY) * Math.cos(lampX));
  const along = Math.min(size * length, kLongestStrip);
  const across = size;
  return {
    xx: along * along * ax * ax + across * across * bx * bx,
    xy: along * along * ax * ay + across * across * bx * by,
    yy: along * along * ay * ay + across * across * by * by,
  };
}

/** Motion applied to the hand-set angles. */
function PoseOf(motion, depth, phase, set) {
  const pose = { ...set };
  const a = 2.0 * kPi * phase;
  switch (motion) {
    case Motion.Tip: pose.tiltY += depth * Math.sin(a); break;
    case Motion.Swing: pose.tiltX += depth * Math.sin(a); break;
    case Motion.Circle:
      pose.tiltX += depth * Math.sin(a);
      pose.tiltY += depth * Math.cos(a);
      break;
    case Motion.LampOrbit:
      pose.lampX += depth * Math.sin(a);
      pose.lampY += depth * Math.cos(a);
      break;
    default: break;
  }
  return pose;
}

/** Motion's phase: cycles, advanced by rate x this frame's time and reduced to [0, 1). */
class Wobble {
  constructor() {
    this.phase = 0.0;
    this.elapsed = 0.0;
  }

  /** `rate` cycles per unit, `units` of them elapsed this frame (seconds, beats or bars). */
  Advance(rate, units) {
    this.elapsed += units;
    this.phase += rate * units;
    this.phase -= Math.floor(this.phase);
  }

  Phase() {
    return this.phase;
  }
}

// The colour: Wyman, Sloan and Shirley's multi-lobe fit to the CIE 1931
// 2-degree matching functions, and IEC 61966-2-1's XYZ-to-sRGB matrix white
// balanced on the fit's own equal-energy white, by the table pass's
// quadrature.
const kVisibleLow = 360.0;
const kVisibleHigh = 830.0;
const kSpectrumTaps = 96;

function lobe(x, mu, below, above) {
  const t = (x - mu) * (x < mu ? below : above);
  return Math.exp(-0.5 * t * t);
}

function ColourMatching(lambda) {
  return [
    0.362 * lobe(lambda, 442.0, 0.0624, 0.0374) + 1.056 * lobe(lambda, 599.8, 0.0264, 0.0323)
      - 0.065 * lobe(lambda, 501.1, 0.0490, 0.0382),
    0.821 * lobe(lambda, 568.8, 0.0213, 0.0247) + 0.286 * lobe(lambda, 530.9, 0.0613, 0.0322),
    1.217 * lobe(lambda, 437.0, 0.0845, 0.0278) + 0.681 * lobe(lambda, 459.0, 0.0385, 0.0725),
  ];
}

/** Column-major, as glUniformMatrix3fv takes it. */
function XyzToDisplay() {
  const m = [
    [3.2406, -1.5372, -0.4986],
    [-0.9689, 1.8758, 0.0415],
    [0.0557, -0.2040, 1.0570],
  ];
  const white = [0.0, 0.0, 0.0];
  const step = (kVisibleHigh - kVisibleLow) / kSpectrumTaps;
  for (let i = 0; i < kSpectrumTaps; i += 1) {
    const [x, y, z] = ColourMatching(kVisibleLow + (i + 0.5) * step);
    white[0] += x * step;
    white[1] += y * step;
    white[2] += z * step;
  }
  const out = new Float32Array(9);
  for (let row = 0; row < 3; row += 1) {
    const made = m[row][0] * white[0] + m[row][1] * white[1] + m[row][2] * white[2];
    for (let col = 0; col < 3; col += 1) out[col * 3 + row] = m[row][col] / made;
  }
  return out;
}

// Shaders.h: the table's size, and each pattern's natural cell in picture widths.
const kTableColumns = 2048;
const kTableHeight = 2 * 64 + 1;
const kHatchSpacing = 0.012;
const kCosmosCell = 0.06;
const kIceCell = 0.07;
const kSparkleCell = 0.004;

// ==== END PORT =============================================================

// ==== BEGIN FRAME ==========================================================
//
// Holofoil.cpp, the CPU half of a frame. Each pass binds and draws as the
// plugin's does; `input` is the clip as the kit hands it over, the size of the
// composition, which is the host's viewport here (ViewOrigin 0, 0).

/** Seconds of host time a single frame may advance by. */
const kMaxFrameDelta = 0.25;

/** The pattern's natural cell, picture widths. */
function cellOf(pattern) {
  switch (pattern) {
    case Pattern.Crosshatch: return kHatchSpacing;
    case Pattern.Cosmos: return kCosmosCell;
    case Pattern.CrackedIce: return kIceCell;
    case Pattern.Sparkle: return kSparkleCell;
    default: return 1.0;
  }
}

function setInt2(program, name, x, y) {
  const loc = program.location(name);
  if (loc === null) { program.missing.add(name); return; }
  program.gl.uniform2i(loc, x | 0, y | 0);
}

function unbindTextureUnits(gl, count) {
  for (let unit = count - 1; unit >= 0; unit -= 1) bindTexture(gl, unit, null);
}

class HolofoilInstance {
  /** `tableFormat` is gl.RGBA32F, or gl.RGBA16F where 32-bit floats cannot be filtered. */
  constructor(gl, quad, programs, tableFormat) {
    this.gl = gl;
    this.quad = quad;
    this.programs = programs;
    this.tableFormat = tableFormat;

    this.params = new Float32Array(PT.ABOUT_FIRST);
    for (let id = 0; id < PT.ABOUT_FIRST; id += 1) this.params[id] = InfoOf(id).defaultValue;

    this.table = new PassBuffer(gl, { filter: 'linear' }); // kTableColumns x kTableHeight
    this.tableGroove = -1.0; // the Groove Depth it was built for
    this.tableHooks = -1;    // and the hooks
    this.tableBuilds = 0;

    this.wobble = new Wobble();
    this.lastPose = { tiltX: 0.0, tiltY: 0.0, lampX: 0.0, lampY: 0.0 };

    this.now = 0.0;
    this.lastNow = -1.0;

    // The beat, as SetBeatInfo last said. No host here ever calls it.
    this.bpm = 120.0;

    this.hooks = 0;
  }

  SetFloatParameter(index, value) {
    if (index >= PT.ABOUT_FIRST) return;
    this.params[index] = f32(value);
  }

  ensureTable() {
    const gl = this.gl;
    this.table.ensure(kTableColumns, kTableHeight, this.tableFormat);

    const groove = GrooveFromParam(this.params[PT.GROOVE]);
    const tableAffect = this.hooks & 8; // kHookNoEfficiency
    if (groove === this.tableGroove && tableAffect === this.tableHooks) return;

    const table = this.programs.table;
    this.table.bind();
    table.use();
    table.set('GrooveDepth', f32(groove));
    gl.uniformMatrix3fv(table.location('XyzToDisplay'), false, XyzToDisplay());
    table.setInt('Hooks', this.hooks);
    this.quad.draw();
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);

    this.tableGroove = groove;
    this.tableHooks = tableAffect;
    this.tableBuilds += 1;
  }

  /** ProcessOpenGL, with the page's clock in place of UpdateClock's vote. */
  ProcessOpenGL(input, time) {
    if (!input || input.width === 0 || input.height === 0) return;
    const gl = this.gl;
    const p = this.params;
    const { width, height } = input;
    gl.disable(gl.BLEND);

    // Time, and the card's motion.
    this.now = time;
    const dt = this.lastNow >= 0.0 ? clampTo(this.now - this.lastNow, 0.0, kMaxFrameDelta) : 0.0;
    this.lastNow = this.now;

    const sync = OptionIndex(p[PT.MOTION_SYNC], MotionSync.Count);
    const units = sync === MotionSync.Beat ? dt * this.bpm / 60.0 : sync === MotionSync.Bar ? dt * this.bpm / 240.0 : dt;
    this.wobble.Advance(MotionRateFromParam(p[PT.MOTION_RATE]), units);

    const set = {
      tiltX: TiltFromParam(p[PT.TILT_X]),
      tiltY: TiltFromParam(p[PT.TILT_Y]),
      lampX: LampAngleFromParam(p[PT.LAMP_X]),
      lampY: LampAngleFromParam(p[PT.LAMP_Y]),
    };
    const motion = OptionIndex(p[PT.MOTION], Motion.Count);
    const pose = PoseOf(motion, MotionDepthFromParam(p[PT.MOTION_DEPTH]), this.wobble.Phase(), set);
    this.lastPose = pose;

    const eye = EyeInCard(pose.tiltX, pose.tiltY, InverseViewerFromParam(p[PT.VIEWER]));
    const lamp = LampInCard(pose.tiltX, pose.tiltY, pose.lampX, pose.lampY, InverseLampFromParam(p[PT.LAMP_DISTANCE]));
    const spread = LampSpread(pose.tiltX, pose.tiltY, pose.lampX, pose.lampY, LampSizeFromParam(p[PT.LAMP_SIZE]),
      LampLengthFromParam(p[PT.LAMP_LENGTH]));

    // 1. The colour table, if the groove has changed.
    this.ensureTable();

    // The grating and the area, in double, then handed over as floats.
    const pattern = OptionIndex(p[PT.PATTERN], Pattern.Count);
    const area = OptionIndex(p[PT.AREA], Area.Count);
    const angle = GratingAngleFromParam(p[PT.ANGLE]);
    const hatchA = angle + 45.0 * kDegree;
    const hatchB = angle - 45.0 * kDegree;

    const halfW = 0.5 * clamp01(p[PT.WINDOW_WIDTH]) * width;
    const halfH = 0.5 * clamp01(p[PT.WINDOW_HEIGHT]) * height;
    const corner = Math.min(CornerFromParam(p[PT.CORNER]) * height, halfW, halfH);
    const ramp = 1.0 + WindowSoftnessFromParam(p[PT.SOFTNESS]) * height;

    // 2. The foil, into the host's framebuffer and viewport.
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, width, height);
    const foil = this.programs.foil;
    foil.use();
    bindTexture(gl, 0, input.texture);
    bindTexture(gl, 1, this.table.texture);
    foil.setSampler('InputTexture', 0);
    foil.setSampler('Table', 1);
    setInt2(foil, 'Size', width, height);
    setInt2(foil, 'ViewOrigin', 0, 0);
    foil.set('PxToWidth', f32(1.0 / width));

    foil.set('Eye', f32(eye.x), f32(eye.y), f32(eye.z), f32(eye.w));
    foil.set('Lamp', f32(lamp.x), f32(lamp.y), f32(lamp.z), f32(lamp.w));
    foil.set('LampSpread', f32(spread.xx), f32(spread.xy), f32(spread.yy));
    foil.set('RoughSigma', f32(RoughnessFromParam(p[PT.ROUGHNESS])));
    foil.set('Brightness', f32(LampBrightnessFromParam(p[PT.LAMP_BRIGHTNESS])));

    foil.set('Pitch', f32(PitchFromParam(p[PT.PITCH])));
    foil.set('Direction', f32(Math.cos(angle)), f32(Math.sin(angle)));
    foil.set('HatchA', f32(Math.cos(hatchA)), f32(Math.sin(hatchA)));
    foil.set('HatchB', f32(Math.cos(hatchB)), f32(Math.sin(hatchB)));
    foil.setInt('PatternIndex', pattern);
    foil.set('Cell', f32(cellOf(pattern) * PatternScaleFromParam(p[PT.SCALE])));
    foil.set('FacetSlope', f32(Math.tan(FacetTiltFromParam(p[PT.FACET]))));

    foil.set('Ink', f32(clamp01(p[PT.INK])));
    foil.set('Base', f32(clamp01(p[PT.BASE])));

    foil.setInt('AreaIndex', area);
    foil.set('Window', f32(clamp01(p[PT.WINDOW_X]) * width), f32(clamp01(p[PT.WINDOW_Y]) * height), f32(halfW), f32(halfH));
    foil.set('Corner', f32(corner));
    foil.set('Ramp', f32(ramp));
    foil.set('Threshold', f32(clamp01(p[PT.THRESHOLD])));
    foil.set('Band', f32(InkSoftnessFromParam(p[PT.SOFTNESS])));
    foil.set('MixAmount', f32(clamp01(p[PT.MIX])));
    foil.setInt('Hooks', this.hooks);
    this.quad.draw();
    unbindTextureUnits(gl, 2);
  }

  dispose() {
    this.table.dispose();
    this.tableGroove = -1.0;
    this.tableHooks = -1;
  }
}

// ==== END FRAME ============================================================

/**
 * The table's format in this browser: the plugin's RGBA32F where 32-bit float
 * textures can be filtered, RGBA16F where they cannot. WebGL2 samples an
 * unfilterable texture under GL_LINEAR as black, which would leave every
 * colour out and the mirror white: a plausible wrong picture.
 */
function tableFormatFor(gl) {
  return gl.getExtension('OES_texture_float_linear') ? gl.RGBA32F : gl.RGBA16F;
}

function createRenderer(gl, quad) {
  // InitGL: the vertex stage is kVersion + kQuadVertex; each fragment stage is
  // Assemble( pass ) = kVersion + kCommon + the pass's fragment.
  const vertex = S.VERSION + S.QUAD_VERTEX;
  const assemble = (fragment) => S.VERSION + S.COMMON + fragment;
  const programs = {
    table: new Program(gl, vertex, assemble(S.TABLE_FRAGMENT), 'table'),
    foil: new Program(gl, vertex, assemble(S.FOIL_FRAGMENT), 'foil'),
  };

  const tableFormat = tableFormatFor(gl);
  const tableFormatName = tableFormat === gl.RGBA32F ? 'RGBA32F, as the plugin’s' : 'RGBA16F (this browser cannot filter 32-bit floats)';
  const instance = new HolofoilInstance(gl, quad, programs, tableFormat);

  // For demo/tools/crosscheck/: a FRESH instance (hftest's is one too) run over
  // the given frames on the harness's clock, frame / fps seconds, and read back
  // as RGBA8, top row first, as `hftest --pipe` writes it. Settings are by the
  // plugin's display name, an option by its element's name, as `--set` takes
  // them. `half` forces the RGBA16F table a browser without
  // OES_texture_float_linear gets, so the crosscheck can say what that costs.
  // It runs in one task, so the page's own frames cannot interleave; the next
  // one puts the canvas back to the composition's size.
  window.holofoilDemo = {
    tableFormat: tableFormatName,

    /** kTable as `hftest --list` prints it: name, kind and default. */
    controls() {
      const kinds = ['standard', 'option', 'x', 'y'];
      return kTable.map((row) => ({ id: row.id, name: row.name, kind: kinds[row.kind], default: row.defaultValue, options: row.options }));
    },

    renderCase({ settings = {}, frames, width, height, fps = 60, count, half = false }) {
      const run = new HolofoilInstance(gl, quad, programs, half ? gl.RGBA16F : tableFormat);
      for (const [name, value] of Object.entries(settings)) {
        const info = kTable.find((row) => row.name === name);
        if (!info) throw new Error(`no parameter called '${name}'`);
        const index = info.options ? info.options.indexOf(value) : -1;
        run.SetFloatParameter(info.id, index >= 0 ? index : Number(value));
      }
      const texture = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, width, height);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      const canvas = gl.canvas;
      canvas.width = width;
      canvas.height = height;
      const row = width * 4;
      const flipped = new Uint8Array(row * height);
      const frameBytes = row * height;
      const total = count ?? frames.length / frameBytes;
      for (let f = 0; f < total; f += 1) {
        const frame = frames.subarray(f * frameBytes, (f + 1) * frameBytes);
        for (let y = 0; y < height; y += 1) flipped.set(frame.subarray(y * row, (y + 1) * row), (height - 1 - y) * row);
        gl.bindTexture(gl.TEXTURE_2D, texture);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, flipped);
        gl.bindTexture(gl.TEXTURE_2D, null);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, width, height);
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        run.ProcessOpenGL({ texture, width, height }, f / fps);
      }
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, flipped);
      const out = new Uint8Array(row * height);
      for (let y = 0; y < height; y += 1) out.set(flipped.subarray(y * row, (y + 1) * row), (height - 1 - y) * row);
      run.dispose();
      gl.deleteTexture(texture);
      return out;
    },
  };

  current = {
    gl,
    instance,
    programs,

    render({ input, params, time }) {
      // The host restates every parameter each frame.
      for (const p of PARAMS) instance.SetFloatParameter(p.slot, params.get(p.id));
      instance.ProcessOpenGL(input, time);
    },

    /** What the status line under the picture reports. */
    state() {
      return { pose: instance.lastPose, builds: instance.tableBuilds, tableFormat: tableFormatName };
    },
  };
  return current;
}

/** The renderer mountDemo made, for the status line. */
let current = null;

//===========================================================================
// The card: tools/hftest/main.cpp's buildCard() as a clip, so the page can
// show the plugin on the picture its defaults were set on. A trading card's
// layout -- a border, a name bar, an art window (a sky, a sun, mountains and
// a lake) exactly where the default Window puts it, a type line, a text box
// of grey lines and a strip of small print -- stretched to the composition as
// the harness stretches it. Time slides the sun, so the art is live.
//===========================================================================
const CARD = `
uniform float Time;
uniform vec2 Resolution;
in vec2 uv;
out vec4 fragColor;

uint pcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
float hash01( uint a, uint b )
{
	return float( pcg( a * 2654435761u ^ pcg( b + 0x9e3779b9u ) ) ) * ( 1.0 / 4294967296.0 );
}
bool inBox( float u, float v, float u0, float v0, float u1, float v1 )
{
	return u >= u0 && u < u1 && v >= v0 && v < v1;
}

void main()
{
	//The pixel, rows from the bottom, as the harness builds it.
	vec2 px      = floor( gl_FragCoord.xy );
	float u      = ( px.x + 0.5 ) / Resolution.x;
	float v      = ( px.y + 0.5 ) / Resolution.y;
	float aspect = Resolution.x / Resolution.y;
	vec3 c       = vec3( 0.16, 0.18, 0.24 );//the border
	if( inBox( u, v, 0.035, 0.025, 0.965, 0.975 ) )
	{
		//The frame: a cool metal-looking gradient.
		float k = 0.5 + 0.5 * ( v - u );
		c       = vec3( 0.50 + 0.12 * k, 0.58 + 0.10 * k, 0.62 + 0.08 * k );
	}
	if( inBox( u, v, 0.07, 0.885, 0.93, 0.945 ) )
	{
		c          = vec3( 0.93, 0.90, 0.82 );
		int letter = int( floor( ( u - 0.1 ) * 60.0 ) );
		if( u > 0.1 && u < 0.52 && v > 0.9 && v < 0.93 && letter % 5 != 4 )
			c = vec3( 0.12 );
		if( u > 0.82 && u < 0.9 && v > 0.9 && v < 0.93 )
			c = vec3( 0.75, 0.15, 0.12 );
	}
	if( inBox( u, v, 0.08, 0.43, 0.92, 0.85 ) )
	{
		//The art: the window's own coordinates, 0..1 up and across.
		float au = ( u - 0.08 ) / 0.84, av = ( v - 0.43 ) / 0.42;
		float k  = clamp( ( av - 0.35 ) / 0.65, 0.0, 1.0 );
		c        = vec3( 0.98 - 0.80 * k, 0.62 - 0.30 * k, 0.38 + 0.35 * k );
		float sx = 0.66 - 0.05 * sin( 0.6 * Time ), sy = 0.56;
		float d  = length( vec2( ( au - sx ) * 0.84 * aspect, ( av - sy ) * 0.42 ) );
		if( d < 0.06 )
			c = vec3( 1.0, 0.92, 0.6 );
		if( av > 0.72 && pcg( uint( px.x ) * 7919u + uint( px.y ) * 104729u ) % 900u == 0u )
			c = vec3( 1.0 );
		float ridge = 0.38 + 0.12 * abs( sin( au * 9.0 ) ) * ( 0.6 + 0.4 * sin( au * 23.0 + 1.0 ) );
		if( av < ridge )
			c = vec3( 0.22, 0.14, 0.30 );
		if( av < 0.22 )
		{
			//The lake: the sky again, darker, with ripples.
			float ripple = 0.5 + 0.5 * sin( av * 180.0 + au * 6.0 );
			c            = vec3( 0.30 + 0.25 * ripple, 0.30 + 0.12 * ripple, 0.48 + 0.10 * ripple );
		}
	}
	if( inBox( u, v, 0.07, 0.375, 0.93, 0.415 ) )
	{
		c = vec3( 0.84, 0.85, 0.84 );
		if( u > 0.1 && u < 0.45 && v > 0.385 && v < 0.405 )
			c = vec3( 0.2 );
	}
	if( inBox( u, v, 0.08, 0.1, 0.92, 0.36 ) )
	{
		c            = vec3( 0.95, 0.93, 0.86 );
		float rows   = ( 0.345 - v ) / 0.034;
		int line     = int( floor( rows ) );
		float inLine = rows - float( line );
		float len    = 0.55 + 0.3 * hash01( uint( line ), 3u );
		if( line >= 0 && line < 6 && inLine > 0.25 && inLine < 0.7 && u > 0.11 && u < 0.11 + len * 0.78 )
			c = vec3( 0.32 );
	}
	if( inBox( u, v, 0.1, 0.045, 0.6, 0.07 ) && int( floor( u * 140.0 ) ) % 4 != 3 )
		c = vec3( 0.75 );
	fragColor = vec4( c, 1.0 );
}
`;

if (!SOURCES.some((s) => s.id === 'card')) {
  SOURCES.unshift({
    id: 'card',
    name: 'Trading card',
    hint: 'The plugin harness’s own test card (hftest), drawn in the page: the art window is exactly where the default Window puts it, and the sun moves so the art is live.',
    fragment: CARD,
  });
}

//===========================================================================
// The parameters: the constructor's, in kTable's order and groups, with the
// plugin's names, element lists and defaults. Hints from the user guide and
// Controls.h.
//===========================================================================
const V = (v) => f32(v);
const fix = (n, d = 2) => n.toFixed(d);
const signed = (n, d = 1) => `${n >= 0 ? '+' : '−'}${Math.abs(n).toFixed(d)}`;
const sig = (n) => (n >= 100 ? n.toFixed(0) : n >= 10 ? n.toFixed(1) : n >= 1 ? n.toFixed(2) : n.toFixed(3));
const deg = (radians) => radians / kDegree;
const distance = (inverse) => (inverse > 0.0 ? `${sig(1.0 / inverse)} w` : '∞');

/** id, ParamId, display, hint. */
const SPEC = [
  ['area', PT.AREA, null, 'Where the foil is. Art Window is a holo, Around Art a reverse holo (its exact complement), Whole Card a full foil; Bright Ink and Dark Ink put it where the print is lighter or darker than Threshold, Clip Alpha where the clip is opaque.'],
  ['pattern', PT.PATTERN, null, 'What is pressed into the foil. Rainbow is one grating; Crosshatch two families of diagonal lines; Cosmos discs, each its own grating, on a plain one; Cracked Ice shards; Sparkle tiny tilted facets. The cell patterns flash rather than sweep.'],

  ['tiltX', PT.TILT_X, (v) => `${signed(deg(TiltFromParam(V(v))))}°`, 'Turns the card side to side, ±40°; the middle is square on.'],
  ['tiltY', PT.TILT_Y, (v) => `${signed(deg(TiltFromParam(V(v))))}°`, 'Tips the top of the card toward or away from you, ±40°.'],
  ['motion', PT.MOTION, null, 'Hold leaves the card where Tilt X and Y put it. Tip rocks it toward and away (the default, what sweeps the rainbow under a lamp overhead), Swing side to side, Circle round a circle; Lamp Orbit holds the card and moves the lamp round its place.'],
  ['motionDepth', PT.MOTION_DEPTH, (v) => `${fix(deg(MotionDepthFromParam(V(v))), 1)}°`, 'How far Motion goes, 0 to 40°.'],
  ['motionRate', PT.MOTION_RATE, (v) => `${sig(MotionRateFromParam(V(v)))} cyc`, 'How fast, 0.02 to 4 cycles, geometrically: a second, a beat or a bar, as Motion Sync says.'],
  ['motionSync', PT.MOTION_SYNC, null, 'What Motion Rate counts in: seconds, or beats or bars of the host’s tempo. No host sends one here, so Beat and Bar run on the plugin’s own 120 BPM, as it does in a host that sends none.'],
  ['viewer', PT.VIEWER, (v) => distance(InverseViewerFromParam(V(v))), 'How far your eye is from the card, in picture widths: 0.6 to infinitely far. Close in, each part of the card sees you from a different angle, so the colour spreads across it.'],

  ['lampX', PT.LAMP_X, (v) => `${signed(deg(LampAngleFromParam(V(v))), 0)}°`, 'Where the lamp is across, seen from the middle of the card, ±80°.'],
  ['lampY', PT.LAMP_Y, (v) => `${signed(deg(LampAngleFromParam(V(v))), 0)}°`, 'Where the lamp is up, seen from the middle of the card, ±80°. 35° up by default.'],
  ['lampDistance', PT.LAMP_DISTANCE, (v) => distance(InverseLampFromParam(V(v))), 'How far the lamp is, in picture widths: 0.4 to infinitely far. A near lamp spreads the colour the way a near eye does.'],
  ['lampSize', PT.LAMP_SIZE, (v) => `${sig(deg(LampSizeFromParam(V(v))))}°`, 'How big the lamp looks from the card, 0.2 to 20°, geometrically. Small is a bare bulb and pure colour; large a softbox and silver.'],
  ['lampLength', PT.LAMP_LENGTH, (v) => `×${sig(LampLengthFromParam(V(v)))}`, 'Stretches the lamp into a strip light running level across the room, 1 (round) to 20 times as long as it is wide. Its long way is capped at 60° either side.'],
  ['lampBrightness', PT.LAMP_BRIGHTNESS, (v) => fix(LampBrightnessFromParam(V(v))), 'How bright the lamp is, 0 to 4, where 1 is as bright as the paper’s white.'],

  ['pitch', PT.PITCH, (v) => `${PitchFromParam(V(v)).toFixed(0)} nm`, 'The spacing of the grating’s ridges, 500 to 3000 nm, geometrically. Finer ridges throw the colours further apart, so the rainbow sweeps faster as the card tips.'],
  ['angle', PT.ANGLE, (v) => `${deg(GratingAngleFromParam(V(v))).toFixed(0)}°`, 'Turns the grating, 0 to 180°. At 90° its lines run across the card, so Tip sweeps the colour; turn them to run up the card and it is Swing that does.'],
  ['groove', PT.GROOVE, (v) => `${GrooveFromParam(V(v)).toFixed(0)} nm`, 'How deep the ridges are, 0 to 400 nm: how much light goes into the colours and how much stays in the mirror. Below about 80 nm it is a cold, mostly silver foil; around 210 nm the mirror turns magenta. Changing it rebuilds the colour table.'],
  ['scale', PT.SCALE, (v) => `×${sig(PatternScaleFromParam(V(v)))}`, 'Sizes the cells of Crosshatch, Cosmos, Cracked Ice and Sparkle, a quarter to four times their natural size. Small cells shimmer at small compositions.'],
  ['facet', PT.FACET, (v) => `${fix(deg(FacetTiltFromParam(V(v))), 1)}°`, 'The most a cell’s surface leans, 0 to 20°: each cell catches the lamp at a different moment.'],
  ['roughness', PT.ROUGHNESS, (v) => `${fix(deg(RoughnessFromParam(V(v))), 2)}°`, 'Blurs the surface’s direction under each pixel, 0 to 10°. It is exactly a bigger, dimmer lamp, so it takes the colour toward silver.'],

  ['ink', PT.INK, (v) => fix(V(v)), 'How much the print filters the foil. At 0 the foil’s light is added over the clip; at 1 it shows through light ink, takes the colour of coloured ink and does not get through black.'],
  ['base', PT.BASE, (v) => fix(V(v)), 'How much of the paper’s white the foil replaces under the print. It only acts where Ink does.'],

  ['windowX', PT.WINDOW_X, (v) => fix(V(v)), 'The art window’s centre across the frame. FF_TYPE_XPOS in the plugin.'],
  ['windowY', PT.WINDOW_Y, (v) => fix(V(v)), 'The art window’s centre up the frame. FF_TYPE_YPOS in the plugin.'],
  ['windowWidth', PT.WINDOW_WIDTH, (v) => fix(V(v)), 'The art window’s width, as a fraction of the frame’s.'],
  ['windowHeight', PT.WINDOW_HEIGHT, (v) => fix(V(v)), 'The art window’s height, as a fraction of the frame’s.'],
  ['corner', PT.CORNER, (v) => `${fix(CornerFromParam(V(v)), 3)} h`, 'Rounds the window’s corners, up to a quarter of the frame’s height.'],
  ['softness', PT.SOFTNESS, (v) => `${fix(WindowSoftnessFromParam(V(v)), 3)} h · ±${fix(InkSoftnessFromParam(V(v)))}`, 'The foil area’s edge: for the window, an extra feather up to a tenth of the frame’s height; for Bright and Dark Ink, a ramp up to ±0.5 either side of Threshold.'],
  ['threshold', PT.THRESHOLD, (v) => fix(V(v)), 'Where Bright Ink and Dark Ink split, on the print’s luminance (Rec. 709 weights, in linear light, encoded back).'],

  ['mix', PT.MIX, (v) => fix(V(v)), 'Fades the effect in, in linear light. At 0 the clip comes through bit for bit.'],
];

const PARAMS = SPEC.map(([id, slot, display, hint]) => {
  const info = InfoOf(slot);
  if (info.id !== slot) throw new Error(`${id}: not a declared parameter`);
  const p = { id, slot, name: info.name, group: info.group, hint, default: info.defaultValue };
  if (info.kind === Kind.Option) Object.assign(p, { type: 'option', elements: info.options.slice() });
  else Object.assign(p, { type: 'standard', display });
  return p;
});
if (PARAMS.length !== PT.ABOUT_FIRST || PARAMS.some((p, i) => p.slot !== i)) {
  throw new Error('the panel must carry every control, in the plugin’s order');
}

// Presets: whole states, on top of the constructor's defaults. The finishes are
// the README's table, with the numbers tools/figures.py renders them at; the
// last two are the user guide's recipes.
const PRESETS = {
  'Holo (the defaults)': {},
  'Reverse holo': { area: Area.AroundArt },
  'Full foil': { area: Area.WholeCard },
  'Shiny (sparkle)': { area: Area.WholeCard, pattern: Pattern.Sparkle, facet: 0.4, roughness: 0.2 },
  'Cosmos': { pattern: Pattern.Cosmos },
  'Cracked ice': { pattern: Pattern.CrackedIce, facet: 0.4 },
  'Diagonal crosshatch': { pattern: Pattern.Crosshatch, scale: 0.75 },
  'Square crosshatch': { area: Area.WholeCard, pattern: Pattern.Crosshatch, scale: 0.75, angle: 0.25 },
  'Cold foil': { area: Area.BrightInk, groove: 0.15, lampY: 0.5, lampSize: 0.7 },
  'Etched': { area: Area.BrightInk, pattern: Pattern.Sparkle, roughness: 0.4, groove: 0.25, lampY: 0.5, lampSize: 0.7 },
  'The light goes round (a bar)': { motion: Motion.LampOrbit, motionSync: MotionSync.Bar, motionRate: ParamFromMotionRate(1.0) },
  'Spectrum on the beat': { motionSync: MotionSync.Beat, motionRate: ParamFromMotionRate(1.0) },
};

const demo = mountDemo({
  name: 'Holofoil',
  pluginId: 'HF01',
  tagline:
    'Trading-card holographic foil under whatever clip you put through it: a holo window, a reverse holo, full foil or foil keyed by the print, embossed plain, crosshatched, with discs, in shards or as glitter. None of the colour is painted on: each pixel is a diffraction grating under a lamp, and its colour is the grating equation’s answer, through the CIE 1931 matching functions.',
  repo: 'https://github.com/stoatworks-labs/holofoil',
  page: 'https://stoatworks-labs.com/software/holofoil/',
  video: 'https://www.youtube.com/watch?v=Lustt96QYow',

  blurb:
    'It is Holofoil’s own GLSL, ported from the repository to WebGL2 unedited, fed by a JavaScript port of the plugin’s CPU half (every control’s mapping, the eye and the lamp in the card’s frame, the lamp’s spread, the motion and the colour matrix), running on generated clips or your own in this page — no install.',

  needFloat: true,
  showBackdrop: true,

  // The harness's card first: the default window is its art. Then lights on
  // black, where the foil's added light shows best (it adds light, so over a
  // bright clip it has little room); the shape on transparency for Clip Alpha.
  sources: ['card', 'spot', 'scene', 'alpha', 'grid', 'bars', 'ramp'],

  params: PARAMS,
  presets: PRESETS,

  differences: [
    'The CPU half is a PORT, not the plugin’s code: Controls.cpp (every mapping, name, group and default), Card.cpp (the eye and the lamp in the card’s frame, the lamp’s covariance, the motion and its phase, the colour matching fit and the white-balanced XYZ-to-sRGB matrix) and the CPU half of Holofoil.cpp that turns them into each pass’s uniforms — ProcessOpenGL and ensureTable, in double, handed over as floats as the plugin does. What is measured is the picture: demo/tools/crosscheck/ feeds the plugin’s own harness (hftest) and this page the same 60 frames of the harness’s moving card and compares the last. On 2 November 2026, on the Apple M4 Max the plugin was measured on (Chrome 156, ANGLE on Metal), fourteen cases — every Foil Area, Pattern and Motion, Beat sync, a near eye and lamp, a moved and feathered window, Mix, and the groove depths that change the colour table — came out with every pixel within one level of 255 of the plugin’s and at least 99.997% of them identical to the bit, at 640 × 360, 1280 × 720 and 720 × 1008. The page’s 31 controls are the names, kinds and defaults hftest --list prints. The default motion one frame short is caught (a pixel 107 levels off), and so is a held card tipped 0.08° further. A browser on another GPU has not been compared.',
    'The GPU half is not a port: the table and foil passes are the plugin’s own GLSL, joined as the plugin joins them. The colour table is 2048 × 129 and built only when Groove Depth changes, in RGBA32F as the plugin’s where this browser can filter 32-bit float textures (OES_texture_float_linear); where it cannot, in RGBA16F, which rounds each colour to about three significant figures; with that table forced, the crosscheck’s default and sparkle cases still had every pixel within one level of the plugin’s. The line under the picture says which this browser got. EXT_color_buffer_float is needed either way, and without it the page stops and says so. demo/tools/check_shaders.py fails the repository’s verify script if a character of the shaders, or the way they are joined, drifts.',
    'The clock is the page’s: seconds from frame deltas, capped at a tenth of a second a frame by the page (the plugin caps a frame at a quarter). The plugin’s vote on whether its host sends seconds or milliseconds is not ported, because this host’s unit is known. The transport’s Restart is a clock jump backwards, which the plugin takes as no time passing, so the card carries on rocking from where it was. Paused, nothing moves; Step is a sixtieth of a second.',
    'Motion Sync: no host tempo reaches a page, so Beat and Bar run on the plugin’s own 120 BPM in 4/4, which is what it does in a host that sends no beat. In Resolume Motion Rate counts Resolume’s beats and bars, and follows the tempo when it changes.',
    'Window X and Window Y are FF_TYPE_XPOS and FF_TYPE_YPOS, which Resolume shows as positions; here they are sliders. The About block is not on the panel.',
    'The Trading card clip is this page’s GLSL copy of the harness’s own test card (buildCard in tools/hftest), stretched to the composition as the harness stretches it; the default Window sits exactly on its art at any size. The other clips are the kit’s, or your own file, 8-bit as Resolume hands an effect its input. The foil adds light, so it shows best on darker clips: over a bright one there is little room left above it.',
    'The output here is the page’s 8-bit canvas, so light above white clips at white, as it does in an 8-bit composition. The plugin itself hands its host light above 1 unclamped, which a 16-bit composition in Resolume keeps.',
    'The Presets menu is the page’s: the plugin has none, since Resolume saves parameter presets itself. Each preset is a set of the plugin’s own controls on top of its defaults: the finishes from the README’s table, at the numbers its figures are rendered with, and two of the user guide’s recipes.',
    'The plugin’s negative-control hooks (the Hooks uniform its harness flips to prove its checks can fail) are 0 here, as in a shipped instance.',
    'The crosscheck says the page draws what the plugin draws on the machine it was run on; it says nothing about whether either is right. The plugin’s harness, hftest, shows every pixel’s light within 0.18% of the brightest of a wavelength-by-wavelength sum of the grating equation that shares none of the plugin’s shortcuts, its hue the grating’s within a derived bound, the mirror highlight within a thousandth of a pixel of where geometry puts it, a holo and a reverse holo adding up to the clip plus a whole-card foil, and 17 wrong models and 29 one-character mutants caught. The plugin has been loaded in Resolume Arena 7.27.1 on Windows (software rendering), never yet on macOS, and nobody has held it up next to a real foil card. That harness, not this page, is the reason to believe the picture.',
  ],

  createRenderer,
});

// The line under the picture: where Motion has put the card and the lamp, and
// which table this browser got. It reports; it measures nothing.
const stage = document.querySelector('.stage');
if (demo && current && stage) {
  const line = document.createElement('p');
  line.className = 'stage__status';
  line.setAttribute('aria-live', 'off');
  stage.append(line);
  setInterval(() => {
    const { pose, builds, tableFormat } = current.state();
    if (builds === 0) return; // no frame drawn yet
    line.textContent = `The card at Tilt X ${signed(deg(pose.tiltX))}°, Tilt Y ${signed(deg(pose.tiltY))}°, the lamp at ${signed(deg(pose.lampX), 0)}°, ${signed(deg(pose.lampY), 0)}° · the colour table is ${tableFormat}, built ${builds} time${builds === 1 ? '' : 's'}`;
  }, 250);
}
