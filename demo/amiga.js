/**
 * The Amiga's display, as constraints — a hand port of source/Amiga.cpp.
 *
 * This is the plugin's CPU half, and it cannot be copied across the way the
 * shaders are: it is C++, so it is translated, line for line, and what keeps
 * the translation honest is demo/tools/check_port.sh, which compiles the
 * plugin's own Amiga.cpp unchanged and requires this file to agree with it
 * EXACTLY — every HAM6 output colour and every line's total error on random
 * lines and palettes, and every register choosePalette picks on real
 * pictures. Change Amiga.cpp and change this, or that check fails.
 *
 * No DOM and no GL here, so node can import it (the check does) and so can a
 * Web Worker (ham-worker.js, which runs the encoder off the page's thread).
 *
 * Numbers:
 *  - The HAM encoder's costs are integers below 2^31 (a 640-pixel line's worst
 *    error is 416 million; the "infinity" is 2^30 and nothing is added to it
 *    that could reach 2^31), so JS numbers hold them exactly and the tables are
 *    Int32Arrays as the C++'s are int32_t.
 *  - choosePalette is in doubles in both, operation for operation in the same
 *    order. One caveat the check measures rather than hides: Apple clang's
 *    arm64 build contracts some of Amiga.cpp's multiply-adds into fused ones
 *    (-ffp-contract=on is its default and the repo does not turn it off), which
 *    JS cannot do. check_port.sh compares against the C++ built both ways.
 */

//---------------------------------------------------------------------------
// The options, in the order the host shows them (Amiga.cpp's tables).
//---------------------------------------------------------------------------
export const MODE_OCS = 0;
export const MODE_EHB = 1;
export const MODE_HAM6 = 2;
export const MODE_NAMES = ['OCS 32 Colours', 'Extra Half-Brite', 'HAM6'];

export const SCREENS = [
  { name: 'PAL Low Res', width: 320, lines: 256, fieldHz: 50, hires: false },
  { name: 'NTSC Low Res', width: 320, lines: 200, fieldHz: 60, hires: false },
  { name: 'PAL High Res', width: 640, lines: 256, fieldHz: 50, hires: true },
  { name: 'NTSC High Res', width: 640, lines: 200, fieldHz: 60, hires: true },
];

export const PALETTE_PER_FRAME = 0;
export const PALETTE_FIXED = 1;
export const PALETTE_CHOICE_NAMES = ['Per Frame', 'Fixed'];

const clampInt = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

export function screen(id) {
  return SCREENS[clampInt(id, 0, SCREENS.length - 1)];
}

export function effectiveMode(mode, hires) {
  return hires ? MODE_OCS : clampInt(mode, 0, MODE_NAMES.length - 1);
}

export function baseRegisterCount(mode, hires) {
  if (hires) return 16;
  return effectiveMode(mode, hires) === MODE_HAM6 ? 16 : 32;
}

/** floor( t x rate + 1e-6 ), in double, as Amiga.cpp's fieldIndex. */
export function fieldIndex(seconds, fieldHz) {
  return Math.floor(seconds * fieldHz + 1e-6);
}

//---------------------------------------------------------------------------
// Colours are [r, g, b] with 0..15 per gun.
//---------------------------------------------------------------------------
export const index12 = (c) => (c[0] << 8) | (c[1] << 4) | c[2];
export const fromIndex12 = (i) => [(i >> 8) & 15, (i >> 4) & 15, i & 15];
export const to8 = (v4) => v4 * 17;
export const halfBrite = (c) => [c[0] >> 1, c[1] >> 1, c[2] >> 1];

const luma = (c) => 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];

function sortDarkestFirst(p) {
  // Array.prototype.sort is stable, as std::stable_sort is.
  p.sort((a, b) => {
    const la = luma(a);
    const lb = luma(b);
    if (la !== lb) return la < lb ? -1 : 1;
    return index12(a) - index12(b);
  });
  return p;
}

