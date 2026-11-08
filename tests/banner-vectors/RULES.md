# Online banner — implementation rules (binding for plan 1)

Authority: design revision 9, sections 4 and 5 (docs\superpowers\specs\2026-11-02-online-banner-design.md).
This file pins what the design leaves open, so the vectors, the JavaScript code and the C++ code
make the same decisions. Every code file in this folder tree follows it. Decisions the design does
not state are marked **(R1)** … **(R26)**; each one is recorded as a ruling in the build ledger.

## A. File checker (banner-core.js, online_banner.cpp)

### A1. Checks, in order — the first failure is the reason

| # | reason | fails when |
|---|---|---|
| 1 | `size` | the whole file is longer than 16384 bytes |
| 2 | `sigline` | there is no LF in the first 101 bytes (line 1 longer than 100 bytes, or no LF at all); or line 1 is not exactly 88 characters of standard base64 (`A–Z a–z 0–9 + /`, `==` padding) decoding to exactly 64 bytes. A CR before the LF makes line 1 invalid. |
| 3 | `sig` | ECDSA P-256 with SHA-256 over the 17 bytes `GOATCAM-BANNER-V1`, then LF, then the JSON bytes (everything after the first LF) does not verify against the compiled public key. The signature is r‖s, 32 + 32 bytes (IEEE P1363). |
| 4 | `utf8` | the JSON bytes are not valid UTF-8 (overlong forms, U+D800–U+DFFF, above U+10FFFF and truncated sequences are invalid) |
| 5 | `json` | the text is not exactly one JSON object, optionally surrounded by JSON whitespace (space, tab, LF, CR) **(R1: trailing whitespace is allowed; anything else after the object is not)**; RFC 8259 grammar (no comments, no leading zeros, no `+`, no NaN); a raw U+0000–U+001F inside a string; a `\u` escape that leaves a lone surrogate |
| 6 | `dupkey` | any object at any depth has two members whose decoded names are equal (`"a"` and `"a"` are equal) |
| 7 | `depth` | nesting deeper than 3: the top-level object is depth 1 and each object or array inside a container adds 1 (`photo.sha256` and `qr.rows` are depth 3). **(R2)** Every parser reads with the same hard stop: a container whose nesting reaches 32 stops the parse (never at 4) and reports `depth` there without reading further; that stop wins over every other row, even for a top-level array. Below the stop, rows 5, 6 and 7 are judged on the whole text. |
| 8 | `schema` | `schema` is missing, not a number written as an integer (no fraction, no exponent: `1.0` and `1e0` fail), or not 1 |
| 9 | `keyid` | `keyId` is missing, not a string, or not equal to the expected keyId |
| 10 | `field` | see A2 |
| 11 | `text` | see A3 |
| 12 | `url` | see A4 (`link` and `qr.text`) |
| 13 | `qr` | `qr.size` is not 37; `qr.rows` is not exactly 37 strings of exactly 37 characters, each `0` or `1` |
| 14 | `time` | `issued` > now + 86 400 000 ms; or now ≥ `expires`; or `expires` − `issued` > 400 × 86 400 000 ms; or **(R21)** `expires` ≤ `issued` |
| 15 | `revision` | `revision` < hwm |
| 16 | `floor` | `revision` < floor |
| 17 | `equal` | `revision` = hwm, stored bytes are given, and this file is not byte-identical to them. **(R3)** When no stored bytes are given, an equal revision is accepted: a fresh install starts with hwm = floor = the latest published revision, and the publish tool never signs two files with one revision. |

Then: `enabled` false → action `killswitch`; otherwise `show`.

**Kill switch (R4):** when `enabled` is false, only `schema`, `keyId`, `revision`, `issued`, `expires`
and `enabled` are checked (rows 8–10 for those six, then 14–17); every other key is ignored, so a bad
unused field can never block an emergency stop.

### A2. `field` — required keys, types and ranges (when `enabled` is true)

- Required: `revision` (integer written without fraction or exponent, 1 … 9007199254740991);
  `issued`, `expires` (string, exactly `YYYY-MM-DDTHH:MM:SSZ`, a real UTC date with year 2000–9999,
  hour ≤ 23, minute ≤ 59, second ≤ 59); `enabled` (boolean); `headline`, `wordmark` (strings);
  `tagline` (array of 0–2 strings); `effects` (object with booleans `viewfinder` and `steam`);
  `link` (string); `qr` (object: string `text`, integer `size`, array `rows` of strings);
  `photo` (object: `webp` matching `[a-z0-9-]{1,64}\.webp`, `jpg` matching `[a-z0-9-]{1,64}\.jpg`,
  `sha256` object whose `webp` and `jpg` are exactly 64 lowercase hex characters, integer `w` 1–1600,
  integer `h` 1–1000, `focus` and `steamAt` arrays of exactly 2 numbers, `textBox` an array of
  exactly 4 numbers, every number 0–1, and `textBox` x + width ≤ 1 and y + height ≤ 1);
  `labels` (object with strings `link` and `qr`).
