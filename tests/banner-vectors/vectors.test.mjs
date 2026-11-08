import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { webcrypto } from 'node:crypto';

const vectorsDir = process.env.VECTORS_DIR
  ? path.resolve(process.env.VECTORS_DIR)
  : path.dirname(fileURLToPath(import.meta.url));
const casesPath = path.join(vectorsDir, 'cases.json');
const transitionsPath = path.join(vectorsDir, 'transitions.json');
const encoder = new TextEncoder();
const LF = 0x0a;
const CR = 0x0d;
const domainPrefix = encoder.encode('GOATCAM-BANNER-V1');
const concat = (...parts) => {
  const result = new Uint8Array(parts.reduce((total, part) => total + part.length, 0));
  let offset = 0;
  for (const part of parts) {
    result.set(part, offset);
    offset += part.length;
  }
  return result;
};
const domainSeparated = (jsonBytes) => concat(domainPrefix, Uint8Array.of(LF), jsonBytes);

const expectedCases = {
  valid: ['show', null],
  'valid-killswitch': ['killswitch', null],
  'unknown-key': ['show', null],
  'max-text': ['show', null],
  'sig-bad': ['reject', 'sig'],
  'sig-update-style': ['reject', 'sig'],
  'sigline-long': ['reject', 'sigline'],
  'sigline-missing': ['reject', 'sigline'],
  'size-over': ['reject', 'size'],
  'utf8-bad': ['reject', 'utf8'],
  'json-trailing': ['reject', 'json'],
  'json-dupkey': ['reject', 'dupkey'],
  'json-depth4': ['reject', 'depth'],
  'schema-2': ['reject', 'schema'],
  'keyid-wrong': ['reject', 'keyid'],
  'field-missing-headline': ['reject', 'field'],
  'sha-upper': ['reject', 'field'],
  'text-headline-49': ['reject', 'text'],
  'text-bidi': ['reject', 'text'],
  'text-zero-width': ['reject', 'text'],
  'url-http': ['reject', 'url'],
  'url-userinfo': ['reject', 'url'],
  'url-ip': ['reject', 'url'],
  'url-port': ['reject', 'url'],
  'qr-size-33': ['reject', 'qr'],
  'qr-size-huge': ['reject', 'qr'],
  'qr-row-short': ['reject', 'qr'],
  'time-expired': ['reject', 'time'],
  'time-issued-future': ['reject', 'time'],
  'time-window-401d': ['reject', 'time'],
  'time-expires-before-issued': ['reject', 'time'],
  'time-expires-equals-issued': ['reject', 'time'],
  'clock-future': ['reject', 'time'],
  'clock-past': ['reject', 'time'],
  'revision-below-hwm': ['reject', 'revision'],
  'revision-below-floor': ['reject', 'floor'],
  'revision-equal-different': ['reject', 'equal'],
  'revision-equal-same': ['show', null],
  'revision-equal-nostored': ['show', null],
  'photo-ok-webp': ['show', null],
  'photo-ok-jpg': ['show', null],
  'photo-hash': ['show', null],
  'photo-dims': ['show', null],
  'photo-anim': ['show', null],
  'photo-size': ['show', null],
  'json-whitespace': ['show', null],
  'json-pretty': ['show', null],
  'killswitch-spaced': ['killswitch', null],
  'schema-1.0': ['reject', 'schema'],
  'schema-1e0': ['reject', 'schema'],
  'depth4-then-trailing': ['reject', 'json'],
  'depth4-then-dupkey': ['reject', 'dupkey'],
  'dupkey-then-depth33': ['reject', 'depth'],
  'depth32-malformed': ['reject', 'depth'],
  'toplevel-array': ['reject', 'json'],
  'size-exact-16384': ['show', null],
  'qr-size-negative': ['reject', 'qr'],
  'qr-row-number': ['reject', 'field'],
  'hwm-negative': ['reject', 'revision'],
  'floor-negative': ['reject', 'floor'],
  'text-del': ['reject', 'text'],
  'text-soft-hyphen': ['reject', 'text'],
  'text-u0250': ['reject', 'text'],
  'text-u2010': ['reject', 'text'],
  'em-not-substring': ['reject', 'text'],
  'tagline-19': ['reject', 'text'],
  'badge-17': ['reject', 'text'],
  'label-81': ['reject', 'text'],
  'url-label-hyphen': ['reject', 'url'],
  'url-label-64': ['reject', 'url'],
  'url-tld-1': ['reject', 'url'],
  'url-tld-digits': ['reject', 'url'],
  'url-idn-tld': ['show', null],
  'url-port-443': ['show', null],
  'url-201': ['reject', 'url'],
  'url-non-ascii': ['reject', 'url'],
  'textbox-sum': ['reject', 'field'],
  'w-1601': ['reject', 'field'],
  'revision-2p53': ['reject', 'field'],
  'date-feb29-2027': ['reject', 'field'],
  'hour-24': ['reject', 'field'],
  'year-1999': ['reject', 'field'],
  'base64-noncanonical': ['reject', 'sigline'],
  'photo-webp-nopad-end': ['show', null],
  'photo-webp-trailing': ['show', null],
  'photo-vp8x-animflag': ['show', null],
  'photo-anim-only-anim': ['show', null],
  'photo-anim-only-anmf': ['show', null],
  'photo-vp8l': ['show', null],
  'photo-jpeg-fill': ['show', null],
  'photo-jpeg-sof-at-eof': ['show', null],
  'photo-empty': ['show', null],
};

