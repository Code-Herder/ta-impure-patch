/* undither-ui.js — the viewer widget around Undither.Renderer.
 *
 * Undither.mount(container, {base, images}) builds: an environment strip, the
 * three stage toggles, zoom controls, a drag-split before/after canvas and a
 * readout of the parameters restore.py measured for the current image.
 *
 * The same widget is used by the standalone prototype (tools/undither/app) and by
 * the wiki page, so it owns its own DOM and carries no page-specific assumptions.
 */
(function (global) {
  "use strict";

  var STAGES = [
    { key: "undither", label: "Undither", hint: "bilateral — recovers the colours the dither was spatially averaging; kernel in the row below" },
    { key: "deband", label: "Deband", hint: "blurs single-step palette contours, protects real edges" },
    { key: "enhance", label: "Enhance", hint: "procedural local contrast + vibrance — invents, does not recover" },
  ];

  // Mirrors restore.PRESETS. "spec" is the handover document's kernel; on TA frames
  // its 7×7 disc outvotes single-pixel speckle texture and flattens it, so the
  // viewer defaults to "tuned".
  var PRESETS = {
    spec:  { label: "Spec 7×7",    sigmaSpace: 1.8, passes: 2, k: 1.3, metric: "l1",    strength: 1,
             hint: "the handover document's kernel: σs 1.8 (7×7 disc), two passes — flattens painted speckle into mud" },
    tuned: { label: "Tuned 5×5",   sigmaSpace: 1.2, passes: 2, k: 1.3, metric: "l1",    strength: 1,
             hint: "σs 1.2 (5×5 disc): still resolves a period-2 dither, keeps 3 px+ texture" },
    tight: { label: "Tight 3×3",   sigmaSpace: 0.9, passes: 2, k: 1.3, metric: "l1",    strength: 1,
             hint: "σs 0.9 (3×3 cross): most texture kept, some grain left" },
    split: { label: "Luma ÷ chroma", sigmaSpace: 1.2, passes: 2, k: 1.3, metric: "split", strength: 1,
             hint: "5×5 with the range weight split — tight on luma (0.31 σr), loose on chroma (1.15 σr): off-hue flecks go, light/dark texture stays" },
  };
  var PARAM_KEYS = ["sigmaSpace", "passes", "k", "metric", "strength"];
  var KERNELS = [["3×3", 0.9], ["5×5", 1.2], ["7×7", 1.8]];

  function pick(o) { var r = {}; PARAM_KEYS.forEach(function (k) { r[k] = o[k]; }); return r; }
  function sameParams(a, b) { return PARAM_KEYS.every(function (k) { return a[k] === b[k]; }); }
  function kernelName(sigmaSpace) {
    return sigmaSpace >= 1.5 ? "7×7 disc" : sigmaSpace >= 1.05 ? "5×5 disc" : "3×3 cross";
  }

  function el(tag, cls, text) {
    var n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  }

  function mount(container, opts) {
    var base = opts.base || "";
    var images = opts.images || [];
    if (!images.length) { container.textContent = "no images"; return null; }

    container.classList.add("ud");
    var bar = el("div", "ud-bar");
    var stageWrap = el("div", "ud-group");
    stageWrap.appendChild(el("span", "ud-glabel", "Stages"));
    var state = { undither: true, deband: true, enhance: false, params: pick(PRESETS.tuned) };
    var buttons = {};
    STAGES.forEach(function (s) {
      var b = el("button", "ud-btn", s.label);
      b.type = "button";
      b.title = s.hint;
      b.setAttribute("aria-pressed", String(state[s.key]));
      b.classList.toggle("on", state[s.key]);
      b.addEventListener("click", function () {
        state[s.key] = !state[s.key];
        b.classList.toggle("on", state[s.key]);
        b.setAttribute("aria-pressed", String(state[s.key]));
        run();
      });
      buttons[s.key] = b;
      stageWrap.appendChild(b);
    });
    bar.appendChild(stageWrap);

    var zoomWrap = el("div", "ud-group");
    zoomWrap.appendChild(el("span", "ud-glabel", "Zoom"));
    var zoomBtns = [["Fit", "fit"], ["1:1", 1], ["2:1", 2], ["4:1", 4]];
    var zoomEls = {};
    zoomBtns.forEach(function (z) {
      var b = el("button", "ud-btn", z[0]);
      b.type = "button";
      b.addEventListener("click", function () { setZoom(z[1]); });
      zoomEls[z[0]] = b;
      zoomWrap.appendChild(b);
    });
    bar.appendChild(zoomWrap);

    var right = el("div", "ud-group ud-right");
    var fsBtn = el("button", "ud-btn", "Full screen");
    fsBtn.type = "button";
    fsBtn.addEventListener("click", function () {
      if (document.fullscreenElement) document.exitFullscreen();
      else container.requestFullscreen && container.requestFullscreen();
    });
    right.appendChild(fsBtn);
    bar.appendChild(right);
    container.appendChild(bar);

    // ---- second row: the undither kernel ------------------------------------
    var bar2 = el("div", "ud-bar ud-bar-2");
    var presetEls = {}, kernelEls = {}, metricEls = {}, passEls = {};
    function group(label) {
      var g = el("div", "ud-group");
      g.appendChild(el("span", "ud-glabel", label));
      bar2.appendChild(g);
      return g;
    }
    function small(parent, label, hint, onClick, map, key) {
      var b = el("button", "ud-btn ud-sm", label);
      b.type = "button";
      if (hint) b.title = hint;
      b.addEventListener("click", onClick);
      parent.appendChild(b);
      if (map) map[key] = b;
      return b;
    }
    function range(parent, min, max, step, onInput) {
      var r = el("input", "ud-range");
      r.type = "range"; r.min = min; r.max = max; r.step = step;
      r.addEventListener("input", function () { onInput(parseFloat(r.value)); });
      var v = el("span", "ud-val");
      parent.appendChild(r); parent.appendChild(v);
      return { input: r, value: v };
    }
    var gPreset = group("Preset");
    Object.keys(PRESETS).forEach(function (k) {
      small(gPreset, PRESETS[k].label, PRESETS[k].hint,
            function () { setParams(pick(PRESETS[k])); }, presetEls, k);
    });
    var gKernel = group("Kernel");
    KERNELS.forEach(function (kz) {
      small(gKernel, kz[0], "σs " + kz[1] + " → " + kernelName(kz[1]) + " (radius " + Math.max(1, Math.round(kz[1] * 1.5)) + ")",
            function () { setParams({ sigmaSpace: kz[1] }); }, kernelEls, String(kz[1]));
    });
    var gMetric = group("Metric");
    small(gMetric, "RGB L1", "cv2.bilateralFilter's colour distance: |Δr|+|Δg|+|Δb| against σr",
          function () { setParams({ metric: "l1" }); }, metricEls, "l1");
    small(gMetric, "Luma ÷ chroma", "luma difference against 0.31 σr, chroma difference against 1.15 σr",
          function () { setParams({ metric: "split" }); }, metricEls, "split");
    var kCtl = range(group("σr"), 0.5, 2.0, 0.05, function (v) { setParams({ k: v }); });
    kCtl.input.title = "σr = k·q — below the dither amplitude the filter keeps the checkerboard, above edge contrast it melts silhouettes";
    var gPass = group("Passes");
    [1, 2, 3].forEach(function (n) {
      small(gPass, String(n), null, function () { setParams({ passes: n }); }, passEls, String(n));
    });
    var sCtl = range(group("Strength"), 0, 1, 0.05, function (v) { setParams({ strength: v }); });
    sCtl.input.title = "blend between the original and the filtered frame";
    container.appendChild(bar2);

    function presetName() {
      var n = null;
      Object.keys(PRESETS).forEach(function (k) { if (sameParams(state.params, PRESETS[k])) n = k; });
      return n;
    }

    function setParams(patch, silent) {
      Object.assign(state.params, patch);
      var P = state.params, pn = presetName();
      Object.keys(presetEls).forEach(function (k) { presetEls[k].classList.toggle("on", k === pn); });
      Object.keys(kernelEls).forEach(function (k) { kernelEls[k].classList.toggle("on", parseFloat(k) === P.sigmaSpace); });
      Object.keys(metricEls).forEach(function (k) { metricEls[k].classList.toggle("on", k === P.metric); });
      Object.keys(passEls).forEach(function (k) { passEls[k].classList.toggle("on", parseInt(k, 10) === P.passes); });
      kCtl.input.value = P.k;
      sCtl.input.value = P.strength;
      var q = images[current] ? images[current].q : 0;
      kCtl.value.textContent = P.k.toFixed(2) + " q = " + (P.k * q).toFixed(1);
      sCtl.value.textContent = Math.round(P.strength * 100) + " %";
      if (!silent) run();
    }

    var stage = el("div", "ud-stage");
    var canvas = el("canvas", "ud-canvas");
    stage.appendChild(canvas);
    var tagBefore = el("div", "ud-tag ud-tag-l", "256-colour original");
    var tagAfter = el("div", "ud-tag ud-tag-r", "restored");
    stage.appendChild(tagBefore);
    stage.appendChild(tagAfter);
    var handle = el("div", "ud-handle");
    handle.innerHTML = '<span class="ud-knob" aria-hidden="true"></span>';
    stage.appendChild(handle);
    var spin = el("div", "ud-spin", "loading…");
    stage.appendChild(spin);
    container.appendChild(stage);

    var strip = el("div", "ud-strip");
    container.appendChild(strip);
    var readout = el("div", "ud-readout");
    container.appendChild(readout);

    var renderer;
    try {
      renderer = new global.Undither.Renderer(canvas);
    } catch (e) {
      stage.innerHTML = '<p class="ud-error">' + e.message + "</p>";
      return null;
    }

    var current = 0, zoomMode = 1, dragging = null;

    function resize() {
      var dpr = Math.min(global.devicePixelRatio || 1, 2);
      var w = Math.max(320, Math.round(stage.clientWidth * dpr));
      var h = Math.max(180, Math.round(stage.clientHeight * dpr));
      if (canvas.width !== w || canvas.height !== h) {
        canvas.width = w; canvas.height = h;
        setZoom(zoomMode, true);
      }
      draw();
    }

    function setZoom(mode, keepCentre) {
      zoomMode = mode;
      Object.keys(zoomEls).forEach(function (k) {
        var v = k === "Fit" ? "fit" : parseInt(k, 10);
        zoomEls[k].classList.toggle("on", v === mode || (mode === "fit" && k === "Fit"));
      });
      if (!renderer.src) return;
      var dpr = Math.min(global.devicePixelRatio || 1, 2);
      var z = mode === "fit" ? renderer.fitZoom() : mode * dpr;
      renderer.centre(z);
      if (!keepCentre) draw();
    }

    function draw() { renderer.draw(); placeHandle(); }

    function placeHandle() {
      handle.style.left = (renderer.view.split * 100) + "%";
    }

    function run() {
      if (!renderer.src) return;
      renderer.process(state);
      draw();
      updateReadout();
    }

    function fmt(v, d) { return (v == null || isNaN(v)) ? "—" : Number(v).toFixed(d == null ? 1 : d); }

    function qaText(v) {
      return "edge retention " + fmt(v.edge_retention, 2) +
        " · HF reduction " + fmt(v.hf_energy_reduction, 2) +
        " · colour shift " + fmt(v.mean_color_shift, 2);
    }

    function updateReadout() {
      var m = images[current], P = state.params, pn = presetName();
      var on = STAGES.filter(function (s) { return state[s.key]; }).map(function (s) { return s.label; });
      var nearest = global.Undither.presetFor(P);
      var v = m.variants && m.variants[nearest];
      var step = state.undither ? (v ? v.step_after : m.step_undithered) : m.step;
      var sr = P.k * m.q;
      var kernel = kernelName(P.sigmaSpace) + " (σ<sub>s</sub> " + P.sigmaSpace + ") · " +
        P.passes + (P.passes === 1 ? " pass" : " passes") + " · " +
        (P.metric === "split"
          ? "luma σ " + fmt(global.Undither.SPLIT_Y_FRAC * sr) + " / chroma σ " + fmt(global.Undither.SPLIT_C_FRAC * sr)
          : "RGB L1 σ<sub>r</sub> " + fmt(sr)) +
        (P.strength < 1 ? " · strength " + Math.round(P.strength * 100) + " %" : "");
      var approx = pn ? "" : " (nearest measured kernel: " + PRESETS[nearest].label + ")";
      var rows = [
        ["Map", m.map + " · " + m.expansion],
        ["Source", m.width + "×" + m.height + " indexed · " + m.distinct_colours.toLocaleString() + " distinct colours"],
        ["q (dither amplitude)", fmt(m.q) + "  →  σ<sub>r</sub> = " + P.k.toFixed(2) + " q = " + fmt(sr)],
        ["Undither kernel", (pn ? PRESETS[pn].label : "custom") + " — " + kernel],
        ["lag-1 autocorrelation", fmt(m.lag1_autocorr, 2) + (m.dither_present ? "  — dither detected" : "  — detector silent")],
        ["band step", fmt(step) + (state.undither ? " (measured after undithering" + approx + ")" : "")],
        ["restore.py verdict", "undither=" + m.decisions.undither + " · deband=" + m.decisions.deband + " · " + qaText(m.qa)],
        ["This kernel, forced", v ? "deband=" + v.deband + " · " + qaText(v.qa) + approx : "—"],
        ["Showing", on.length ? on.join(" → ") : "original on both sides"],
      ];
      readout.innerHTML = rows.map(function (r) {
        return '<div class="ud-row"><span class="ud-k">' + r[0] + '</span><span class="ud-v">' + r[1] + "</span></div>";
      }).join("");
      tagAfter.textContent = on.length
        ? on.join(" + ") + (state.undither ? " · " + (pn ? PRESETS[pn].label : "custom kernel") : "")
        : "unchanged";
    }

    function load(i) {
      current = i;
      Array.prototype.forEach.call(strip.children, function (c, n) {
        c.classList.toggle("on", n === i);
      });
      var m = images[i];
      spin.style.display = "";
      var img = new Image();
      img.onload = function () {
        renderer.setImage(img, m);
        setZoom(zoomMode, true);
        setParams({}, true);            // refresh the σr readout for this image's q
        run();
        spin.style.display = "none";
      };
      img.onerror = function () { spin.textContent = "could not load " + m.file; };
      img.src = base + "shots/" + m.file;
    }

    images.forEach(function (m, i) {
      var b = el("button", "ud-thumb");
      b.type = "button";
      b.title = m.map;
      var im = el("img");
      im.src = base + (m.thumb || "shots/" + m.file);
      im.alt = m.env;
      im.loading = "lazy";
      b.appendChild(im);
      b.appendChild(el("span", null, m.env));
      b.addEventListener("click", function () { load(i); });
      strip.appendChild(b);
    });

    // ---- pointer: near the seam drags the split, elsewhere pans -------------
    function canvasPos(ev) {
      var r = canvas.getBoundingClientRect();
      return { x: ev.clientX - r.left, y: ev.clientY - r.top, w: r.width, h: r.height };
    }

    stage.addEventListener("pointerdown", function (ev) {
      var p = canvasPos(ev);
      var seamX = renderer.view.split * p.w;
      dragging = Math.abs(p.x - seamX) < 22 ? { mode: "split" }
        : { mode: "pan", x: ev.clientX, y: ev.clientY,
            panX: renderer.view.panX, panY: renderer.view.panY };
      stage.setPointerCapture(ev.pointerId);
      stage.classList.add(dragging.mode === "split" ? "ud-dragging-split" : "ud-dragging-pan");
      if (dragging.mode === "split") {
        renderer.view.split = Math.min(1, Math.max(0, p.x / p.w));
        draw();
      }
      ev.preventDefault();
    });

    stage.addEventListener("pointermove", function (ev) {
      var p = canvasPos(ev);
      if (!dragging) {
        var seamX = renderer.view.split * p.w;
        stage.classList.toggle("ud-near-seam", Math.abs(p.x - seamX) < 22);
        return;
      }
      if (dragging.mode === "split") {
        renderer.view.split = Math.min(1, Math.max(0, p.x / p.w));
      } else {
        var dpr = Math.min(global.devicePixelRatio || 1, 2);
        renderer.view.panX = dragging.panX - (ev.clientX - dragging.x) * dpr / renderer.view.zoom;
        renderer.view.panY = dragging.panY - (ev.clientY - dragging.y) * dpr / renderer.view.zoom;
      }
      draw();
    });

    function endDrag(ev) {
      if (!dragging) return;
      dragging = null;
      stage.classList.remove("ud-dragging-split", "ud-dragging-pan");
      try { stage.releasePointerCapture(ev.pointerId); } catch (e) {}
    }
    stage.addEventListener("pointerup", endDrag);
    stage.addEventListener("pointercancel", endDrag);

    stage.addEventListener("wheel", function (ev) {
      if (!renderer.src) return;
      ev.preventDefault();
      var p = canvasPos(ev);
      var dpr = Math.min(global.devicePixelRatio || 1, 2);
      var before = { x: renderer.view.panX + p.x * dpr / renderer.view.zoom,
                     y: renderer.view.panY + p.y * dpr / renderer.view.zoom };
      var z = renderer.view.zoom * Math.pow(1.0015, -ev.deltaY);
      renderer.view.zoom = Math.min(16, Math.max(renderer.fitZoom() * 0.5, z));
      renderer.view.panX = before.x - p.x * dpr / renderer.view.zoom;
      renderer.view.panY = before.y - p.y * dpr / renderer.view.zoom;
      zoomMode = null;
      Object.keys(zoomEls).forEach(function (k) { zoomEls[k].classList.remove("on"); });
      draw();
    }, { passive: false });

    container.tabIndex = 0;
    container.addEventListener("keydown", function (ev) {
      var i = "123".indexOf(ev.key);
      if (i >= 0) { buttons[STAGES[i].key].click(); ev.preventDefault(); }
      else if (ev.key === "f") { setZoom("fit"); }
      else if (ev.key === "0") { setZoom(1); }
      else if (ev.key === "ArrowLeft") { load((current + images.length - 1) % images.length); }
      else if (ev.key === "ArrowRight") { load((current + 1) % images.length); }
    });

    if (global.ResizeObserver) new ResizeObserver(resize).observe(stage);
    global.addEventListener("resize", resize);
    document.addEventListener("fullscreenchange", function () { setTimeout(resize, 50); });

    resize();
    setParams({}, true);
    load(0);
    return { renderer: renderer, state: state, load: load, run: run,
             setSplit: function (v) { renderer.view.split = v; draw(); },
             setZoom: setZoom, setParams: setParams, presetName: presetName };
  }

  global.Undither = global.Undither || {};
  global.Undither.mount = mount;
  global.Undither.STAGES = STAGES;
  global.Undither.PRESETS = PRESETS;
})(window);
