/**
 * The page's Amiga (demo/amiga.js) against the plugin's own source/Amiga.cpp.
 *
 *     demo/tools/check_port.sh          (finds node and c++, runs this)
 *     node demo/tools/check_port.mjs [--quick]
 *
 * ------------------------------------------------------------ the reference
 *
 * refamiga.cpp is compiled against source/Amiga.cpp UNCHANGED, twice:
 *
 *   as-built   -std=c++17 -O3 -DNDEBUG -ffp-contract=off for this machine's
 *              arch: CMake's Release flags, and the no-contraction flag
 *              CMakeLists.txt sets on Amiga.cpp (since v1.1.0's port check
 *              found the fused arm64 build ordering equal-luma registers
 *              differently). Every double rounded after every operation, as
 *              JS does and as the x86_64 and MSVC builds do. Strict.
 *   fused      the same with -ffp-contract=on, Apple clang's arm64 default:
 *              what the plugin's arm64 slice computed before the flag.
 *              Reported, never failed -- it is the contrast that shows the
 *              flag matters.
 *
 * ------------------------------------------------------------ what it compares
 *
 *   ham      HamEncoder::encodeLine on random lines and palettes: every output
 *            colour of every pixel and the line's total error, exactly. Lines
 *            of 1..48 pixels and full 320- and 640-pixel lines; noise, ramps,
 *            hard edges, and lines already on the 12-bit grid (where ties make
 *            the backtrack's search order matter); palettes of 1..16 registers,
 *            with duplicates. Against BOTH builds: the encoder is integers.
 *   palette  choosePalette on generated 320x256 pictures (ramps, bars, noise,
 *            blobs, a picture with fewer colours than registers, flat, empty,
 *            and two made of colour pairs whose luma is mathematically equal,
 *            so the darkest-first sort's order rests on rounding and the
 *            tie-break),
 *            16 and 32 registers, EHB's twins on and off, cold and seeded, and
 *            a moving picture whose registers are carried frame to frame as
 *            the plugin carries them: every register, exactly, against the
 *            unfused build; and against the as-built one, reported but not
 *            failed. On arm64 the as-built C++ orders equal-luma registers
 *            differently from the unfused one on the tie pictures: that is the
 *            plugin's arm64 slice disagreeing with its own x86_64 slice, not
 *            the port disagreeing with the plugin, and JS cannot fuse.
 *   tables   fixedPalette, displayPalette, baseRegisterCount and effectiveMode
 *            for every mode (and one either side) on both screen kinds.
 *   fields   fieldIndex on 20,000 clock values at 50 and 60 Hz, including the
 *            k / rate values the 1e-6 exists for and ~499 million seconds.
 *
 * ------------------------------------------------------------ what it cannot
 *
 * refamiga is not the plugin binary and has no GL: that the page reads back
 * the same pixels, uploads the result to the same place, and sets the same
 * uniforms as ProcessOpenGL, only a reader checks (plugin.js). The pictures
 * are this script's, not clips through the downres shader. The two-gun
 * negative control is the harness's and is not ported. Agreement on these
 * cases is evidence about the port, not a proof of it.
 */
import { execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { tmpdir, arch } from 'node:os';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, '..', '..');
const A = await import(join(REPO, 'demo', 'amiga.js'));
const quick = process.argv.includes('--quick');

//---------------------------------------------------------------------------
// A deterministic PRNG (mulberry32), so a failure reproduces.
//---------------------------------------------------------------------------
let seed = 0x5eed1234;
function rand() {
  seed = (seed + 0x6d2b79f5) | 0;
  let t = seed;
  t = Math.imul(t ^ (t >>> 15), t | 1);
  t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
}
const ri = (n) => Math.floor(rand() * n);
const clamp8 = (v) => (v < 0 ? 0 : v > 255 ? 255 : Math.round(v));

//---------------------------------------------------------------------------
// The case stream.
//---------------------------------------------------------------------------
const chunks = [];
const i32 = (v) => { const b = Buffer.alloc(4); b.writeInt32LE(v); chunks.push(b); };
const f64 = (v) => { const b = Buffer.alloc(8); b.writeDoubleLE(v); chunks.push(b); };
const tag = (c) => chunks.push(Buffer.from(c, 'latin1'));
const bytes = (a) => chunks.push(Buffer.from(a));
const colourBytes = (p) => bytes(p.flat());

const expect = []; // what the JS computed, in record order