- Optional: `headlineEm` (string), `badge` (string).
- **(R19)** Numbers are parsed independently of the process locale (a host that calls setlocale
  with a decimal comma must not change any decision).
- A wrong JSON type for any known key is `field`, including a `qr.rows` element that is not a
  string and a `qr.size` that is not an integer token (a negative integer such as -37 is an
  integer: it passes `field` and fails `qr`). Unknown keys anywhere are ignored (they still
  count for `dupkey` and `depth`).

### A3. `text` — length counts Unicode code points

- `headline` 1–48; `headlineEm` (if present) 1–48 and a substring of `headline`; `wordmark` 1–12;
  each `tagline` part 1–18; `badge` (if present) 1–16; `labels.link` and `labels.qr` 1–80.
- **(R5)** Every character is one of: U+0020–U+007E, U+00A0–U+00AC, U+00AE–U+024F, and
  U+2013 – U+2014 — U+2018 ‘ U+2019 ’ U+201C “ U+201D ” U+2026 … U+2122 ™ (· U+00B7 and ® U+00AE
  are already in range). The design's U+0020–U+024F range contains the controls U+007F–U+009F and
  the invisible soft hyphen U+00AD; they are excluded.

### A4. `url` — `link` and `qr.text`

- At most 200 characters; **(R6)** every character is printable ASCII U+0021–U+007E (no space,
  control or non-ASCII character).
- Starts with exactly `https://`. The authority is the text after it up to the first `/`, `?` or
  `#` (or the end). The authority is a host, or a host followed by `:443`.
- Host: 2 or more labels joined by `.`, at most 253 characters; each label 1–63 characters of
  `A–Z a–z 0–9 -`, not starting or ending with `-`. **(R7)** The last label matches
  `[A-Za-z]{2,63}` or `xn--[A-Za-z0-9-]{1,59}`, which rules out IPv4 in every notation; IPv6
  literals and `user:password@` fail because `[`, `]` and `@` are not host characters.

### A5. Results

- `evaluate` → `{ action: "show" | "killswitch" | "reject", reason, banner, revision, sha256 }`.
  `banner` is the parsed object with the JSON field names (`null` on reject); `revision` is a
  number (`null` on reject); `sha256` is the lowercase hex SHA-256 of the whole signed file
  (`null` on reject).
- `keyIdOf(point65)` = the first 16 lowercase hex characters of SHA-256 over the 65-byte
  uncompressed point (0x04, X, Y).
- **(R14) A malformed context fails closed**, with the reason of the check that uses it: a public
  key that is not 65 bytes starting 0x04 → `sig`; `nowMs` not a finite number → `time`; `hwm` not a
  safe integer ≥ 0 → `revision`; `floor` not a safe integer ≥ 0 → `floor`; stored bytes neither
  absent (null/nullptr) nor bytes → `equal`. These are checked at the row that uses them, so a file
  that fails earlier keeps its own reason. In C++ the key is a pointer to the compiled 65-byte constant `kBannerPublicKey`;
  its length is the caller's contract (only null and the first byte can be checked there).

### A6. Photo check (`checkPhoto`, `CheckPhoto`) — no decoding

In order: `photo-size` (0 bytes, or more than 300 000 bytes — **(R8)** 300 KB = 300 000 bytes, while
16 KiB = 16 384 bytes); `photo-hash` (SHA-256 is not the named hash); `photo-dims` (the header cannot
be read, or its width and height differ from `w` and `h`); `photo-anim` (WebP only: any chunk with
FourCC `ANIM` or `ANMF`, or the VP8X animation flag 0x02 set).

- JPEG: start `FF D8`; walk the markers; the first SOFn (C0–C3, C5–C7, C9–CB, CD–CF) holds height
  then width, 16-bit big-endian, at offsets 5 and 7 from the marker; reaching SOS (`FF DA`) or the
  end first is `photo-dims`.
