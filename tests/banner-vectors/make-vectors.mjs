import { webcrypto } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../../../..');
const casesDir = path.join(here, 'cases');
const photosDir = path.join(here, 'photos');
const webpSource = path.join(root, 'site', 'public', 'assets', 'hero-coffee-800.webp');
const jpgSource = path.join(root, 'site', 'public', 'assets', 'hero-coffee-800.jpg');
const qrSource = path.join(root, 'brand', 'goatcam-banner', 'assets', 'qr-matrix.json');
const encoder = new TextEncoder();

const prefix = encoder.encode('GOATCAM-BANNER-V1\n');
const LF = Uint8Array.of(0x0a);
const SIZE_OVER_FILE_BYTES = 16_385;
const SIGNED_HEADER_BYTES = 89; // 88 signature characters plus the LF before JSON.
const nowDefault = '2026-11-02T12:00:00Z';
const issuedDefault = '2026-11-02T00:00:00Z';
const expiresDefault = '2027-11-02T00:00:00Z';
const revisionDefault = 7;
const hwmDefault = 1;
const floorDefault = 1;

const concat = (...parts) => {
  const result = new Uint8Array(parts.reduce((total, part) => total + part.length, 0));
  let offset = 0;
  for (const part of parts) {
    result.set(part, offset);
    offset += part.length;
  }
  return result;
};

const hex = (bytes) => Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('');
const sha256 = async (bytes) => hex(new Uint8Array(await webcrypto.subtle.digest('SHA-256', bytes)));
const codePoints = (text) => Array.from(text);
const fillText = (seed, length) => {
  const chars = codePoints(seed);
  const filler = ['·', '®', '™', '…', '–', '—', '‘', '’', '“', '”'];
  let index = 0;
  while (chars.length < length) {
    chars.push(filler[index % filler.length]);
    index += 1;
  }
  return chars.slice(0, length).join('');
};

const findBytes = (bytes, needle) => {
  outer: for (let start = 0; start <= bytes.length - needle.length; start += 1) {
    for (let index = 0; index < needle.length; index += 1) {
      if (bytes[start + index] !== needle[index]) continue outer;
    }
    return start;
  }
  return -1;
};

const replaceBytes = (bytes, needle, replacement) => {
  const start = findBytes(bytes, needle);
  if (start < 0) throw new Error('byte marker not found');
  return concat(bytes.slice(0, start), replacement, bytes.slice(start + needle.length));
};

const base64 = (bytes) => Buffer.from(bytes).toString('base64');
const write = (file, bytes) => fs.writeFileSync(file, bytes);

const bytes = {
  webp: new Uint8Array(fs.readFileSync(webpSource)),
  jpg: new Uint8Array(fs.readFileSync(jpgSource)),
};
const hashes = {
  webp: await sha256(bytes.webp),
  jpg: await sha256(bytes.jpg),
};

const qrFile = JSON.parse(fs.readFileSync(qrSource, 'utf8'));
if (
  typeof qrFile.url !== 'string'
  || qrFile.modules !== 37
  || !Array.isArray(qrFile.rows)
  || qrFile.rows.length !== 37
  || qrFile.rows.some((row) => typeof row !== 'string' || !/^[01]{37}$/.test(row))
) {
  throw new Error('qr-matrix.json has unexpected shape');
}
const qrRows = qrFile.rows;
const qrText = qrFile.url;

const keyPair = await webcrypto.subtle.generateKey(
  { name: 'ECDSA', namedCurve: 'P-256' },
  true,
  ['sign', 'verify'],
);
const publicKeyBytes = new Uint8Array(await webcrypto.subtle.exportKey('raw', keyPair.publicKey));
const publicKeyHex = hex(publicKeyBytes);
const keyId = (await sha256(publicKeyBytes)).slice(0, 16);

