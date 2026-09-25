/**
 * HAM6's encoder, off the page's main thread.
 *
 * Not the plugin's arrangement: the plugin encodes on its render thread (on
 * up to eight std::threads) and the frame waits for it -- about 20 ms for a
 * 320 x 256 raster in C++. The same encoder ported to JS runs roughly ten
 * times slower, so on the page's one thread every HAM frame would stall the
 * page for a quarter of a second. The page splits each frame's lines across
 * several of these workers instead and shows the last frame they finished
 * (plugin.js says so under the picture).
 *
 * The maths is the page's port, amiga.js, which check_port.sh holds to the
 * plugin's Amiga.cpp. Lines are independent, so how they are split does not
 * change a pixel.
 */
import { HamEncoder, encodeHamLines } from './amiga.js';

const encoder = new HamEncoder();

onmessage = (event) => {
  const { id, index, width, rows, rgba, palette } = event.data;
  const t0 = performance.now();
  const input = new Uint8Array(rgba);
  const output = new Uint8Array(input.length);
  const cost = encodeHamLines(encoder, input, output, width, rows, palette);
  postMessage({ id, index, cost, ms: performance.now() - t0, rgba: output.buffer }, [output.buffer]);
};
