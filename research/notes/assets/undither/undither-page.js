/* undither-page.js — wires the wiki page: mounts the before/after viewer and
   builds the lightboxed gallery of the fifteen environment captures. */
(function () {
  "use strict";

  var BASE = "assets/undither/";

  function lightbox(images) {
    var box = document.createElement("div");
    box.className = "ud-lightbox";
    box.innerHTML =
      '<button class="ud-lbclose" type="button" aria-label="Close">×</button>' +
      '<button class="ud-lbnav prev" type="button" aria-label="Previous">‹</button>' +
      '<button class="ud-lbnav next" type="button" aria-label="Next">›</button>' +
      '<img alt=""><figcaption></figcaption>';
    document.body.appendChild(box);
    var img = box.querySelector("img"), cap = box.querySelector("figcaption"), at = 0;

    function show(i) {
      at = (i + images.length) % images.length;
      var m = images[at];
      img.src = BASE + "shots/" + m.file;
      img.alt = m.env + " — " + m.map;
      cap.textContent = m.env + " · " + m.map + " (" + m.expansion + ") · " +
        m.width + "×" + m.height + " · " + m.distinct_colours + " distinct colours · " +
        "q " + m.q + " · band step " + m.step + " — click the image for 1:1";
      box.classList.remove("zoom1");
      box.classList.add("open");
    }
    function close() { box.classList.remove("open", "zoom1"); img.src = ""; }

    box.querySelector(".ud-lbclose").addEventListener("click", close);
    img.addEventListener("click", function (e) {   // fit <-> 1:1, not close
      e.stopPropagation();
      box.classList.toggle("zoom1");
    });
    box.querySelector(".prev").addEventListener("click", function (e) { e.stopPropagation(); show(at - 1); });
    box.querySelector(".next").addEventListener("click", function (e) { e.stopPropagation(); show(at + 1); });
    box.addEventListener("click", function (e) { if (e.target === box) close(); });
    document.addEventListener("keydown", function (e) {
      if (!box.classList.contains("open")) return;
      if (e.key === "Escape") { if (box.classList.contains("zoom1")) box.classList.remove("zoom1"); else close(); }
      else if (e.key === "ArrowLeft") show(at - 1);
      else if (e.key === "ArrowRight") show(at + 1);
    });
    return show;
  }

  function gallery(host, images, open) {
    host.className = "ud-gallery";
    images.forEach(function (m, i) {
      var fig = document.createElement("figure");
      var btn = document.createElement("button");
      btn.type = "button";
      btn.setAttribute("aria-label", "Open " + m.env + " full size");
      var im = document.createElement("img");
      im.src = BASE + (m.thumb || "shots/" + m.file);
      im.alt = m.env + " — " + m.map;
      im.loading = "lazy";
      btn.appendChild(im);
      btn.addEventListener("click", function () { open(i); });
      var cap = document.createElement("figcaption");
      cap.innerHTML = "<b>" + m.env + "</b><br>" + m.map;
      fig.appendChild(btn);
      fig.appendChild(cap);
      host.appendChild(fig);
    });
  }

  document.addEventListener("DOMContentLoaded", function () {
    var viewer = document.getElementById("undither-viewer");
    var gal = document.getElementById("undither-gallery");
    fetch(BASE + "shots.json").then(function (r) { return r.json(); }).then(function (images) {
      if (viewer && window.Undither && window.Undither.mount) {
        window.UD = window.Undither.mount(viewer, { base: BASE, images: images });
      }
      if (gal) gallery(gal, images, lightbox(images));
    }).catch(function (e) {
      if (viewer) viewer.innerHTML = '<p class="ud-error">could not load ' + BASE + "shots.json (" + e + ")</p>";
    });
  });
})();