const photo = (
  webpHash = hashes.webp,
  jpgHash = hashes.jpg,
  h = 498,
  webpName = 'hero-coffee-800.webp',
  jpgName = 'hero-coffee-800.jpg',
) => ({
  webp: webpName,
  jpg: jpgName,
  sha256: { webp: webpHash, jpg: jpgHash },
  w: 800,
  h,
  focus: [0.30, 0.55],
  steamAt: [0.17, 0.62],
  textBox: [0.33, 0.10, 0.40, 0.45],
});

const banner = (overrides = {}) => {
  const result = {
    schema: 1,
    keyId,
    revision: revisionDefault,
    issued: issuedDefault,
    expires: expiresDefault,
    enabled: true,
    headline: 'GOAT Cam — your PC’s webcam, NOW.',
    headlineEm: 'NOW',
    wordmark: 'GOAT Cam',
    tagline: ['iPhone app', 'On the App Store'],
    badge: '1080p · 60 fps',
    effects: { viewfinder: true, steam: true },
    link: 'https://goatcam.dagoat.io',
    qr: { text: qrText, size: 37, rows: qrRows },
    photo: photo(),
    labels: {
      link: 'GOAT Cam — open goatcam.dagoat.io',
      qr: 'QR code that opens GOAT Cam on the App Store',
    },
  };
  return { ...result, ...overrides };
};

const killSwitch = (overrides = {}) => ({
  schema: 1,
  keyId,
  revision: revisionDefault,
  issued: issuedDefault,
  expires: expiresDefault,
  enabled: false,
  ...overrides,
});

const maxTextBanner = () => {
  const result = banner();
  result.headline = fillText('NOW — “GOAT” PC’s webcam…™®·', 48);
  result.headlineEm = result.headline;
  result.wordmark = fillText('GOAT Cam —', 12);
  result.tagline = [fillText('iPhone app —', 18), fillText('Store “GOAT”…', 18)];
  result.badge = fillText('1080p ·', 16);
  result.labels = {
    link: fillText('GOAT Cam — open “link”…™®', 80),
    qr: fillText('QR code — App Store “GOAT”…™®', 80),
  };
  return result;
};

const asJsonBytes = (value) => encoder.encode(JSON.stringify(value));
const replaceJsonToken = (json, key, token) => json.replace(
  new RegExp(`("${key}"\\s*:\\s*)-?(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?`),
  `$1${token}`,
);
const spacedJson = (json) => {
  let result = '';
  let inString = false;
  let escaped = false;
  for (const character of json) {
    if (inString) {
      result += character;
      if (escaped) escaped = false;
      else if (character === '\\') escaped = true;
      else if (character === '"') inString = false;
      continue;
    }
    if (character === '"') {
      inString = true;
      result += character;
    } else if (character === ':' || character === ',') {
      result += `${character} \t`;
    } else {
      result += character;
    }
  }
  return `\n \t${result}\r\n`;
};
const signFile = async (jsonBytes, {
  domainSeparated = true,
  mutateSignature = false,
  lineMutator = null,
  includeLf = true,
} = {}) => {
  const payload = domainSeparated ? concat(prefix, jsonBytes) : jsonBytes;
  let signature = new Uint8Array(await webcrypto.subtle.sign(
    { name: 'ECDSA', hash: 'SHA-256' },
    keyPair.privateKey,
    payload,
  ));
  if (signature.length !== 64) throw new Error(`expected P1363 signature, got ${signature.length} bytes`);
  if (mutateSignature) {
    signature = new Uint8Array(signature);
    signature[0] ^= 0x01;
  }
  let line = base64(signature);
  if (lineMutator) line = lineMutator(line);
  return concat(encoder.encode(line), includeLf ? LF : new Uint8Array(), jsonBytes);
};