const photoReasons = new Set(['ok', 'photo-size', 'photo-hash', 'photo-dims', 'photo-anim']);
const reasonCodes = new Set([
  'size', 'sigline', 'sig', 'utf8', 'json', 'dupkey', 'depth', 'schema', 'keyid',
  'field', 'text', 'url', 'qr', 'time', 'revision', 'floor', 'equal', ...photoReasons,
]);
const eventTypes = new Set([
  'START', 'SHOWN', 'HIDDEN', 'SIGNED', 'PHOTOS', 'COMMITTED', 'FAILED',
  'TIMER', 'RESUME', 'CLICK', 'POINTER', 'FOCUS',
]);
const effectTypes = new Set([
  'fetch-signed', 'fetch-photos', 'cancel', 'write-set', 'write-record',
  'delete-unnamed-sets', 'display', 'arm-timer', 'disarm-timer', 'open-link',
]);
const requiredScenarios = [
  'cold-start-show', 'shown-twice', 'hidden-mid-attempt', 'late-reply-after-hide',
  'killswitch-under-pointer', 'fresh-under-pointer-waits', 'expiry-while-visible',
  'visible-24h-check', 'backoff-6-12-24', 'crash-before-record', 'hwm-below-floor',
  'click-expired', 'resume-after-expiry', 'start-clock-ahead', 'mayfetch-false',
  'expiry-30s-exact', 'photo-after-expiry', 'storage-write-fails',
  'clock-behind-last-check', 'expiry-while-pending',
  'late-reply-after-reshow', 'start-set-invalid', 'start-revision-mismatch',
  'start-null-record-floor', 'precommit-revision-below-hwm', 'pending-hide-reshow',
  'check-timer-clamp-low', 'two-kinds', 'focus-defers-swap', 'click-opens',
  'timer-while-hidden', 'commit-resets-failures',
  'stale-signed-while-newer-runs', 'stale-committed-while-newer-writes',
  'stale-failed-while-newer-runs', 'photos-in-signed-phase', 'committed-in-photos-phase',
  'restart-clears-holds', 'hide-clears-holds', 'expiry-check-on-hidden', 'timer-resume-hidden',
  'resume-records-mayfetch', 'click-pointer-focus-hidden', 'precommit-at-expiry',
  'committed-no-expiry-recheck', 'resume-resets-timer', 'pending-replacement-expires-first',
  'precommit-unparsable-expiry', 'show-missing-drawn-photo-hash',
];

