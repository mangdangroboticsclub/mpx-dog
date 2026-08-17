// Regenerate studio/studio.html.gz from studio/studio.html.
//
// The firmware embeds ONLY the gzipped copy (main/CMakeLists.txt, EMBED_FILES)
// and always answers with Content-Encoding: gzip, exactly like the PWA assets.
// Run this after editing studio.html, then rebuild the firmware:
//
//   node tools/gzip-studio.mjs
//   idf.py build
//
// Node only — no npm install, no dependencies.

import { readFileSync, writeFileSync, statSync } from "node:fs";
import { gzipSync } from "node:zlib";
import { fileURLToPath } from "node:url";
import { join, dirname } from "node:path";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const src = join(root, "studio", "studio.html");
const out = src + ".gz";

const raw = readFileSync(src);
writeFileSync(out, gzipSync(raw, { level: 9 }));

const pct = (100 - (statSync(out).size / raw.length) * 100).toFixed(1);
console.log(
  `studio.html  ${raw.length} B  ->  studio.html.gz  ${statSync(out).size} B  (-${pct}%)`
);