const animWebp = (width, height) => {
  const chunk = (name, data) => {
    const size = new Uint8Array(4);
    new DataView(size.buffer).setUint32(0, data.length, true);
    const pad = data.length % 2 ? Uint8Array.of(0) : new Uint8Array();
    return concat(encoder.encode(name), size, data, pad);
  };
  const vp8x = new Uint8Array(10);
  vp8x[0] = 0x02;
  const canvasWidth = width - 1;
  const canvasHeight = height - 1;
  vp8x[4] = canvasWidth & 0xff;
  vp8x[5] = (canvasWidth >> 8) & 0xff;
  vp8x[6] = (canvasWidth >> 16) & 0xff;
  vp8x[7] = canvasHeight & 0xff;
  vp8x[8] = (canvasHeight >> 8) & 0xff;
  vp8x[9] = (canvasHeight >> 16) & 0xff;
  const anim = chunk('ANIM', new Uint8Array(6));
  const anmf = chunk('ANMF', new Uint8Array(16));
  const body = concat(chunk('VP8X', vp8x), anim, anmf);
  const riffSize = new Uint8Array(4);
  new DataView(riffSize.buffer).setUint32(0, body.length + 4, true);
  return concat(encoder.encode('RIFF'), riffSize, encoder.encode('WEBP'), body);
};

const writeUint32Little = (target, offset, value) => {
  target[offset] = value & 0xff;
  target[offset + 1] = (value >>> 8) & 0xff;
  target[offset + 2] = (value >>> 16) & 0xff;
  target[offset + 3] = (value >>> 24) & 0xff;
};
const webpChunk = (fourCC, data, includePad = true) => {
  const pad = includePad && data.length % 2 === 1 ? 1 : 0;
  const result = new Uint8Array(8 + data.length + pad);
  result.set(encoder.encode(fourCC), 0);
  writeUint32Little(result, 4, data.length);
  result.set(data, 8);
  return result;
};
const webpFile = (chunks) => {
  const body = concat(...chunks);
  const result = new Uint8Array(12 + body.length);
  result.set(encoder.encode('RIFF'), 0);
  writeUint32Little(result, 4, body.length + 4);
  result.set(encoder.encode('WEBP'), 8);
  result.set(body, 12);
  return result;
};
const vp8xData = (w = 800, h = 498, animated = false) => {
  const data = new Uint8Array(10);
  data[0] = animated ? 0x02 : 0;
  data[4] = (w - 1) & 0xff;
  data[5] = ((w - 1) >>> 8) & 0xff;
  data[6] = ((w - 1) >>> 16) & 0xff;
  data[7] = (h - 1) & 0xff;
  data[8] = ((h - 1) >>> 8) & 0xff;
  data[9] = ((h - 1) >>> 16) & 0xff;
  return data;
};
const vp8lData = (w = 800, h = 498) => {
  const bits = (w - 1) | ((h - 1) << 14);
  return Uint8Array.of(0x2f, bits & 0xff, (bits >>> 8) & 0xff, (bits >>> 16) & 0xff, (bits >>> 24) & 0xff);
};
const jpegSof = (fill = false) => Uint8Array.of(
  0xff, 0xd8,
  ...(fill ? [0xff, 0xff] : [0xff]), 0xc0,
  0x00, 0x07, 0x08, 0x01, 0xf2, 0x03, 0x20,
);

fs.mkdirSync(casesDir, { recursive: true });
fs.mkdirSync(photosDir, { recursive: true });
for (const directory of [casesDir, photosDir]) {
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    if (entry.isFile()) fs.unlinkSync(path.join(directory, entry.name));
  }
}

