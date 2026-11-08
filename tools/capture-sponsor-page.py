"""Headless captures and geometry checks for the generated WebView2 sponsor page."""

import argparse
import json
import os
import re
import struct
import subprocess
import tempfile
from pathlib import Path


BROWSERS = [
    os.path.expandvars(r"%LOCALAPPDATA%\ms-playwright\chromium_headless_shell-1234"
                       r"\chrome-headless-shell-win64\chrome-headless-shell.exe"),
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
]


def browser_path() -> str:
    for candidate in BROWSERS:
        if os.path.exists(candidate):
            return candidate
    raise SystemExit("no headless browser found; tried:\n  " + "\n  ".join(BROWSERS))


def png_size(path: Path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return struct.unpack(">II", data[16:24])


def probe_script(model, hover: bool) -> str:
    model_js = json.dumps(model, ensure_ascii=False, separators=(",", ":")).replace("<", "\\u003c")
    return """
<script>
(function () {
  var model = %s;
  var targetHover = %s;
    var host = document.getElementById('h');
    var shadow = host && host.shadowRoot;
  function box(e) { var r = e.getBoundingClientRect(); return {left:r.left, top:r.top, right:r.right, bottom:r.bottom, width:r.width, height:r.height}; }
  function contentBox(e) { var r = document.createRange(); r.selectNodeContents(e); return box(r); }
  function textLines(e) {
    var lines = [''];
    if (!e) return lines;
    for (var i = 0; i < e.childNodes.length; ++i) {
      var node = e.childNodes[i];
      if (node.nodeType === Node.ELEMENT_NODE && node.tagName === 'BR') lines.push('');
      else lines[lines.length - 1] += node.textContent || '';
    }
    return lines;
  }
  function inside(a, b) { return a.left >= b.left - .5 && a.top >= b.top - .5 && a.right <= b.right + .5 && a.bottom <= b.bottom + .5; }
  function insideWithMargin(a, b, margin) {
    return !!a && !!b && a.left >= b.left + margin - .01 &&
      a.top >= b.top + margin - .01 && a.right <= b.right - margin + .01 &&
      a.bottom <= b.bottom - margin + .01;
  }
  function lockupElementsInside(plate, plateBox) {
    var nodes = plate ? plate.querySelectorAll('.goat-lockup__meteors, .goat-lockup__mark, .goat-lockup__word') : [];
    var failures = [];
    for (var n = 0; n < nodes.length; ++n) {
      var r = box(nodes[n]);
      if (!inside(r, plateBox)) {
        failures.push(String(nodes[n].tagName || 'element').toLowerCase() +
          (typeof nodes[n].className === 'string' && nodes[n].className ? '.' + nodes[n].className.replace(/ +/g, '.') : ''));
      }
    }
    return { pass: nodes.length > 0 && failures.length === 0, failures: failures.slice(0, 8) };
  }
  function intersects(a, b) { return a.right >= b.left && a.left <= b.right && a.bottom >= b.top && a.top <= b.bottom; }
  function result() {
    var root = shadow && shadow.querySelector('.goat--strip,.goat--panel');
    var plate = root && root.querySelector('.goat-lockup');
    var photoImage = plate && plate.querySelector('.gc-photo__img');
    var stage = plate && plate.querySelector('.gc-stage');
    var iphone = plate && plate.querySelector('.gc-iphone');
    var word = plate && plate.querySelector('.goat-lockup__word');
    var qr = plate && plate.querySelector('.gc-qr');
    var vf = plate && plate.querySelector('.gc-vf');
    var restStage = null, hoverStage = null;
    var restPlateBox = null, hoverPlateBox = null;
    var restQrBox = null, hoverQrBox = null, restVfBox = null, hoverVfBox = null;
    var dagoatRest = { pass: true, failures: [] }, dagoatHover = { pass: true, failures: [] };
    if (plate) {
      plate.classList.remove('is-active');
      restPlateBox = box(plate);
      restQrBox = qr ? box(qr) : null;
      restVfBox = vf ? box(vf) : null;
      restStage = stage ? box(stage) : null;
      if (model.kind === 'dagoat') dagoatRest = lockupElementsInside(plate, restPlateBox);
      plate.classList.add('is-active');
      hoverPlateBox = box(plate);
      hoverQrBox = qr ? box(qr) : null;
      hoverVfBox = vf ? box(vf) : null;
      hoverStage = stage ? box(stage) : null;
      if (model.kind === 'dagoat') dagoatHover = lockupElementsInside(plate, hoverPlateBox);
      if (!targetHover) plate.classList.remove('is-active');
    }
    var plateBox = plate ? box(plate) : null;
    var stages = plate ? plate.querySelectorAll('.gc-stage') : [];
    var stagePass = true;
    for (var i = 0; i < stages.length; ++i) {
      stagePass = stagePass && stages[i].parentElement === plate && intersects(box(stages[i]), plateBox);
    }
    var phoneGapText = iphone && word ? box(iphone).left - box(word).right : null;
    var phoneGapQr = iphone && qr ? box(qr).left - box(iphone).right : null;
    var clearPass = iphone && word && qr && phoneGapText >= 6 && phoneGapQr >= 6;
    var qrViewfinderRestMargin = qr && vf ? Math.min(
      restQrBox.left - restVfBox.left, restQrBox.top - restVfBox.top,
      restVfBox.right - restQrBox.right, restVfBox.bottom - restQrBox.bottom) : null;
    var qrViewfinderHoverMargin = qr && vf ? Math.min(
      hoverQrBox.left - hoverVfBox.left, hoverQrBox.top - hoverVfBox.top,
      hoverVfBox.right - hoverQrBox.right, hoverVfBox.bottom - hoverQrBox.bottom) : null;
    var qrViewfinderPass = model.kind !== 'online' ||
      (!!qr && !!vf && insideWithMargin(restQrBox, restVfBox, 4) &&
       insideWithMargin(hoverQrBox, hoverVfBox, 4));
    var headline = root && root.querySelector('.goat__line');
    var headlineRows = textLines(headline);
    var headlineContentBox = headline ? contentBox(headline) : null;
    var headlineSource = model.banner ? String(model.banner.headline || '') : '';
    var dash = headlineSource.indexOf(' — ');
    var expectedHeadlineRows = dash >= 0
      ? [headlineSource.slice(0, dash + 3), headlineSource.slice(dash + 3)]
      : [headlineSource];
    var headlinePass = model.kind !== 'online' ||
      (headlineRows.length === 2 && headlineRows[0] === expectedHeadlineRows[0] && headlineRows[1] === expectedHeadlineRows[1]);
    var headlineVisible = model.kind !== 'online' || !headline || !plate ||
      (headlineContentBox.left >= box(root).left - .5 && headlineContentBox.right <= box(plate).left + .5);
    var tagline = word && word.querySelector('.goat-lockup__tag');
    var taglineRows = tagline ? Array.prototype.slice.call(tagline.children).map(function (e) { return e.textContent; }) : [];
    var expectedTagline = model.banner && Array.isArray(model.banner.tagline) ? model.banner.tagline : [];
    var taglinePass = model.kind !== 'online' ||
      (taglineRows.length === expectedTagline.length && taglineRows.every(function (v, i) { return v === expectedTagline[i]; }));
    var out = {
      mode: model.kind,
      state: targetHover ? 'hover' : 'rest',
      root: !!root,
      rootClass: root ? root.className : null,
      plate: !!plate,
      stages: stages.length,
      stagePass: stagePass,
      plateInside: !plate || inside(plateBox, box(host)),
      qrInside: !qr || inside(restQrBox, restPlateBox),
      viewfinderInside: !vf || inside(restVfBox, restPlateBox),
      qrViewfinderInside: !qr || !vf || inside(restQrBox, restVfBox),
      qrViewfinderPass: qrViewfinderPass,
      qrViewfinderRestMargin: qrViewfinderRestMargin,
      qrViewfinderHoverMargin: qrViewfinderHoverMargin,
      dagoatRestInside: model.kind !== 'dagoat' || dagoatRest.pass,
      dagoatHoverInside: model.kind !== 'dagoat' || dagoatHover.pass,
      dagoatFailures: (dagoatRest.failures || []).concat(dagoatHover.failures || []).slice(0, 8),
      iphoneTextGap: phoneGapText,
      iphoneQrGap: phoneGapQr,
      headlineRows: headlineRows,
      headlinePass: headlinePass,
      headlineContentBox: headlineContentBox,
      headlineVisible: headlineVisible,
      taglineRows: taglineRows,
      taglinePass: taglinePass,
      photoNaturalWidth: photoImage ? photoImage.naturalWidth : 0,
      photoDecoded: model.kind !== 'online' || (!!photoImage && photoImage.naturalWidth > 0),
      iphoneBox: iphone ? box(iphone) : null,
      wordBox: word ? box(word) : null,
      qrBox: qr ? box(qr) : null,
      vfBox: vf ? box(vf) : null,
      clearPass: model.kind !== 'online' || !!clearPass,
      restStage: restStage,
      hoverStage: hoverStage,
      stageGrew: !stage || (hoverStage.width > restStage.width && hoverStage.height >= restStage.height)
    };
    var marker = document.createElement('pre');
    marker.id = 'capture-result';
    marker.textContent = JSON.stringify(out);
    // Keep the measurement in --dump-dom, but never let the probe become part of the PNG.
    marker.style.display = 'none';
    document.body.appendChild(marker);
  }
  if (window.__goatcamBannerApply) window.__goatcamBannerApply(model);
  if (shadow) {
    var motion = document.createElement('style');
    motion.textContent = '* { animation: none !important; transition: none !important; }';
    shadow.appendChild(motion);
  }
  setTimeout(result, 900);
}());
</script>
""" % (model_js, "true" if hover else "false")


def run_one(browser: str, page: str, model, hover: bool, dsf: float, host_width: int, output: Path):
    html = page + probe_script(model, hover)
    with tempfile.NamedTemporaryFile("w", suffix=".html", prefix="goatcam-page-",
                                     encoding="utf-8", delete=False) as handle:
        handle.write(html)
        temp = Path(handle.name)
    try:
        uri = "file:///" + temp.as_posix()
        cmd = [browser, "--headless", "--disable-gpu", "--no-sandbox",
               "--force-device-scale-factor=%s" % dsf, "--window-size=%d,110" % host_width,
               "--virtual-time-budget=1200",
               "--screenshot=%s" % output, "--dump-dom", uri]
        completed = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                                   errors="replace", timeout=120)
        dom = completed.stdout or ""
        match = re.search(r'<pre id="capture-result"[^>]*>([^<]*)</pre>', dom)
        if completed.returncode != 0 or not match:
            raise RuntimeError((completed.stderr or dom)[-2000:])
        data = json.loads(match.group(1))
        dimensions = png_size(output)
        print("capture mode=%s state=%s dsf=%s host=%d file=%s size=%sx%s bytes=%d geometry=%s"
              % (data["mode"], data["state"], dsf, host_width, output, dimensions[0], dimensions[1],
                 output.stat().st_size,
                 "PASS" if data["root"] and data["plate"] and data["stagePass"] and
                 data["plateInside"] and data["qrInside"] and data["viewfinderInside"] and
                 data["qrViewfinderInside"] and
                 data["qrViewfinderPass"] and data["dagoatRestInside"] and data["dagoatHoverInside"] and
                 data["clearPass"] and data["stageGrew"] and data["headlinePass"] and
                 data["headlineVisible"] and data["photoDecoded"] and
                 data["taglinePass"] else "FAIL"))
        print("  root=%s class=%s stage rest=%s hover=%s grew=%s iphone-text-gap=%s iphone-qr-gap=%s headline=%s visible=%s headline-box=%s tagline=%s"
              % (data["root"], data["rootClass"], data["restStage"], data["hoverStage"],
                 data["stageGrew"], data["iphoneTextGap"], data["iphoneQrGap"],
                 data["headlinePass"], data["headlineVisible"], data["headlineContentBox"], data["taglinePass"]))
        print("  plateInside=%s qrInside=%s viewfinderInside=%s qrViewfinderInside=%s stagePass=%s clearPass=%s"
              % (data["plateInside"], data["qrInside"], data["viewfinderInside"],
                 data["qrViewfinderInside"], data["stagePass"], data["clearPass"]))
        print("  qr-viewfinder-margin rest=%s hover=%s pass=%s dagoat-rest=%s dagoat-hover=%s failures=%s" %
              (data["qrViewfinderRestMargin"], data["qrViewfinderHoverMargin"],
              data["qrViewfinderPass"], data["dagoatRestInside"], data["dagoatHoverInside"],
              data["dagoatFailures"]))
        if data["mode"] == "online":
            print("  photo naturalWidth=%s decoded=%s" %
                  (data["photoNaturalWidth"], data["photoDecoded"]))
        if data["mode"] == "online":
            print("  boxes iphone=%s word=%s qr=%s vf=%s" %
                  (data["iphoneBox"], data["wordBox"], data["qrBox"], data["vfBox"]))
        if not (data["root"] and data["plate"] and data["stagePass"] and data["plateInside"] and
                data["qrInside"] and data["viewfinderInside"] and data["qrViewfinderInside"] and
                data["qrViewfinderPass"] and data["dagoatRestInside"] and data["dagoatHoverInside"] and
                data["clearPass"] and data["stageGrew"] and data["headlinePass"] and
                data["headlineVisible"] and data["photoDecoded"] and
                data["taglinePass"]):
            raise RuntimeError("page geometry check failed")
    finally:
        temp.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--page", default="build/panel/panel-app.html", type=Path)
    parser.add_argument("--model", type=Path)
    parser.add_argument("--message-json", type=Path,
                        help="C++ PageMessageJson output; its goatcamBanner value is applied")
    parser.add_argument("--out-dir", default="build/capture-page", type=Path)
    parser.add_argument("--host-width", nargs="+", default=[848, 976], type=int)
    args = parser.parse_args()
    page = args.page.read_text(encoding="utf-8")
    if args.model and args.message_json:
        parser.error("--model and --message-json are mutually exclusive")
    if args.message_json:
        message = json.loads(args.message_json.read_text(encoding="utf-8"))
        online = message.get("goatcamBanner") if isinstance(message, dict) else None
        if not isinstance(online, dict):
            raise SystemExit("message JSON has no object goatcamBanner value")
    else:
        online = json.loads(args.model.read_text(encoding="utf-8")) if args.model else {"kind": "dagoat"}
    browser = browser_path()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    print("browser: %s" % browser)
    for kind, model in (("dagoat", {"kind": "dagoat"}), ("online", online)):
        for hover in (False, True):
            for dsf in (1, 1.5, 2):
                for host_width in args.host_width:
                    output = args.out_dir / ("w%d-%s-%s-dsf%s.png" %
                                             (host_width, kind, "hover" if hover else "rest",
                                              str(dsf).replace(".", "p")))
                    run_one(browser, page, model, hover, dsf, host_width, output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