//--- ham ------------------------------------------------------------------
function randomPalette() {
  const n = 1 + ri(16);
  const p = [];
  for (let i = 0; i < n; i += 1) {
    if (i > 0 && rand() < 0.1) p.push(p[ri(i)].slice());
    else if (rand() < 0.3) { const v = ri(16); p.push([v, v, v]); } else p.push([ri(16), ri(16), ri(16)]);
  }
  return p;
}
function randomLine(width) {
  const rgb = new Uint8Array(width * 3);
  const kind = ri(5);
  const a = [ri(256), ri(256), ri(256)];
  const b = [ri(256), ri(256), ri(256)];
  const edge = ri(width + 1);
  for (let x = 0; x < width; x += 1) {
    for (let k = 0; k < 3; k += 1) {
      let v;
      if (kind === 0) v = ri(256); // noise
      else if (kind === 1) v = a[k] + ((b[k] - a[k]) * x) / Math.max(1, width - 1); // ramp
      else if (kind === 2) v = x < edge ? a[k] : b[k]; // hard edge
      else if (kind === 3) v = ri(16) * 17; // on the 12-bit grid: ties
      else v = (x < edge ? a[k] : b[k]) + (rand() - 0.5) * 24; // edge + dither-ish noise
      rgb[x * 3 + k] = clamp8(v);
    }
  }
  return rgb;
}
const encoder = new A.HamEncoder();
let hamCases = 0;
let hamPixels = 0;
function hamCase(width) {
  const palette = randomPalette();
  const rgb = randomLine(width);
  const out = new Uint8Array(width * 3);
  const cost = encoder.encodeLine(rgb, 0, 3, width, palette, out);
  tag('H'); i32(width); i32(palette.length); colourBytes(palette); bytes(rgb);
  expect.push({ kind: 'ham', width, cost, out, palette });
  hamCases += 1;
  hamPixels += width;
}
const nShort = quick ? 500 : 4000;
for (let i = 0; i < nShort; i += 1) hamCase(1 + ri(48));
for (let i = 0; i < (quick ? 20 : 150); i += 1) hamCase(320);
for (let i = 0; i < (quick ? 5 : 40); i += 1) hamCase(640);

//--- palette --------------------------------------------------------------
const W = 320;
const H = 256;
function picture(kind, t = 0) {
  const rgba = new Uint8Array(W * H * 4);
  const blobs = [];
  for (let i = 0; i < 7; i += 1) blobs.push({ x: rand() * W, y: rand() * H, r: 20 + rand() * 60, c: [rand() * 255, rand() * 255, rand() * 255] });
  for (let y = 0; y < H; y += 1) {
    for (let x = 0; x < W; x += 1) {
      const o = (y * W + x) * 4;
      let c;
      if (kind === 'ramp') {
        const h = (x / W) * 6;
        const v = 1 - y / H;
        const f = (k) => Math.max(0, Math.min(1, Math.abs(((h + k) % 6) - 3) - 1));
        c = [f(0) * v * 255, f(4) * v * 255, f(2) * v * 255];
      } else if (kind === 'bars') {
        const bar = Math.floor((x / W) * 7);
        const lv = y < H * 0.7 ? 191 : 255;
        c = [[1, 1, 1], [1, 1, 0], [0, 1, 1], [0, 1, 0], [1, 0, 1], [1, 0, 0], [0, 0, 1]][bar].map((k) => k * lv);
      } else if (kind === 'noise') {
        c = [ri(256), ri(256), ri(256)];
      } else if (kind === 'blobs' || kind === 'moving') {
        c = [20, 30, 40];
        for (const b of blobs) {
          const bx = kind === 'moving' ? b.x + t * 9 : b.x;
          const d2 = ((x - bx) ** 2 + (y - b.y) ** 2) / (b.r * b.r);
          const w = Math.exp(-d2);
          c = c.map((v, k) => v * (1 - w) + b.c[k] * w);
        }
        c = c.map((v) => v + (rand() - 0.5) * 6);
      } else if (kind.startsWith('ties')) {
        // Pairs whose luma is mathematically equal -- (15, g, b + 7) and
        // (0, g + 9, b), since 299 x 15 + 114 x 7 = 587 x 9 -- so which comes
        // first is decided by the doubles' rounding, then by the tie-break.
        const pairs = [];
        for (let g = 0; g <= 6; g += 1) for (let b = 0; b <= 8; b += 1) pairs.push([[15, g, b + 7], [0, g + 9, b]]);
        const n = kind === 'ties16' ? 8 : 16;
        const start = kind === 'ties16' ? 3 : 20;
        const cell = ((y >> 5) * 10 + (x >> 5)) % (2 * n);
        c = pairs[(start + (cell >> 1) * 3) % pairs.length][cell & 1].map((v) => v * 17);
      } else if (kind === 'few') {
        c = [[0, 0, 0], [255, 255, 255], [200, 30, 30], [30, 200, 30], [30, 30, 200]][(x >> 6) % 5];
      } else {
        c = [0, 0, 0]; // flat
      }
      rgba[o] = clamp8(c[0]);
      rgba[o + 1] = clamp8(c[1]);
      rgba[o + 2] = clamp8(c[2]);
      rgba[o + 3] = 255;
    }
  }
  return rgba;
}
let paletteCases = 0;
function paletteCase(rgba, pixels, count, twins, seeds, label) {
  const p = A.choosePalette(rgba, pixels, count, twins, seeds);
  tag('P'); i32(pixels); i32(count); i32(twins ? 1 : 0); i32(seeds.length); colourBytes(seeds); bytes(rgba.subarray(0, pixels * 4));
  expect.push({ kind: 'palette', count, p, label });
  paletteCases += 1;
  return p;
}
// Seeded is always seeded from what the JS chose, as the page does; if the two
// agree on the cold call they are handed the same seeds.
const configs = [[16, false], [32, false], [32, true]];
for (const kind of ['ramp', 'bars', 'noise', 'blobs', 'few', 'flat']) {
  const rgba = picture(kind);
  for (const [count, twins] of configs) {
    const cold = paletteCase(rgba, W * H, count, twins, [], `${kind} ${count}${twins ? ' EHB' : ''} cold`);
    paletteCase(rgba, W * H, count, twins, cold, `${kind} ${count}${twins ? ' EHB' : ''} seeded`);
  }
}
for (const [count, twins] of configs) paletteCase(new Uint8Array(0), 0, count, twins, [], `empty ${count}`);
for (const kind of ['ties16', 'ties32']) {
  const rgba = picture(kind);
  for (const [count, twins] of configs) paletteCase(rgba, W * H, count, twins, [], `${kind} ${count}${twins ? ' EHB' : ''} cold`);
}
{
  const frames = quick ? 3 : 8;
  const saved = seed;
  for (const [count, twins] of configs) {
    seed = saved; // the same moving picture for each configuration
    let regs = [];
    for (let f = 0; f < frames; f += 1) {
      const s = seed;
      const rgba = picture('moving', f);
      seed = s;
      regs = paletteCase(rgba, W * H, count, twins, regs, `moving ${count}${twins ? ' EHB' : ''} frame ${f}`);
    }
  }
}