const flippedWebp = new Uint8Array(bytes.webp);
flippedWebp[flippedWebp.length - 1] ^= 0x01;
const animBanner = banner();
const animBytes = animWebp(animBanner.photo.w, animBanner.photo.h);
const vp8lWebp = webpFile([webpChunk('VP8L', vp8lData(), false)]);
const trailingWebp = concat(webpFile([webpChunk('VP8X', vp8xData())]), Uint8Array.of(0xaa, 0xbb, 0xcc));
const vp8xAnimWebp = webpFile([webpChunk('VP8X', vp8xData(800, 498, true))]);
const animOnlyWebp = webpFile([
  webpChunk('VP8X', vp8xData()),
  webpChunk('ANIM', new Uint8Array()),
]);
const anmfOnlyWebp = webpFile([
  webpChunk('VP8X', vp8xData()),
  webpChunk('ANMF', new Uint8Array()),
]);
const jpegFill = jpegSof(true);
const jpegSofAtEof = jpegSof(false);
const emptyPhoto = new Uint8Array();
const oversizeBytes = new Uint8Array(300001);
for (let index = 0; index < oversizeBytes.length; index += 1) oversizeBytes[index] = (index * 31 + 17) & 0xff;
const photoHashes = {
  flippedWebp: await sha256(flippedWebp),
  animWebp: await sha256(animBytes),
  vp8lWebp: await sha256(vp8lWebp),
  trailingWebp: await sha256(trailingWebp),
  vp8xAnimWebp: await sha256(vp8xAnimWebp),
  animOnlyWebp: await sha256(animOnlyWebp),
  anmfOnlyWebp: await sha256(anmfOnlyWebp),
  jpegFill: await sha256(jpegFill),
  jpegSofAtEof: await sha256(jpegSofAtEof),
  emptyPhoto: await sha256(emptyPhoto),
  oversize: await sha256(oversizeBytes),
};

write(path.join(photosDir, 'hero-coffee-800.webp'), bytes.webp);
write(path.join(photosDir, 'hero-coffee-800.jpg'), bytes.jpg);
write(path.join(photosDir, 'photo-hash.webp'), flippedWebp);
write(path.join(photosDir, 'photo-anim.webp'), animBytes);
write(path.join(photosDir, 'photo-webp-nopad-end.webp'), vp8lWebp);
write(path.join(photosDir, 'photo-webp-trailing.webp'), trailingWebp);
write(path.join(photosDir, 'photo-vp8x-animflag.webp'), vp8xAnimWebp);
write(path.join(photosDir, 'photo-anim-only-anim.webp'), animOnlyWebp);
write(path.join(photosDir, 'photo-anim-only-anmf.webp'), anmfOnlyWebp);
write(path.join(photosDir, 'photo-vp8l.webp'), vp8lWebp);
write(path.join(photosDir, 'photo-jpeg-fill.jpg'), jpegFill);
write(path.join(photosDir, 'photo-jpeg-sof-at-eof.jpg'), jpegSofAtEof);
write(path.join(photosDir, 'photo-empty.webp'), emptyPhoto);
write(path.join(photosDir, 'photo-size.webp'), oversizeBytes);

const cases = [];
const addCase = async (name, {
  value = banner(),
  now = nowDefault,
  hwm = hwmDefault,
  floor = floorDefault,
  stored = null,
  expect = 'show',
  reason = null,
  photoInfo = null,
  rawBytes = null,
  signOptions = {},
  sizeOver = false,
  sizeTotal = null,
} = {}) => {
  let jsonBytes = rawBytes ?? asJsonBytes(value);
  if (sizeOver || sizeTotal !== null) {
    const targetFileBytes = sizeOver ? SIZE_OVER_FILE_BYTES : sizeTotal;
    const targetJsonLength = targetFileBytes - SIGNED_HEADER_BYTES;
    if (jsonBytes.length >= targetJsonLength) throw new Error('size-over fixture is unexpectedly large');
    jsonBytes = concat(jsonBytes, new Uint8Array(targetJsonLength - jsonBytes.length).fill(0x20));
  }
  const signed = await signFile(jsonBytes, signOptions);
  const file = `cases/${name}.signed`;
  write(path.join(here, file.replaceAll('/', path.sep)), signed);
  cases.push({ name, file, now, hwm, floor, stored, expect, reason, photo: photoInfo });
};