//---------------------------------------------------------------------------
// Palettes.
//---------------------------------------------------------------------------
export function fixedPalette(mode, hires) {
  const p = [];
  const m = effectiveMode(mode, hires);

  if (hires) {
    for (const r of [0, 15]) for (const g of [0, 15]) for (const b of [0, 15]) p.push([r, g, b]);
    p.push([4, 4, 4], [8, 8, 8], [11, 11, 11], [15, 8, 0], [0, 8, 15], [8, 15, 0], [15, 0, 8], [8, 0, 15]);
  } else if (m === MODE_HAM6) {
    for (let v = 0; v < 16; v += 1) p.push([v, v, v]);
  } else {
    for (const r of [0, 8, 15]) for (const g of [0, 8, 15]) for (const b of [0, 8, 15]) p.push([r, g, b]);
    p.push([4, 4, 4], [11, 11, 11], [15, 11, 8], [8, 4, 0], [0, 4, 8]);
  }

  return sortDarkestFirst(p);
}

export function displayPalette(base, mode) {
  const shown = base.map((c) => c.slice());
  if (mode === MODE_EHB) for (const c of base) shown.push(halfBrite(c));
  return shown;
}

const KW = [0.299, 0.587, 0.114];

function dist(ax, ay, az, b, scaleB) {
  // The C++'s lambda, unrolled: d += kW[k] * e * e for k = 0, 1, 2.
  let d = 0.0;
  let e = ax - b[0] * scaleB;
  d += KW[0] * e * e;
  e = ay - b[1] * scaleB;
  d += KW[1] * e * e;
  e = az - b[2] * scaleB;
  d += KW[2] * e * e;
  return d;
}

/**
 * Weighted k-means over the picture's 12-bit histogram (Amiga.cpp's
 * choosePalette). `rgba` is 8-bit RGBA, `pixels` of them; `seeds` is the last
 * frame's registers (an array of [r,g,b]) or [].
 */
export function choosePalette(rgba, pixels, count, halfBriteTwins, seeds) {
  const hist = new Uint32Array(4096);
  for (let i = 0; i < pixels; i += 1) {
    const o = i * 4;
    const r = ((rgba[o] + 8) / 17) | 0;
    const g = ((rgba[o + 1] + 8) / 17) | 0;
    const b = ((rgba[o + 2] + 8) / 17) | 0;
    hist[(r << 8) | (g << 4) | b] += 1;
  }

  const px = [];
  const py = [];
  const pz = [];
  const pw = [];
  for (let i = 0; i < 4096; i += 1) {
    if (hist[i]) {
      px.push((i >> 8) & 15);
      py.push((i >> 4) & 15);
      pz.push(i & 15);
      pw.push(hist[i]);
    }
  }
  const n = pw.length;

  const centre = [];
  if (seeds.length === count) {
    for (let j = 0; j < count; j += 1) centre.push([seeds[j][0], seeds[j][1], seeds[j][2]]);
  } else if (n === 0) {
    for (let j = 0; j < count; j += 1) centre.push([0, 0, 0]);
  } else {
    for (let j = 0; j < count; j += 1) centre.push([0, 0, 0]);
    let first = 0;
    for (let i = 1; i < n; i += 1) if (pw[i] > pw[first]) first = i;
    const nearest = new Float64Array(n).fill(1e30);
    let chosen = 0;
    let pick = first;
    while (chosen < count) {
      centre[chosen] = [px[pick], py[pick], pz[pick]];
      chosen += 1;
      let best = -1.0;
      const c = centre[chosen - 1];
      for (let i = 0; i < n; i += 1) {
        const d = dist(px[i], py[i], pz[i], c, 1.0);
        if (d < nearest[i]) nearest[i] = d; // std::min( nearest, d )
        const score = pw[i] * nearest[i];
        if (score > best) {
          best = score;
          pick = i;
        }
      }
      if (best <= 0.0) {
        // Fewer distinct colours than registers: the rest repeat (copies, as
        // std::array assignment copies).
        for (; chosen < count; chosen += 1) centre[chosen] = centre[chosen % Math.max(1, n)].slice();
        break;
      }
    }
  }

  // Lloyd iterations over the weighted bins.
  for (let iteration = 0; iteration < 10 && n > 0; iteration += 1) {
    const sumA = new Float64Array(count * 3);
    const sumH = new Float64Array(count * 3);
    const nA = new Float64Array(count);
    const nH = new Float64Array(count);
    for (let i = 0; i < n; i += 1) {
      const x = px[i];
      const y = py[i];
      const z = pz[i];
      let bestJ = 0;
      let bestTwin = false;
      let best = 1e30;
      for (let j = 0; j < count; j += 1) {
        const d = dist(x, y, z, centre[j], 1.0);
        if (d < best) {
          best = d;
          bestJ = j;
          bestTwin = false;
        }
        if (halfBriteTwins) {
          const dh = dist(x, y, z, centre[j], 0.5);
          if (dh < best) {
            best = dh;
            bestJ = j;
            bestTwin = true;
          }
        }
      }
      const w = pw[i];
      const sum = bestTwin ? sumH : sumA;
      sum[bestJ * 3 + 0] += w * x;
      sum[bestJ * 3 + 1] += w * y;
      sum[bestJ * 3 + 2] += w * z;
      (bestTwin ? nH : nA)[bestJ] += w;
    }
    for (let j = 0; j < count; j += 1) {
      const den = 4.0 * nA[j] + nH[j];
      if (den <= 0.0) continue;
      for (let k = 0; k < 3; k += 1) {
        const v = (4.0 * sumA[j * 3 + k] + 2.0 * sumH[j * 3 + k]) / den;
        centre[j][k] = v < 0.0 ? 0.0 : v > 15.0 ? 15.0 : v;
      }
    }
  }

  // std::lround: the centres are in 0..15, so half-away-from-zero is
  // Math.round's half-up.
  const palette = centre.map((c) => [
    clampInt(Math.round(c[0]), 0, 15),
    clampInt(Math.round(c[1]), 0, 15),
    clampInt(Math.round(c[2]), 0, 15),
  ]);
  return sortDarkestFirst(palette);
}