//--- tables ---------------------------------------------------------------
tag('F');
{
  const t = [];
  for (let hires = 0; hires < 2; hires += 1) {
    for (let mode = -1; mode <= A.MODE_NAMES.length; mode += 1) {
      const e = A.effectiveMode(mode, hires === 1);
      const base = A.fixedPalette(mode, hires === 1);
      t.push({ e, n: A.baseRegisterCount(mode, hires === 1), base, shown: A.displayPalette(base, e) });
    }
  }
  expect.push({ kind: 'tables', t });
}

//--- fields ---------------------------------------------------------------
{
  const s = [];
  const hz = [];
  for (let i = 0; i < 20000; i += 1) {
    const rate = i % 2 ? 50 : 60;
    let v;
    const pick = i % 4;
    if (pick === 0) v = ri(1000000) / rate; // k / rate exactly
    else if (pick === 1) v = rand() * 100000;
    else if (pick === 2) v = 499000000 + rand() * 1000; // Resolume's measured clock
    else v = ri(60000) / 1000; // host milliseconds
    s.push(v);
    hz.push(rate);
  }
  tag('T'); i32(s.length); s.forEach(f64); hz.forEach(i32);
  expect.push({ kind: 'fields', k: s.map((v, i) => A.fieldIndex(v, hz[i])) });
}

//---------------------------------------------------------------------------
// Build the reference both ways and run it.
//---------------------------------------------------------------------------
const dir = mkdtempSync(join(tmpdir(), 'nesolume-port-'));
const cases = join(dir, 'cases.bin');
writeFileSync(cases, Buffer.concat(chunks));
const cxx = process.env.CXX || 'c++';
const archFlag = arch() === 'arm64' ? ['-arch', 'arm64'] : [];
const builds = [
  { name: 'as-built', flags: ['-ffp-contract=off'] },
  { name: 'fused', flags: ['-ffp-contract=on'] },
];
const outputs = {};
try {
  for (const b of builds) {
    const exe = join(dir, `refamiga-${b.name}`);
    execFileSync(cxx, ['-std=c++17', '-O3', '-DNDEBUG', ...archFlag, ...b.flags, '-I', join(REPO, 'source'),
      join(HERE, 'refamiga.cpp'), join(REPO, 'source', 'Amiga.cpp'), '-o', exe], { stdio: 'inherit' });
    const out = join(dir, `out-${b.name}.bin`);
    execFileSync(exe, [cases, out], { stdio: 'inherit' });
    outputs[b.name] = readFileSync(out);
  }
} finally {
  rmSync(dir, { recursive: true, force: true });
}