await addCase('valid');
await addCase('valid-killswitch', { value: killSwitch(), expect: 'killswitch' });
await addCase('unknown-key', { value: { ...banner(), future: { x: [1] } } });
await addCase('max-text', { value: maxTextBanner() });
await addCase('sig-bad', { expect: 'reject', reason: 'sig', signOptions: { mutateSignature: true } });
await addCase('sig-update-style', { expect: 'reject', reason: 'sig', signOptions: { domainSeparated: false } });
await addCase('sigline-long', { expect: 'reject', reason: 'sigline', signOptions: { lineMutator: (line) => `${line}${'A'.repeat(13)}` } });
await addCase('sigline-missing', { expect: 'reject', reason: 'sigline', signOptions: { includeLf: false } });
await addCase('size-over', { expect: 'reject', reason: 'size', sizeOver: true });

const utf8Valid = asJsonBytes(banner({ headline: 'BAD NOW' }));
const utf8Bad = replaceBytes(utf8Valid, encoder.encode('BAD'), Uint8Array.of(0xc3, 0x28));
await addCase('utf8-bad', { expect: 'reject', reason: 'utf8', rawBytes: utf8Bad });

const trailingJson = concat(asJsonBytes(banner()), encoder.encode('x'));
await addCase('json-trailing', { expect: 'reject', reason: 'json', rawBytes: trailingJson });

await addCase('json-whitespace', { rawBytes: encoder.encode(spacedJson(JSON.stringify(banner()))) });
await addCase('json-pretty', { rawBytes: encoder.encode(JSON.stringify(banner(), null, 2)) });
const spacedKillSwitch = JSON.stringify(killSwitch()).replace('"enabled":false', '"enabled" : false');
await addCase('killswitch-spaced', { value: killSwitch(), expect: 'killswitch', rawBytes: encoder.encode(spacedKillSwitch) });
await addCase('schema-1.0', { expect: 'reject', reason: 'schema', rawBytes: encoder.encode(replaceJsonToken(JSON.stringify(banner()), 'schema', '1.0')) });
await addCase('schema-1e0', { expect: 'reject', reason: 'schema', rawBytes: encoder.encode(replaceJsonToken(JSON.stringify(banner()), 'schema', '1e0')) });

const depth4ThenTrailing = `${JSON.stringify({ ...banner(), future: { a: { b: [1] } } })} x`;
await addCase('depth4-then-trailing', { expect: 'reject', reason: 'json', rawBytes: encoder.encode(depth4ThenTrailing) });
const depth4ThenDupKeyBase = JSON.stringify(banner());
const depth4ThenDupKey = `{"future":{"a":{"b":[1]}},${depth4ThenDupKeyBase.slice(1).replace(
  ',"revision"', `,"keyId":${JSON.stringify(keyId)},"revision"`,
)}`;
await addCase('depth4-then-dupkey', { expect: 'reject', reason: 'dupkey', rawBytes: encoder.encode(depth4ThenDupKey) });
let depth33 = '0';
for (let index = 0; index < 33; index += 1) depth33 = `{"x":${depth33}}`;
const dupKeyThenDepth33 = `{"schema":1,"schema":1,"keyId":${JSON.stringify(keyId)},"future":${depth33}}`;
await addCase('dupkey-then-depth33', { expect: 'reject', reason: 'depth', rawBytes: encoder.encode(dupKeyThenDepth33) });
let depth32Malformed = '{"future":';
for (let index = 0; index < 31; index += 1) depth32Malformed += '{"x":';
depth32Malformed += '!';
await addCase('depth32-malformed', { expect: 'reject', reason: 'depth', rawBytes: encoder.encode(`${depth32Malformed}${'}'.repeat(32)}`) });
await addCase('toplevel-array', { expect: 'reject', reason: 'json', rawBytes: encoder.encode(`[${JSON.stringify(banner())}]`) });
await addCase('size-exact-16384', { sizeTotal: 16_384 });

const duplicateBanner = banner();
const duplicateText = JSON.stringify(duplicateBanner).replace(
  ',"wordmark"',
  `,"headline":${JSON.stringify(duplicateBanner.headline)},"wordmark"`,
);
await addCase('json-dupkey', { expect: 'reject', reason: 'dupkey', rawBytes: encoder.encode(duplicateText) });
await addCase('json-depth4', { expect: 'reject', reason: 'depth', value: { ...banner(), future: { a: { b: [1] } } } });
await addCase('schema-2', { expect: 'reject', reason: 'schema', value: banner({ schema: 2 }) });
await addCase('keyid-wrong', { expect: 'reject', reason: 'keyid', value: banner({ keyId: '0000000000000000' }) });

