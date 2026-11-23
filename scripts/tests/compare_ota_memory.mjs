#!/usr/bin/env node
/**
 * Compare OTA memory diagnostics from two ESP32-S3 serial captures.
 * Run: node scripts/tests/compare_ota_memory.mjs before.log after.log
 * Use one paired OTA per log and equivalent Modbus TCP traffic conditions.
 */
import { readFileSync } from "node:fs";

function extract(path) {
  const phases = new Map();
  let count = 0;
  const min = {internal: null, largest: null, psram: null, stack_free: null};
  for (const line of readFileSync(path, "utf8").split(/\r?\n/)) {
    if (!line.includes("[OTA-MEM]")) continue;
    const items = Object.fromEntries(
      [...line.matchAll(/(?:^|\s)([a-z_]+)=([^\s]+)/g)].map(m => [m[1], m[2]])
    );
    if (!items.phase) continue;
    const sample = {};
    for (const key of Object.keys(min)) {
      const value = Number(items[key]);
      if (items[key] !== undefined && Number.isFinite(value)) {
        sample[key] = value;
        min[key] = min[key] === null ? value : Math.min(min[key], value);
      }
    }
    phases.set(items.phase, sample);
    count++;
  }
  if (!count) throw Error("No [OTA-MEM] samples in " + path);
  return {count, min, phases};
}

const [beforePath, afterPath] = process.argv.slice(2);
if (!beforePath || !afterPath) {
  console.error("Usage: node scripts/tests/compare_ota_memory.mjs before.log after.log");
  process.exit(2);
}
const before = extract(beforePath);
const after = extract(afterPath);
const fields = ["internal", "largest", "psram", "stack_free"];
function compare(a, b) {
  return Object.fromEntries(fields.map(k => [
    k, a[k] === undefined || b[k] === undefined || a[k] === null || b[k] === null
      ? null : b[k] - a[k]
  ]));
}
const phaseDeltas = {};
for (const [phase, sample] of after.phases) {
  if (before.phases.has(phase)) phaseDeltas[phase] = compare(before.phases.get(phase), sample);
}
console.log(JSON.stringify({
  units: "bytes (positive delta = more free memory)",
  before_samples: before.count,
  after_samples: after.count,
  min_before: before.min,
  min_after: after.min,
  minimum_deltas: compare(before.min, after.min),
  same_phase_deltas: phaseDeltas,
  warning: "Host log comparison only. A positive delta does not establish causal improvement unless traffic and OTA images are comparable."
}, null, 2));