//---------------------------------------------------------------------------
// Compare.
//---------------------------------------------------------------------------
function compare(buf, name) {
  let at = 0;
  const r = { hamBad: 0, paletteBad: [], orderOnly: 0, tablesBad: 0, fieldsBad: 0, firstHam: null };
  const readColours = (n) => { const a = []; for (let i = 0; i < n; i += 1) { a.push([buf[at], buf[at + 1], buf[at + 2]]); at += 3; } return a; };
  const same = (x, y) => x.length === y.length && x.every((c, i) => c[0] === y[i][0] && c[1] === y[i][1] && c[2] === y[i][2]);
  for (const e of expect) {
    if (e.kind === 'ham') {
      const cost = Number(buf.readBigInt64LE(at)); at += 8;
      let ok = cost === e.cost;
      for (let i = 0; i < e.width * 3; i += 1) if (buf[at + i] !== e.out[i]) ok = false;
      at += e.width * 3;
      if (!ok) { r.hamBad += 1; if (!r.firstHam) r.firstHam = `width ${e.width}, C++ cost ${cost}, JS ${e.cost}`; }
    } else if (e.kind === 'palette') {
      const p = readColours(e.count);
      if (!same(p, e.p)) {
        r.paletteBad.push(e.label);
        const key = (a) => a.map((c) => c.join(',')).sort().join(' ');
        if (key(p) === key(e.p)) r.orderOnly += 1;
      }
    } else if (e.kind === 'tables') {
      for (const t of e.t) {
        const eff = buf.readInt32LE(at); const n = buf.readInt32LE(at + 4); at += 8;
        const bs = buf.readInt32LE(at); at += 4; const base = readColours(bs);
        const ss = buf.readInt32LE(at); at += 4; const shown = readColours(ss);
        if (eff !== t.e || n !== t.n || !same(base, t.base) || !same(shown, t.shown)) r.tablesBad += 1;
      }
    } else if (e.kind === 'fields') {
      for (const k of e.k) { if (Number(buf.readBigInt64LE(at)) !== k) r.fieldsBad += 1; at += 8; }
    }
  }
  if (at !== buf.length) throw new Error(`${name}: read ${at} of ${buf.length} bytes -- the record streams disagree`);
  return r;
}

let failed = false;
const results = {};
for (const b of builds) {
  const r = compare(outputs[b.name], b.name);
  results[b.name] = r;
  console.log(`${b.name.padEnd(9)} ham: ${hamCases - r.hamBad}/${hamCases} lines (${hamPixels} pixels) identical` +
    `${r.firstHam ? ` -- first difference: ${r.firstHam}` : ''}`);
  console.log(`${' '.repeat(9)} palette: ${paletteCases - r.paletteBad.length}/${paletteCases} choosePalette calls identical` +
    `${r.paletteBad.length ? ` -- differ: ${r.paletteBad.join('; ')}` : ''}`);
  console.log(`${' '.repeat(9)} tables: ${r.tablesBad ? `${r.tablesBad} differ` : 'identical'};  fieldIndex: ${r.fieldsBad ? `${r.fieldsBad} of 20000 differ` : '20000/20000 identical'}`);
  if (r.hamBad || r.tablesBad) failed = true;
  if (b.name === 'as-built' && (r.paletteBad.length || r.fieldsBad)) failed = true;
}
const fused = results['fused'];
if (fused.paletteBad.length || fused.fieldsBad) {
  // Not a port error: a fused build of the same source disagrees with every
  // unfused one. The pairs of equal-luma colours make the darkest-first sort's
  // order rest on the last bit of a double, and a fused multiply-add rounds
  // differently. The plugin is built unfused so that this cannot happen.
  console.log('note: the fused (-ffp-contract=on) build differs as shown; the plugin is built with -ffp-contract=off');
}
if (failed) {
  console.log('FAIL: demo/amiga.js no longer agrees with source/Amiga.cpp');
  process.exit(1);
}
const fusedNote = fused.paletteBad.length
  ? `; a fused ${arch()} build would differ on ${fused.paletteBad.length} palettes` +
    ` (${fused.orderOnly === fused.paletteBad.length ? 'the same registers in another order' : `${fused.paletteBad.length - fused.orderOnly} with different registers`}), which is why it is not built that way`
  : `; a fused ${arch()} build agrees too`;
console.log(`port agrees: ${hamCases} HAM lines and ${paletteCases} palettes exact against Amiga.cpp as built (-ffp-contract=off)${fusedNote}`);