const missingHeadline = banner();
delete missingHeadline.headline;
await addCase('field-missing-headline', { expect: 'reject', reason: 'field', value: missingHeadline });
const upperSha = hashes.webp.toUpperCase();
await addCase('sha-upper', { expect: 'reject', reason: 'field', value: banner({ photo: photo(upperSha) }) });
await addCase('text-headline-49', { expect: 'reject', reason: 'text', value: banner({ headline: 'A'.repeat(49), headlineEm: 'A' }) });
await addCase('text-bidi', { expect: 'reject', reason: 'text', value: banner({ headline: `GOAT Cam NOW\u202E` }) });
await addCase('text-zero-width', { expect: 'reject', reason: 'text', value: banner({ wordmark: `GOAT\u200BCam` }) });
for (const [name, code] of [['text-del', 0x7f], ['text-soft-hyphen', 0xad], ['text-u0250', 0x250], ['text-u2010', 0x2010]]) {
  await addCase(name, { expect: 'reject', reason: 'text', value: banner({ headline: `A${String.fromCodePoint(code)}` }) });
}
await addCase('em-not-substring', { expect: 'reject', reason: 'text', value: banner({ headline: 'GoatCam', headlineEm: 'Nope' }) });
await addCase('tagline-19', { expect: 'reject', reason: 'text', value: banner({ tagline: ['a'.repeat(19)] }) });
await addCase('badge-17', { expect: 'reject', reason: 'text', value: banner({ badge: 'a'.repeat(17) }) });
await addCase('label-81', { expect: 'reject', reason: 'text', value: banner({ labels: { ...banner().labels, link: 'a'.repeat(81) } }) });
await addCase('url-http', { expect: 'reject', reason: 'url', value: banner({ link: 'http://goatcam.dagoat.io' }) });
await addCase('url-userinfo', { expect: 'reject', reason: 'url', value: banner({ link: 'https://user:pass@goatcam.dagoat.io/' }) });
await addCase('url-ip', { expect: 'reject', reason: 'url', value: banner({ link: 'https://192.168.1.10/' }) });
await addCase('url-port', { expect: 'reject', reason: 'url', value: banner({ link: 'https://goatcam.dagoat.io:8443/' }) });
await addCase('url-label-hyphen', { expect: 'reject', reason: 'url', value: banner({ link: 'https://goatcam-.io/' }) });
await addCase('url-label-64', { expect: 'reject', reason: 'url', value: banner({ link: `https://${'a'.repeat(64)}.com/` }) });
await addCase('url-tld-1', { expect: 'reject', reason: 'url', value: banner({ link: 'https://goatcam.d/' }) });
await addCase('url-tld-digits', { expect: 'reject', reason: 'url', value: banner({ link: 'https://goatcam.123/' }) });
await addCase('url-idn-tld', { value: banner({ link: 'https://xn--d1acufc.xn--p1ai' }) });
await addCase('url-port-443', { value: banner({ link: 'https://goatcam.dagoat.io:443/' }) });
await addCase('url-201', { expect: 'reject', reason: 'url', value: banner({ link: `https://${'a'.repeat(63)}.${'b'.repeat(63)}.${'c'.repeat(63)}.com/` }) });
await addCase('url-non-ascii', { expect: 'reject', reason: 'url', value: banner({ link: 'https://goatcam.ä¾‹å­/' }) });
await addCase('textbox-sum', { expect: 'reject', reason: 'field', value: banner({ photo: { ...banner().photo, textBox: [0.5, 0, 0.6, 0] } }) });
await addCase('w-1601', { expect: 'reject', reason: 'field', value: banner({ photo: { ...banner().photo, w: 1601 } }) });
await addCase('revision-2p53', { expect: 'reject', reason: 'field', value: banner({ revision: 9007199254740992 }) });
await addCase('date-feb29-2027', { expect: 'reject', reason: 'field', value: banner({ issued: '2027-02-29T00:00:00Z' }) });
await addCase('hour-24', { expect: 'reject', reason: 'field', value: banner({ issued: '2026-11-02T24:00:00Z' }) });
await addCase('year-1999', { expect: 'reject', reason: 'field', value: banner({ issued: '1999-11-02T00:00:00Z' }) });
await addCase('qr-size-33', { expect: 'reject', reason: 'qr', value: banner({ qr: { ...banner().qr, size: 33 } }) });
await addCase('qr-size-huge', {
  expect: 'reject',
  reason: 'qr',
  rawBytes: encoder.encode(replaceJsonToken(JSON.stringify(banner()), 'size', `1${'0'.repeat(400)}`)),
});
await addCase('qr-size-negative', { expect: 'reject', reason: 'qr', value: banner({ qr: { ...banner().qr, size: -37 } }) });
await addCase('qr-row-number', { expect: 'reject', reason: 'field', value: banner({ qr: { ...banner().qr, rows: [1, ...qrRows.slice(1)] } }) });
await addCase('qr-row-short', { expect: 'reject', reason: 'qr', value: banner({ qr: { ...banner().qr, rows: [...qrRows.slice(0, 1), '0'.repeat(36), ...qrRows.slice(2)] } }) });

