#!/usr/bin/env node
'use strict';

// Keep the existing 40px Latin kerning grid when adding Cyrillic. The pinned
// converter otherwise scales every pair to accommodate just sixteen new pairs.
// Their -8.12px adjustment rounds to -130 in FP4; saturating to -128 loses only
// 1/8px, without changing any previously supported pair. No runtime font changes.
const fs = require('fs');
const path = require('path');

async function main() {
  const executable = process.argv[2];
  if (!executable) throw new Error('Usage: ui_font_conv.js /path/to/lv_font_conv [converter options]');
  const entry = fs.realpathSync(executable);
  const root = path.dirname(entry);
  const pkg = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8'));
  if (pkg.name !== 'lv_font_conv' || pkg.version !== '1.5.3') {
    throw new Error('UI font conversion requires lv_font_conv 1.5.3');
  }

  const modulePath = require.resolve(path.join(root, 'lib/font/font'));
  const Font = require(modulePath);
  const allowedLeft = new Set([0x0413, 0x0490]); // Newly supported Г and Ґ.
  const allowedRight = new Set([0x2b, 0x2d, 0x7e, 0xab, 0xac, 0xad, 0xb7, 0xf7]);

  class CompatibleFont extends Font {
    constructor(data, options) {
      super(data, options);
      this.preserveUiKerning = options.size === 40 && options.font.some(
        font => path.basename(font.source_path) === 'Montserrat-Medium.ttf');
      if (!this.preserveUiKerning) return;

      for (const glyph of data.glyphs) {
        for (const [right, adjustment] of Object.entries(glyph.kerning || {})) {
          const fixed = Math.round(adjustment * 16);
          if (!Number.isFinite(adjustment)) throw new Error('Non-finite UI font kerning');
          if (fixed >= -128 && fixed <= 127) continue;
          // Fail closed if a source update would clamp any other pair, change
          // an old glyph pair, or require more than the reviewed 1/8px bound.
          if (!allowedLeft.has(glyph.code) || !allowedRight.has(Number(right)) || fixed !== -130) {
            throw new Error(`Unexpected UI kerning overflow: U+${glyph.code.toString(16)} / U+${Number(right).toString(16)} = ${fixed}`);
          }
        }
      }
      this.kerningScale = 1;
    }

    kernToFP(value) {
      const fixed = super.kernToFP(value);
      return this.preserveUiKerning ? Math.max(-128, Math.min(127, fixed)) : fixed;
    }
  }

  // Install before loading either writer: C and binary outputs must use the
  // identical conversion, including the converter's original class packing.
  require.cache[modulePath].exports = CompatibleFont;
  const args = process.argv.slice(3);
  process.argv = [process.argv[0], entry, ...args];
  await require(path.join(root, 'lib/cli')).run(args);
}

main().catch(error => {
  console.error(error.message);
  process.exitCode = 1;
});