//---------------------------------------------------------------------------
// Hold-and-modify: HamEncoder::encodeLine, exactly.
//---------------------------------------------------------------------------
export const WEIGHT_R = 3;
export const WEIGHT_G = 6;
export const WEIGHT_B = 1;

const INF = 1 << 30;
const AR = 0; // [ g * 16 + b ]  min over red
const AG = 256; // [ r * 16 + b ]  min over green
const AB = 512; // [ r * 16 + g ]  min over blue
const M = 768; // min over everything
const STRIDE = 772;

export function pixelError(r, g, b, rgb, o) {
  const dr = r * 17 - rgb[o];
  const dg = g * 17 - rgb[o + 1];
  const db = b * 17 - rgb[o + 2];
  return WEIGHT_R * dr * dr + WEIGHT_G * dg * dg + WEIGHT_B * db * db;
}

export class HamEncoder {
  constructor() {
    this.tables = new Int32Array(0);
    this.inPalette = new Uint8Array(4096);
    this.er = new Int32Array(16);
    this.eg = new Int32Array(16);
    this.eb = new Int32Array(16);
    this.XR = new Int32Array(16);
    this.YR = new Int32Array(16);
    this.XG = new Int32Array(16);
    this.YG = new Int32Array(16);
    this.XB = new Int32Array(16);
    this.YB = new Int32Array(16);
  }