await addCase('time-expired', { expect: 'reject', reason: 'time', now: expiresDefault });
await addCase('time-issued-future', { expect: 'reject', reason: 'time', value: banner({ issued: '2026-11-04T12:00:00Z' }) });
await addCase('time-window-401d', { expect: 'reject', reason: 'time', value: banner({ expires: '2027-12-08T00:00:00Z' }) });
await addCase('time-expires-before-issued', {
  expect: 'reject',
  reason: 'time',
  value: banner({ issued: '2026-11-02T11:00:00Z', expires: '2026-11-02T10:59:59Z' }),
});
await addCase('time-expires-equals-issued', {
  expect: 'reject',
  reason: 'time',
  value: banner({ issued: '2026-11-02T11:00:00Z', expires: '2026-11-02T11:00:00Z' }),
});
const addDays = (iso, days) => new Date(Date.parse(iso) + days * 86_400_000).toISOString().replace('.000Z', 'Z');
await addCase('clock-future', { expect: 'reject', reason: 'time', now: addDays(expiresDefault, 730) });
await addCase('clock-past', { expect: 'reject', reason: 'time', now: '2026-10-03T00:00:00Z' });
await addCase('revision-below-hwm', { expect: 'reject', reason: 'revision', hwm: 8 });
await addCase('revision-below-floor', { expect: 'reject', reason: 'floor', floor: 8 });
await addCase('hwm-negative', { expect: 'reject', reason: 'revision', hwm: -1 });
await addCase('floor-negative', { expect: 'reject', reason: 'floor', floor: -1 });

const equalSame = banner();
await addCase('revision-equal-different', {
  expect: 'reject',
  reason: 'equal',
  hwm: 7,
  stored: 'cases/revision-equal-same.signed',
  value: banner({ headline: 'A different NOW revision-seven banner' }),
});
await addCase('revision-equal-same', { hwm: 7, stored: 'cases/revision-equal-same.signed', value: equalSame });
await addCase('revision-equal-nostored', { hwm: 7 });