const readManifest = () => JSON.parse(fs.readFileSync(casesPath, 'utf8'));
const readTransitions = () => JSON.parse(fs.readFileSync(transitionsPath, 'utf8'));
const readCase = (entry) => new Uint8Array(fs.readFileSync(path.join(vectorsDir, entry.file)));
const firstLf = (bytes) => bytes.indexOf(LF);
const jsonFromSigned = (bytes) => {
  const lineEnd = firstLf(bytes);
  assert.ok(lineEnd >= 0, 'signed fixture has no JSON line');
  return JSON.parse(new TextDecoder().decode(bytes.subarray(lineEnd + 1)));
};
const sha256Hex = async (bytes) => {
  const digest = await webcrypto.subtle.digest('SHA-256', bytes);
  return Buffer.from(digest).toString('hex');
};
const publicKeyFrom = async (manifest) => webcrypto.subtle.importKey(
  'raw',
  Uint8Array.from(manifest.publicKeyHex.match(/../g), (pair) => Number.parseInt(pair, 16)),
  { name: 'ECDSA', namedCurve: 'P-256' },
  false,
  ['verify'],
);
const signatureFrom = (bytes) => {
  const lineEnd = firstLf(bytes);
  assert.ok(lineEnd >= 0, 'signed fixture has no signature line terminator');
  return new Uint8Array(Buffer.from(Buffer.from(bytes.subarray(0, lineEnd)).toString('ascii'), 'base64'));
};

test('vector files exist', () => {
  assert.equal(fs.existsSync(casesPath), true, 'cases.json is missing');
  assert.equal(fs.existsSync(transitionsPath), true, 'transitions.json is missing');
});

test('manifest key metadata is self-consistent', async () => {
  const manifest = readManifest();
  assert.match(manifest.publicKeyHex, /^04[0-9a-f]{128}$/);
  assert.match(manifest.keyId, /^[0-9a-f]{16}$/);
  assert.ok(Array.isArray(manifest.cases));
  const digestHex = await sha256Hex(Uint8Array.from(manifest.publicKeyHex.match(/../g), (pair) => Number.parseInt(pair, 16)));
  assert.equal(manifest.keyId, digestHex.slice(0, 16));
});

test('all expected case names and outcomes are unchanged', () => {
  const manifest = readManifest();
  const byName = new Map(manifest.cases.map((entry) => [entry.name, entry]));
  assert.equal(byName.size, manifest.cases.length, 'case names must be unique');
  assert.deepEqual([...byName.keys()].sort(), Object.keys(expectedCases).sort());
  for (const [name, [expect, reason]] of Object.entries(expectedCases)) {
    const entry = byName.get(name);
    assert.ok(entry, `missing case ${name}`);
    assert.equal(entry.expect, expect, `${name} expect`);
    assert.equal(entry.reason, reason, `${name} reason`);
    assert.match(entry.now, /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$/);
    assert.match(entry.file, /^cases\/[a-z0-9.-]+\.signed$/);
  }
});

test('every named case file and stored file exists', () => {
  const manifest = readManifest();
  for (const entry of manifest.cases) {
    assert.equal(fs.existsSync(path.join(vectorsDir, entry.file)), true, `${entry.name} file is missing`);
    if (entry.stored !== null) {
      assert.equal(fs.existsSync(path.join(vectorsDir, entry.stored)), true, `${entry.name} stored file is missing`);
    }
  }
});

test('every photo file named by a case exists', () => {
  const manifest = readManifest();
  for (const entry of manifest.cases) {
    if (entry.photo === null) continue;
    assert.equal(fs.existsSync(path.join(vectorsDir, entry.photo.file)), true, `${entry.name} photo is missing`);
    assert.match(entry.photo.file, /^photos\/[a-z0-9._-]+$/);
    assert.ok(['webp', 'jpg'].includes(entry.photo.kind));
    assert.ok(photoReasons.has(entry.photo.expect));
  }
});

test('every case reason is a fixed reason code', () => {
  const manifest = readManifest();
  for (const entry of manifest.cases) {
    assert.ok(entry.reason === null || reasonCodes.has(entry.reason), `${entry.name} reason code`);
  }
});

test('valid fixture has an LF-delimited standard signature line', () => {
  const manifest = readManifest();
  const entry = manifest.cases.find((candidate) => candidate.name === 'valid');
  const signed = readCase(entry);
  assert.equal(signed[88], LF);
  assert.notEqual(signed[87], CR);
  assert.equal(firstLf(signed), 88);
  assert.match(Buffer.from(signed.subarray(0, 88)).toString('ascii'), /^[A-Za-z0-9+/]{86}==$/);
});