- WebP: `RIFF`, 32-bit size, `WEBP`, then chunks (FourCC, 32-bit little-endian size, data, one pad
  byte when the size is odd); a chunk running past the end is `photo-dims`. **(R15)** A missing pad
  byte after the last chunk is tolerated; any other bytes after the last chunk are `photo-dims`. Dimensions come from
  the first chunk: `VP8 ` (start code `9D 01 2A` at data offset 3; width = 16-bit LE at offset 6
  & 0x3FFF; height = 16-bit LE at offset 8 & 0x3FFF), `VP8L` (signature 0x2F at offset 0; then
  14 bits width − 1 and 14 bits height − 1, little-endian bit order), or `VP8X` (canvas width − 1 and
  height − 1 as 24-bit LE at data offsets 4 and 7). Every chunk is walked for the `ANIM`/`ANMF` check.
- `photo-decode` is not part of this check: the effect runner reports it when the platform decoder
  fails or decodes to a size other than `w` × `h`.

## B. Vectors (cases.json)

All cases use one throwaway key per generator run. Unless a case says otherwise: now
2026-11-02T12:00:00Z, issued 2026-11-02T00:00:00Z, expires 2027-11-02T00:00:00Z, revision 7,
hwm 1, floor 1, stored null, and the photo fields describe hero-coffee-800 (800 × 498).

| case | expect | reason | defect |
|---|---|---|---|
| valid | show | null | none |
| valid-killswitch | killswitch | null | `enabled` false, only the six fields |
| unknown-key | show | null | extra top-level `"future": {"x": [1]}` |
| max-text | show | null | every text at its maximum length, `headlineEm` inside `headline`, and the nine extra punctuation characters used across the strings |
| sig-bad | reject | sig | one signature byte flipped |
| sig-update-style | reject | sig | signed over the JSON bytes without the prefix |
| sigline-long | reject | sigline | line 1 is 101 bytes |
| sigline-missing | reject | sigline | no LF anywhere |
| size-over | reject | size | 16 385 bytes (a valid file padded with JSON whitespace, then signed) |
| utf8-bad | reject | utf8 | bytes C3 28 inside `headline`, signed |
| json-trailing | reject | json | `x` after the object, signed |
| json-dupkey | reject | dupkey | `headline` twice |
| json-depth4 | reject | depth | `"future": {"a": {"b": [1]}}` |
| schema-2 | reject | schema | `schema` 2 |
| keyid-wrong | reject | keyid | another 16-hex keyId |
| field-missing-headline | reject | field | no `headline` |
| sha-upper | reject | field | `photo.sha256.webp` in uppercase hex |
| text-headline-49 | reject | text | 49-character headline |
| text-bidi | reject | text | U+202E in `headline` |
| text-zero-width | reject | text | U+200B in `wordmark` |
| url-http | reject | url | `link` is `http://goatcam.dagoat.io` |
| url-userinfo | reject | url | `link` is `https://user:pass@goatcam.dagoat.io/` |
| url-ip | reject | url | `link` is `https://192.168.1.10/` |
| url-port | reject | url | `link` is `https://goatcam.dagoat.io:8443/` |
| qr-size-33 | reject | qr | `size` 33 |
| qr-row-short | reject | qr | one row of 36 characters |
| time-expired | reject | time | now = expires |
| time-issued-future | reject | time | issued = now + 2 days |
| time-window-401d | reject | time | expires − issued = 401 days |
| clock-future | reject | time | a valid file; now = expires + 730 days |
| clock-past | reject | time | a valid file; now = issued − 30 days |
| revision-below-hwm | reject | revision | hwm 8 |
| revision-below-floor | reject | floor | floor 8, hwm 1 |
| revision-equal-different | reject | equal | hwm 7; stored = a different valid revision-7 file |
| revision-equal-same | show | null | hwm 7; stored = this file |
| revision-equal-nostored | show | null | hwm 7; stored null (R3) |
| photo-ok-webp | show | null | photo hero-coffee-800.webp → `ok` |
| photo-ok-jpg | show | null | photo hero-coffee-800.jpg → `ok` |
| photo-hash | show | null | the WebP with one byte flipped → `photo-hash` |
| photo-dims | show | null | `h` 499 for the 800 × 498 WebP → `photo-dims` |
| photo-anim | show | null | a hand-built RIFF WEBP: VP8X (animation flag set, canvas = `w` × `h`), ANIM, ANMF → `photo-anim` |
| photo-size | show | null | 300 001 bytes whose true hash is named → `photo-size` |