await addCase('photo-ok-webp', { photoInfo: { file: 'photos/hero-coffee-800.webp', kind: 'webp', expect: 'ok' } });
await addCase('photo-ok-jpg', { photoInfo: { file: 'photos/hero-coffee-800.jpg', kind: 'jpg', expect: 'ok' } });
await addCase('photo-hash', { photoInfo: { file: 'photos/photo-hash.webp', kind: 'webp', expect: 'photo-hash' } });
await addCase('photo-dims', { value: banner({ photo: photo(hashes.webp, hashes.jpg, 499) }), photoInfo: { file: 'photos/hero-coffee-800.webp', kind: 'webp', expect: 'photo-dims' } });
await addCase('photo-anim', { value: banner({ photo: photo(photoHashes.animWebp) }), photoInfo: { file: 'photos/photo-anim.webp', kind: 'webp', expect: 'photo-anim' } });
await addCase('photo-size', { value: banner({ photo: photo(photoHashes.oversize) }), photoInfo: { file: 'photos/photo-size.webp', kind: 'webp', expect: 'photo-size' } });
await addCase('base64-noncanonical', {
  expect: 'reject',
  reason: 'sigline',
  signOptions: {
    lineMutator: (line) => {
      const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
      const value = alphabet.indexOf(line[85]);
      return `${line.slice(0, 85)}${alphabet[value ^ 1]}${line.slice(86)}`;
    },
  },
});

await addCase('photo-webp-nopad-end', {
  value: banner({ photo: photo(photoHashes.vp8lWebp, hashes.jpg, 498, 'photo-webp-nopad-end.webp') }),
  photoInfo: { file: 'photos/photo-webp-nopad-end.webp', kind: 'webp', expect: 'ok' },
});
await addCase('photo-webp-trailing', {
  value: banner({ photo: photo(photoHashes.trailingWebp, hashes.jpg, 498, 'photo-webp-trailing.webp') }),
  photoInfo: { file: 'photos/photo-webp-trailing.webp', kind: 'webp', expect: 'photo-dims' },
});
await addCase('photo-vp8x-animflag', {
  value: banner({ photo: photo(photoHashes.vp8xAnimWebp, hashes.jpg, 498, 'photo-vp8x-animflag.webp') }),
  photoInfo: { file: 'photos/photo-vp8x-animflag.webp', kind: 'webp', expect: 'photo-anim' },
});
await addCase('photo-anim-only-anim', {
  value: banner({ photo: photo(photoHashes.animOnlyWebp, hashes.jpg, 498, 'photo-anim-only-anim.webp') }),
  photoInfo: { file: 'photos/photo-anim-only-anim.webp', kind: 'webp', expect: 'photo-anim' },
});
await addCase('photo-anim-only-anmf', {
  value: banner({ photo: photo(photoHashes.anmfOnlyWebp, hashes.jpg, 498, 'photo-anim-only-anmf.webp') }),
  photoInfo: { file: 'photos/photo-anim-only-anmf.webp', kind: 'webp', expect: 'photo-anim' },
});
await addCase('photo-vp8l', {
  value: banner({ photo: photo(photoHashes.vp8lWebp, hashes.jpg, 498, 'photo-vp8l.webp') }),
  photoInfo: { file: 'photos/photo-vp8l.webp', kind: 'webp', expect: 'ok' },
});
await addCase('photo-jpeg-fill', {
  value: banner({ photo: photo(hashes.webp, photoHashes.jpegFill, 498, 'hero-coffee-800.webp', 'photo-jpeg-fill.jpg') }),
  photoInfo: { file: 'photos/photo-jpeg-fill.jpg', kind: 'jpg', expect: 'ok' },
});
await addCase('photo-jpeg-sof-at-eof', {
  value: banner({ photo: photo(hashes.webp, photoHashes.jpegSofAtEof, 498, 'hero-coffee-800.webp', 'photo-jpeg-sof-at-eof.jpg') }),
  photoInfo: { file: 'photos/photo-jpeg-sof-at-eof.jpg', kind: 'jpg', expect: 'ok' },
});
await addCase('photo-empty', {
  value: banner({ photo: photo(photoHashes.emptyPhoto, hashes.jpg, 498, 'photo-empty.webp') }),
  photoInfo: { file: 'photos/photo-empty.webp', kind: 'webp', expect: 'photo-size' },
});

write(path.join(here, 'cases.json'), encoder.encode(JSON.stringify({ publicKeyHex, keyId, cases }, null, 2) + '\n'));