test('valid and update-style signatures prove both domain-separation directions', async () => {
  const manifest = readManifest();
  const publicKey = await publicKeyFrom(manifest);
  for (const name of ['valid', 'sig-update-style']) {
    const entry = manifest.cases.find((candidate) => candidate.name === name);
    const signed = readCase(entry);
    const jsonBytes = signed.subarray(firstLf(signed) + 1);
    const signature = signatureFrom(signed);
    const verifiesDomainSeparated = await webcrypto.subtle.verify(
      { name: 'ECDSA', hash: 'SHA-256' },
      publicKey,
      signature,
      domainSeparated(jsonBytes),
    );
    const verifiesBareJson = await webcrypto.subtle.verify(
      { name: 'ECDSA', hash: 'SHA-256' },
      publicKey,
      signature,
      jsonBytes,
    );
    if (name === 'valid') {
      assert.equal(verifiesDomainSeparated, true);
      assert.equal(verifiesBareJson, false);
    } else {
      assert.equal(verifiesBareJson, true);
      assert.equal(verifiesDomainSeparated, false);
    }
  }
});

test('non-sigline and non-size fixtures have an exact 88-character signature line', () => {
  const manifest = readManifest();
  for (const entry of manifest.cases) {
    if (entry.reason === 'sigline' || entry.reason === 'size') continue;
    const signed = readCase(entry);
    assert.equal(firstLf(signed), 88, `${entry.name} first LF`);
    assert.equal(signed.subarray(0, 88).includes(CR), false, `${entry.name} signature CR`);
    assert.match(Buffer.from(signed.subarray(0, 88)).toString('ascii'), /^[A-Za-z0-9+/]{86}==$/, `${entry.name} signature`);
  }
});

test('size-over is exactly one byte beyond the maximum signed size', () => {
  const manifest = readManifest();
  const entry = manifest.cases.find((candidate) => candidate.name === 'size-over');
  assert.equal(readCase(entry).length, 16_385);
});

test('sigline-long has its first LF at byte 101', () => {
  const manifest = readManifest();
  const entry = manifest.cases.find((candidate) => candidate.name === 'sigline-long');
  assert.equal(firstLf(readCase(entry)), 101);
});

test('photo-size names the true SHA-256 of its 300001-byte fixture', async () => {
  const manifest = readManifest();
  const entry = manifest.cases.find((candidate) => candidate.name === 'photo-size');
  const photoBytes = new Uint8Array(fs.readFileSync(path.join(vectorsDir, entry.photo.file)));
  assert.equal(photoBytes.length, 300_001);
  const banner = jsonFromSigned(readCase(entry));
  assert.equal(await sha256Hex(photoBytes), banner.photo.sha256[entry.photo.kind]);
});

test('transition scenarios cover the required names and valid event/effect shapes', () => {
  const transitions = readTransitions();
  assert.ok(Array.isArray(transitions.scenarios));
  assert.ok(transitions.scenarios.length > 0, 'scenarios must not be empty');
  const scenarios = new Map(transitions.scenarios.map((scenario) => [scenario.name, scenario]));
  for (const name of requiredScenarios) {
    assert.ok(scenarios.has(name), `missing scenario ${name}`);
    const scenario = scenarios.get(name);
    assert.ok(Array.isArray(scenario.steps) && scenario.steps.length > 0);
    assert.equal(scenario.steps[0].event.type, 'START', `${name} must start with START`);
    assert.ok(Array.isArray(scenario.kinds) && scenario.kinds.every((kind) => ['webp', 'jpg'].includes(kind)));
    for (const step of scenario.steps) {
      assert.equal(Number.isInteger(step.atMs), true, `${name} atMs`);
      assert.ok(eventTypes.has(step.event.type), `${name} event ${step.event.type}`);
      assert.ok(Array.isArray(step.expect));
      for (const effect of step.expect) assert.ok(effectTypes.has(effect.type), `${name} effect ${effect.type}`);
    }
  }
});

test('all transition state assertions are objects when present', () => {
  const transitions = readTransitions();
  for (const scenario of transitions.scenarios) {
    for (const step of scenario.steps) {
      if (step.state !== undefined) assert.equal(typeof step.state, 'object');
    }
  }
});