  /**
   * `rgb` holds 8-bit pixels starting at `offset` with `step` bytes between
   * them (3 for packed RGB, 4 to read an RGBA row in place); `palette` is an
   * array of [r,g,b]; `out` is a Uint8Array receiving width x 3 guns (0..15).
   * Returns the line's total error. The hardware's one gun per modify only:
   * the harness's two-gun negative control is not ported.
   */
  encodeLine(rgb, offset, step, width, palette, out) {
    if (width <= 0) return 0;

    const start = palette.length ? palette[0] : [0, 0, 0];
    const inPalette = this.inPalette;
    inPalette.fill(0);
    for (const p of palette) inPalette[index12(p)] = 1;

    const need = (width + 1) * STRIDE;
    if (this.tables.length < need) this.tables = new Int32Array(need);
    const T = this.tables;
    T.fill(INF, 0, STRIDE);

    T[AR + start[1] * 16 + start[2]] = 0;
    T[AG + start[0] * 16 + start[2]] = 0;
    T[AB + start[0] * 16 + start[1]] = 0;
    T[M] = 0;

    const { er, eg, eb, XR, YR, XG, YG, XB, YB } = this;

    //--- Forward ------------------------------------------------------------
    for (let x = 0; x < width; x += 1) {
      const P = x * STRIDE;
      const N = P + STRIDE;
      const Mp = T[P + M];

      const o = offset + x * step;
      const tr = rgb[o];
      const tg = rgb[o + 1];
      const tb = rgb[o + 2];
      let mr = INF;
      let mg = INF;
      let mb = INF;
      for (let v = 0; v < 16; v += 1) {
        const dr = v * 17 - tr;
        const dg = v * 17 - tg;
        const db = v * 17 - tb;
        const a = WEIGHT_R * dr * dr;
        const b = WEIGHT_G * dg * dg;
        const c = WEIGHT_B * db * db;
        er[v] = a;
        eg[v] = b;
        eb[v] = c;
        if (a < mr) mr = a;
        if (b < mg) mg = b;
        if (c < mb) mb = c;
      }

      for (let k = 0; k < 16; k += 1) {
        XR[k] = INF;
        YR[k] = INF;
        XG[k] = INF;
      }
      for (let r = 0; r < 16; r += 1) {
        const e = er[r];
        const ag = P + AG + r * 16;
        const ab = P + AB + r * 16;
        let yb = INF;
        let yg = INF;
        for (let k = 0; k < 16; k += 1) {
          const agk = T[ag + k];
          const abk = T[ab + k];
          let t = e + agk;
          if (t < XR[k]) XR[k] = t;
          t = e + abk;
          if (t < YR[k]) YR[k] = t;
          t = eb[k] + agk;
          if (t < yb) yb = t;
          t = eg[k] + abk;
          if (t < yg) yg = t;
        }
        YB[r] = yb;
        YG[r] = yg;
      }
      for (let g = 0; g < 16; g += 1) {
        const e = eg[g];
        const ar = P + AR + g * 16;
        let xb = INF;
        for (let k = 0; k < 16; k += 1) {
          const ark = T[ar + k];
          let t = e + ark;
          if (t < XG[k]) XG[k] = t;
          t = eb[k] + ark;
          if (t < xb) xb = t;
        }
        XB[g] = xb;
      }

      for (let i = 0; i < 16; i += 1) {
        const ar = P + AR + i * 16;
        const ag = P + AG + i * 16;
        const ab = P + AB + i * 16;
        const nar = N + AR + i * 16;
        const nag = N + AG + i * 16;
        const nab = N + AB + i * 16;
        const egi = eg[i];
        const eri = er[i];
        const yr = YR[i];
        const yg = YG[i];
        const yb = YB[i];
        for (let j = 0; j < 16; j += 1) {
          let t = mr + T[ar + j];
          if (XR[j] < t) t = XR[j];
          if (yr < t) t = yr;
          t += egi + eb[j];
          T[nar + j] = t < INF ? t : INF;

          t = mg + T[ag + j];
          if (XG[j] < t) t = XG[j];
          if (yg < t) t = yg;
          t += eri + eb[j];
          T[nag + j] = t < INF ? t : INF;

          t = mb + T[ab + j];
          if (XB[j] < t) t = XB[j];
          if (yb < t) t = yb;
          t += eri + eg[j];
          T[nab + j] = t < INF ? t : INF;
        }
      }

      // A palette register can follow anything.
      for (const p of palette) {
        let v = er[p[0]] + eg[p[1]] + eb[p[2]] + Mp;
        if (v > INF) v = INF;
        let q = N + AR + p[1] * 16 + p[2];
        if (v < T[q]) T[q] = v;
        q = N + AG + p[0] * 16 + p[2];
        if (v < T[q]) T[q] = v;
        q = N + AB + p[0] * 16 + p[1];
        if (v < T[q]) T[q] = v;
      }

      let m = INF;
      for (let i = 0; i < 256; i += 1) if (T[N + AR + i] < m) m = T[N + AR + i];
      T[N + M] = m;
    }

    //--- Backward -----------------------------------------------------------
    const costAt = (x, r, g, b) => {
      const P = x * STRIDE;
      let t = T[P + AR + g * 16 + b];
      if (T[P + AG + r * 16 + b] < t) t = T[P + AG + r * 16 + b];
      if (T[P + AB + r * 16 + g] < t) t = T[P + AB + r * 16 + g];
      if (inPalette[(r << 8) | (g << 4) | b] && T[P + M] < t) t = T[P + M];
      const v = pixelError(r, g, b, rgb, offset + x * step) + t;
      return v < INF ? v : INF;
    };
    const errAt = (x, c) => pixelError(c[0], c[1], c[2], rgb, offset + x * step);

    const last = width * STRIDE;
    const best = T[last + M];
    let c = [0, 0, 0];
    for (let i = 0; i < 256; i += 1) {
      if (T[last + AR + i] === best) {
        c = [0, i >> 4, i & 15];
        break;
      }
    }
    for (let r = 0; r < 16; r += 1) {
      if (costAt(width - 1, r, c[1], c[2]) === best) {
        c[0] = r;
        break;
      }
    }
    out[(width - 1) * 3] = c[0];
    out[(width - 1) * 3 + 1] = c[1];
    out[(width - 1) * 3 + 2] = c[2];

    for (let x = width - 1; x >= 1; x -= 1) {
      const P = x * STRIDE;
      const arrive = costAt(x, c[0], c[1], c[2]) - errAt(x, c);

      let prev = c;
      let found = false;
      const findGun = (gun, want) => {
        for (let v = 0; v < 16 && !found; v += 1) {
          const k = prev.slice();
          k[gun] = v;
          if (costAt(x - 1, k[0], k[1], k[2]) === want) {
            prev = k;
            found = true;
          }
        }
      };

      if (inPalette[index12(c)] && arrive === T[P + M]) {
        for (let i = 0; i < 256 && !found; i += 1) {
          if (T[P + AR + i] === T[P + M]) {
            prev = [0, i >> 4, i & 15];
            findGun(0, T[P + M]);
          }
        }
      }
      if (!found && arrive === T[P + AR + c[1] * 16 + c[2]]) {
        prev = c;
        findGun(0, arrive);
      }
      if (!found && arrive === T[P + AG + c[0] * 16 + c[2]]) {
        prev = c;
        findGun(1, arrive);
      }
      if (!found && arrive === T[P + AB + c[0] * 16 + c[1]]) {
        prev = c;
        findGun(2, arrive);
      }

      c = prev;
      out[(x - 1) * 3] = c[0];
      out[(x - 1) * 3 + 1] = c[1];
      out[(x - 1) * 3 + 2] = c[2];
    }

    return best;
  }
}

/**
 * encodeHamFrame for the lines `first, first + stride, ...` of an RGBA8 frame:
 * reads `rgbaIn`, writes the encoded RGBA8 (alpha passed through) into
 * `rgbaOut`. Lines are independent, so any split across workers gives the
 * same picture. Returns the summed error.
 */
export function encodeHamLines(encoder, rgbaIn, rgbaOut, width, height, palette, first = 0, stride = 1) {
  const line = new Uint8Array(width * 3);
  let total = 0;
  for (let y = first; y < height; y += stride) {
    const row = y * width * 4;
    total += encoder.encodeLine(rgbaIn, row, 4, width, palette, line);
    for (let x = 0; x < width; x += 1) {
      rgbaOut[row + x * 4] = line[x * 3] * 17;
      rgbaOut[row + x * 4 + 1] = line[x * 3 + 1] * 17;
      rgbaOut[row + x * 4 + 2] = line[x * 3 + 2] * 17;
      rgbaOut[row + x * 4 + 3] = rgbaIn[row + x * 4 + 3];
    }
  }
  return total;
}
