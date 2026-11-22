#!/usr/bin/env node
/**
 * Fast source-level guard for explicit PSRAM allocation in the OTA path.
 * Run: node scripts/tests/check_psram_ota_placement.mjs
 *
 * This does NOT replace a PlatformIO build or a hardware heap measurement.
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const root = new URL("../../", import.meta.url);
const updater = readFileSync(new URL("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp", root), "utf8");
const allocator = readFileSync(new URL("src/Core/SpiRamJsonDocument.h", root), "utf8");
const moduleHeader = readFileSync(new URL("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.h", root), "utf8");
const platformio = readFileSync(new URL("platformio.ini", root), "utf8");

const count = (source, regex) => [...source.matchAll(regex)].length;
assert.match(updater, /#include "Core\/SpiRamJsonDocument\.h"/);
assert.equal(count(updater, /\bSpiRamJsonDocument\s+doc\s*\(/g), 3, "three OTA JSON pools must be in PSRAM");
assert.equal(count(updater, /\bDynamicJsonDocument\s+doc\s*\(/g), 0, "no default JSON pool in OTA");
assert.equal(count(updater, /if\s*\(doc\.capacity\(\)\s*==\s*0U\)/g), 3, "all PSRAM pools must check allocation");
assert.match(allocator, /MALLOC_CAP_SPIRAM\s*\|\s*MALLOC_CAP_8BIT/);
assert.match(allocator, /heap_caps_realloc/);
assert.match(updater, /paired_manifest_json_psram/);
assert.match(updater, /release_manifest_json_psram/);
assert.match(updater, /stage=manifest_check_json/);
// A PSRAM migration must not silently move critical task stacks or change
// global malloc/lwIP/AsyncTCP allocation policy.
assert.match(moduleHeader, /return 10240;/);
assert.match(updater, /\[OTA-MEM\] phase=%s task=%s id=%p core=%d hwm_bytes=%lu/);
assert.match(updater, /\[OTA-HEAP\] phase=%s ms=%lu min_internal=%lu/);
assert.match(updater, /\[OTA-STACK\] LOW phase=/);
assert.match(updater, /paired_manifest_json_released/);
assert.match(platformio, /-D CONFIG_ASYNC_TCP_RUNNING_CORE=1/);
console.log("PASS: three OTA JSON pools use the existing PSRAM allocator, check failures, and preserve critical task settings");
