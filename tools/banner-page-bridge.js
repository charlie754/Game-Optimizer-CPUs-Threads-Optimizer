(function () {
    var host = document.getElementById('h');
    var mount = host && host.shadowRoot
        ? host.shadowRoot.querySelector('[data-goat-banner]')
        : document.querySelector('[data-goat-banner]');
    if (!mount) return;

    var hovered = false;
    var focused = false;
    var pending = null;

    function postPointer(over) {
        if (window.chrome && window.chrome.webview && window.chrome.webview.postMessage) {
            window.chrome.webview.postMessage({goatcamBanner: 'pointer', over: !!over});
        }
    }

    function postFocus(inside) {
        if (window.chrome && window.chrome.webview && window.chrome.webview.postMessage) {
            window.chrome.webview.postMessage({goatcamBanner: 'focus', inside: !!inside});
        }
    }

    function syncActive(plate) {
        if (hovered || focused) plate.classList.add('is-active');
        else plate.classList.remove('is-active');
    }

    function adaptPanel(root) {
        var panels = [];
        if (root.classList && root.classList.contains('goat--panel')) panels.push(root);
        var nested = root.querySelectorAll ? root.querySelectorAll('.goat--panel') : [];
        for (var i = 0; i < nested.length; ++i) panels.push(nested[i]);
        if (panels.length !== 1) throw new Error('expected exactly one goat--panel');
        panels[0].classList.remove('goat--panel');
        panels[0].classList.add('goat--strip');
        return panels[0];
    }

    function bindBannerEvents(root) {
        var plate = root.querySelector('.goat-lockup');
        if (!plate) return;
        plate.addEventListener('pointerenter', function () {
            hovered = true;
            syncActive(plate);
            postPointer(true);
        });
        plate.addEventListener('pointerleave', function () {
            hovered = false;
            syncActive(plate);
            postPointer(false);
            flushPending();
        });
        plate.addEventListener('focusin', function () {
            focused = true;
            syncActive(plate);
            postFocus(true);
        });
        plate.addEventListener('focusout', function (event) {
            if (event.relatedTarget && plate.contains(event.relatedTarget)) return;
            focused = false;
            syncActive(plate);
            postFocus(false);
            flushPending();
        });
    }

    function renderNow(model) {
        pending = null;
        renderGoatBanner(mount, model);
        var root = mount.firstElementChild;
        if (!root) return;
        adaptPanel(root);
        goatcamQrBridge(root);
        bindBannerEvents(root);
    }

    function flushPending() {
        if (!hovered && !focused && pending !== null) renderNow(pending);
    }

    function apply(model) {
        if (!model || typeof model !== 'object') return;
        if (hovered || focused) {
            pending = model;
            return;
        }
        renderNow(model);
    }

    window.__goatcamBannerApply = apply;
    if (window.chrome && window.chrome.webview && window.chrome.webview.addEventListener) {
        window.chrome.webview.addEventListener('message', function (event) {
            var data = event && event.data;
            if (!data || typeof data !== 'object' || !data.goatcamBanner ||
                typeof data.goatcamBanner !== 'object') return;
            apply(data.goatcamBanner);
        });
    }
    renderNow({kind: 'dagoat'});
}());