Fix rounds after review added 47 boundary cases (JSON whitespace, the depth stop incl. depth32-malformed,
schema tokens, text/URL/field edges, ctx values, WebP/JPEG walker edges); their names and expectations are
in cases.json and vectors.test.mjs's required list, each derived from section A with one defect or boundary.

Domain separation, both directions: `vectors.test.mjs` also proves that the `valid` signature does
not verify over the JSON bytes alone (the update-manifest form).

## C. State machine (banner-machine.js, online_banner_machine.cpp)

### C1. Constants and state

H = 3 600 000 ms. Backoff by `failures` 0, 1, 2, 3 = 6 H, 6 H, 12 H, 24 H. Check while visible = 24 H.
Check-timer window = at least 60 000 ms and at most 24 H ahead.

Persisted `record` = `{ hwm, current: null | { revision, sha256, enabled, expiresMs, photos }, lastCheckMs, failures }`;
`photos` holds one hash per kind this client draws (`{}` for a kill switch). The empty record is
`{ hwm: floor, current: null, lastCheckMs: 0, failures: 0 }`.

Volatile: `started`, `visible`, `gen` (0 at create, +1 per attempt, never reset), `attempting`,
`phase` (`signed` | `photos` | `write-set` while attempting), `pending` (the SIGNED result of the
running attempt), `displayed` (`what`: `online` | `dagoat`, plus `sha256` and `expiresMs` for
online), `pendingSwap`, `pointerOver`, `focusInside`, `timerAtMs` (null = disarmed),
`lastMayFetch`, `setVerified`, `nowMs` (of the last event), `hwmHigh`.

### C2. Derived

- `showable(now)` = current ≠ null ∧ current.enabled ∧ current.revision = hwm ∧ now < current.expiresMs ∧ setVerified.
- `desired(now)` = showable ? online (current's sha256 and expiresMs) : dagoat.
- `due(now)` = now ≥ lastCheckMs + backoff[failures] ∨ **(R9)** lastCheckMs > now (the clock moved back).
- `timerTarget(now)` = null when not visible; otherwise the earlier of (a) when `displayed` is
  online: `displayed.expiresMs` and, **(R16)** when `current` is showable, `current.expiresMs` too
  (a pending replacement that expires first must swap at its own expiry) — exact, never clamped —
  and (b) when not attempting and
  `lastMayFetch` is true: lastCheckMs + 24 H, clamped into [now + 60 000, now + 24 H]. Null when
  neither applies. **(R10)** With `lastMayFetch` false no check timer is armed (no polling while a
  gate says no; the next show asks again).

### C3. Processing an event

Before START every event returns `[]`. Every other event runs these steps in order and returns the
effects in the order produced:

1. `nowMs` = now. For TIMER and RESUME, `timerAtMs` = null (the timer has fired).
2. **Expiry first** (every event except START and HIDDEN, only while visible): if `displayed` is
   online and now ≥ `displayed.expiresMs` → `swapNow()`.
3. The event's own step (C4).
4. **Display reconcile** (only while visible): compare `displayed` with `desired(now)`. Equal (same
   `what`, and for online the same sha256) → `pendingSwap` = false. Desired dagoat while online is
   displayed → `swapNow()`. Desired online (a different set, or dagoat displayed) → if
   `pointerOver` or `focusInside`: `pendingSwap` = true and no effect; otherwise emit
   `display {what: "online", immediate: false}`, `displayed` = desired, `pendingSwap` = false.
5. **Timer reconcile**: t = `timerTarget(now)`; if t ≠ `timerAtMs`, emit `arm-timer {atMs: t}`
   (or `disarm-timer` when t is null) and set `timerAtMs` = t.

`swapNow()` emits `display {what: desired.what, immediate: true}`, sets `displayed` = desired and
`pendingSwap` = false. A kill switch, an expiry or an invalid set is therefore never deferred; an
expired banner whose replacement is already committed swaps straight to that replacement.

### C4. Event steps

| event | step 3 |
|---|---|
| START {record, setValid} | **(R17)** Build the new state first and change nothing if building fails: record = the given record when it is an object, else (null or undefined) the empty record, which is not written. Then reset every volatile field except `gen`; `started` = true. hwm below floor → raised to floor. If current ≠ null and not (setValid ∧ current.revision = hwm ∧ now < current.expiresMs): current = null. **(R13)** If hwm was raised or current was cleared, emit one `write-record {record}`. `setVerified` = current ≠ null; `hwmHigh` = hwm. Emit `delete-unnamed-sets`. Steps 2, 4 and 5 do not run. |
| SHOWN {mayFetch} | `lastMayFetch` = mayFetch. If not visible: visible = true, emit `display {what: desired.what, immediate: true}`, `displayed` = desired. (A SHOWN while visible draws nothing.) Then if not attempting ∧ mayFetch ∧ due → start an attempt: gen + 1, attempting, phase `signed`, emit `fetch-signed {gen}`. |
| HIDDEN | Ignored when not visible. Else visible = false; if attempting: emit `cancel {gen}`, attempting = false, `pending` = null; `pointerOver` = `focusInside` = `pendingSwap` = false. |
| SIGNED {gen, result} | Ignored unless attempting, gen = current gen and phase `signed`. `reject` → fail. `killswitch` → pre-commit. `show` → `pending` = result, phase `photos`, emit `fetch-photos {gen, kinds}`. |
| PHOTOS {gen, ok} | Ignored unless attempting, gen matches and phase `photos`. ok false → fail; ok true → pre-commit. |
| pre-commit | expiresMs = the result's `banner.expires` as epoch ms. **(R17)** If not (now < expiresMs) — so an unparsable expiry fails — or result.revision < hwm, or a show result lacks the photo hash of a drawn kind → fail. Else `pending` = result (kill switch included), phase `write-set`, emit `write-set {gen, sha256: result.sha256}`. |
| COMMITTED {gen} | Ignored unless attempting, gen matches and phase `write-set` (the set is on disk). record = `{ hwm: revision, current: { revision, sha256, enabled: action is show, expiresMs, photos }, lastCheckMs: now, failures: 0 }`; `setVerified` = true; attempting = false; emit `write-record {record}`. |
| FAILED {gen} | Ignored unless attempting and gen matches (any phase). Else fail. |
| fail | record.lastCheckMs = now; record.failures = min(3, failures + 1); attempting = false; `pending` = null; emit `write-record {record}`. `current` and `hwm` do not change. |
| TIMER {mayFetch}, RESUME {mayFetch} | Ignored when not visible. `lastMayFetch` = mayFetch; if not attempting ∧ mayFetch ∧ due → start an attempt. |
| CLICK | Ignored when not visible. If step 2 swapped during this event → nothing more (an expired banner never opens). Else emit `open-link`. |
| POINTER {over} / FOCUS {inside} | Ignored when not visible. Set `pointerOver` / `focusInside`. |

**(R11) Commit point.** The record write is emitted only after COMMITTED reports the set on disk,
and the machine adopts the new record when it emits that write. A HIDDEN between `write-set` and
COMMITTED makes COMMITTED stale: no record write, the set is an orphan deleted at the next START. A
failed record write is not reported back; the next record write carries the machine's record, so
the stored `hwm` can lag the machine's but never exceeds it, and no write can lower it.

### C5. Invariants (`checkInvariants()` returns one message per broken rule; empty when all hold)

1. record.hwm ≥ floor. 2. record.hwm ≥ `hwmHigh`, and every event keeps `hwmHigh` = max(`hwmHigh`, record.hwm).
3. visible ∧ displayed online ⇒ nowMs < displayed.expiresMs ∧ showable(nowMs).
4. not visible ⇒ not attempting ∧ timerAtMs = null. 5. timerAtMs ≠ null ⇒ visible.
6. visible ∧ not attempting ∧ lastMayFetch ⇒ timerAtMs ≠ null ∧ timerAtMs ≤ nowMs + 24 H.
7. pendingSwap ⇒ visible ∧ (pointerOver ∨ focusInside) ∧ desired(nowMs) is online.
8. 0 ≤ record.failures ≤ 3. 9. attempting ⇔ phase is set.
10. **(R18)** visible ∧ not pendingSwap ⇒ `displayed` equals desired(nowMs) (same `what`, same sha256 for online).
11. **(R18)** visible ∧ not attempting ∧ lastMayFetch ∧ timerAtMs ≠ null ⇒ timerAtMs ≥ the earliest of nowMs + 60 000 and the expiries named in C2 (a).
12. **(R18)** not visible ⇒ not pointerOver ∧ not focusInside.

Each test suite also carries one negative control proving `checkInvariants()` can fail: a deliberately
broken copy of the machine (for example pre-commit without the hwm re-check) driven into a broken
state must report a message.

The machine tests also assert: an event other than START that arrives while not visible returns no
`fetch-signed`, `fetch-photos`, `write-set`, `write-record` or `arm-timer` effect.

### C6. `state()` and transitions.json

`state()` = `{ record, visible, gen, attempting, shown: displayed.what, pendingSwap, floor, timerAtMs }`
(a superset of the plan's list: the effect runner reads `floor`).

transitions.json: each scenario creates a fresh machine from its `floor` and `kinds`; its first
step is START; `atMs` is absolute epoch ms; `expect` is the exact effect list; a step may add
`"state"`, a partial object compared against `state()` (only the keys given; `record` compared
whole). SIGNED results carry `{ action, reason, revision, sha256, banner }`, where `banner` has at
least `expires` and, for show, `photo.sha256` — the machine reads nothing else. A scenario models a
restart by sending START again with the record the disk would hold.

## D. Effect runners (banner-fetch.js, online_banner_client.cpp)

- **(R12) Views.** The runner holds one visibility. `shown()` while already shown still asks the
  gate and dispatches SHOWN (the machine draws nothing and starts a check only if one is due and
  none runs), then renders the last model again; `hidden()` hides. Apps with several views
  aggregate them and call `shown()` for the first and `hidden()` after the last.
- One queue: control calls and async completions become events dispatched one at a time, in order.
- Startup: read the record (an unreadable or malformed record is null); for a named set, read it and
  re-verify: `evaluate(signed, {hwm: record.hwm, floor: state().floor, storedBytes: signed, …})` must
  return show or kill switch with the record's sha256 and revision, and for show every drawn photo
  must pass `checkPhoto`. **(R20)** Every cached field must also equal the signed file: `enabled` equals
  (action is show), `expiresMs` equals the signed `expires` in ms, and `photos` holds exactly the signed
  hashes of the drawn kinds (nothing for a kill switch); any difference makes `setValid` false (a tampered
  or stale cache is cleared, never repaired). Cached files are read with caps checked before allocation:
  record 16 KiB, signed file 16 384 bytes, each photo 300 000 bytes. That is `setValid`. Then START.
- **(R23) Record fields are bounded identically in every runner:** `hwm`, `current.revision` and
  `lastCheckMs` are safe integers (|n| ≤ 9007199254740991; hwm and revision ≥ 0, revision ≥ 1),
  `expiresMs` likewise, `failures` 0-3; anything else makes the record malformed (null). Machine time
  arithmetic never overflows (saturating adds in C++).
- **(R24) Production configuration:** the origin is exactly `https://<host>` with no path, query,
  fragment or credentials; plain http and a path prefix are allowed only behind an explicit test-only
  option. The C++ key is passed as a reference or pointer to a 65-byte array, so a shorter buffer cannot
  compile.
- **(R25) Timer arming failure:** if the host timer cannot be armed, the runner reports it and retries
  once by a fallback (a plain timer in JS; one posted retry in C++); every later event still runs the
  expiry-first step, so the first event after an expiry removes the banner.
- **(R26) Host callbacks fail visibly, never silently:** `setTimer` and `clearTimer` are required
  options; a `setTimer` that throws or returns false is a failed arm (R25 fallback). A `render` that throws
  is retried with the last model on the next control call. Only the drawn kinds are read from a stored
  set, and a cached `photos` map with any other key is invalid (R20 exactness). In C++, a worker completion
  that cannot be posted to the client's window is parked and dispatched by the UI thread on the next
  public call or WM_TIMER, so an attempt can never stay 'attempting' with nothing in flight; if the
  client's window cannot be created, the client starts no attempt.
- **(R22) Storage adapters** (the app-side `storage` object) return at most those caps — a larger stored
  value is returned as missing — and replace a set atomically (temporary name, then rename, or one
  atomic store write). The runner re-checks every returned length before parsing and treats a larger
  value as invalid; it cannot bound what an adapter allocates, so each app's adapter is tested for both.
- SHOWN, TIMER and RESUME carry `mayFetch` = the gate's answer (an error counts as false).
- Before every fetch, decode and write, the runner checks that the effect's gen is the machine's
  current gen, an attempt is running and the app is visible; otherwise it stops and sends nothing.
  A fetch aborted by `cancel` sends nothing; any other fetch failure sends FAILED (signed file) or
  PHOTOS ok false (photos); a failed decode (`photo-decode`) is PHOTOS ok false.
- `write-set` writes the set under its sha256 without damaging an existing set of that name
  (temporary name, then rename) and sends COMMITTED or FAILED; `write-record` errors are logged and
  not sent back (R11).
- `display` renders the record's current set for online, or the DaGoat banner; `open-link` opens
  the displayed banner's `link`, or `https://dagoat.io` for DaGoat.
