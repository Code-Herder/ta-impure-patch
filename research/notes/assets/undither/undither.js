/* undither.js — WebGL2 port of tools/undither/restore.py, stage for stage.
 *
 * restore.py is the reference implementation; this runs the same three stages on
 * the GPU so each one can be toggled interactively:
 *
 *   1. undither  bilateral, sigmaColor = k*q; kernel (sigmaSpace, passes, range
 *                metric, strength) selectable live — restore.py's PRESETS mirrored
 *                in undither-ui.js
 *   2. deband    single-step contours blurred, stronger edges protected
 *   3. enhance   procedural local contrast + saturation-aware vibrance
 *
 * q and the band step are measured offline by tools/undither/prep.py (which calls
 * restore.py's own analyze()/band_step()) and handed in per image, exactly as
 * restore.py derives them — nothing here re-invents a parameter.  The band step
 * after undithering (deband's gate) depends on the kernel, so prep.py measures it
 * once per preset and the renderer picks the nearest preset for custom settings.
 *
 * Deliberate fidelity notes:
 *  - OpenCV's bilateralFilter scores colour distance as the L1 sum of the three
 *    channel differences, not the Euclidean norm, so the shader does too.  The
 *    "split" metric (restore.undither_split) scores luma and chroma separately.
 *  - Bilateral disc radius = round(1.5*sigmaSpace), OpenCV's rule for d = 0.
 *  - Gaussian radii follow OpenCV's ksize = round(sigma*4*2+1)|1 for float input
 *    (24 px for the sigma-6 deband blur, 8 for the sigma-2 mask, 12 for sigma-3).
 *  - Borders are BORDER_REFLECT_101, OpenCV's default.
 */
(function (global) {
  "use strict";

  var VERT = `#version 300 es
in vec2 a_pos;
void main(){ gl_Position = vec4(a_pos, 0.0, 1.0); }`;

  // ---- shared GLSL: reflect-101 texel fetch -------------------------------
  var COMMON = `
uniform ivec2 u_size;
ivec2 mirror(ivec2 p){
  if(p.x < 0) p.x = -p.x;
  if(p.x >= u_size.x) p.x = 2*u_size.x - 2 - p.x;
  if(p.y < 0) p.y = -p.y;
  if(p.y >= u_size.y) p.y = 2*u_size.y - 2 - p.y;
  return clamp(p, ivec2(0), u_size - 1);
}
vec4 fetchM(sampler2D t, ivec2 p){ return texelFetch(t, mirror(p), 0); }
`;

  // ---- stage 1: bilateral -------------------------------------------------
  var FRAG_BILATERAL = `#version 300 es
precision highp float; precision highp int;
${COMMON}
uniform sampler2D u_src;
uniform float u_sigmaColor;   // k * q, scored against an L1 channel sum (metric 0)
uniform float u_sigmaSpace;
uniform int   u_radius;
uniform int   u_metric;       // 0: RGB L1 like cv2.bilateralFilter; 1: luma/chroma split
uniform float u_sigmaY;       // metric 1: luma tolerance (tight)
uniform float u_sigmaC;       // metric 1: chroma tolerance (loose)
out vec4 o_col;
const vec3 LW = vec3(0.299, 0.587, 0.114);
void main(){
  ivec2 c = ivec2(gl_FragCoord.xy);
  vec3 centre = fetchM(u_src, c).rgb * 255.0;
  float yc = dot(centre, LW);
  vec3 cc = centre - vec3(yc);
  float gs  = -0.5 / (u_sigmaSpace * u_sigmaSpace);
  float gc  = -0.5 / (u_sigmaColor * u_sigmaColor);
  float gy  = -0.5 / (u_sigmaY * u_sigmaY);
  float gcc = -0.5 / (u_sigmaC * u_sigmaC);
  vec3 sum = vec3(0.0); float wsum = 0.0;
  for(int j = -12; j <= 12; ++j){
    if(j < -u_radius || j > u_radius) continue;
    for(int i = -12; i <= 12; ++i){
      if(i < -u_radius || i > u_radius) continue;
      float r2 = float(i*i + j*j);
      if(r2 > float(u_radius*u_radius)) continue;   // OpenCV keeps a disc, not a box
      vec3 s = fetchM(u_src, c + ivec2(i, j)).rgb * 255.0;
      float e;
      if(u_metric == 0){
        vec3 d = abs(s - centre);
        float l1 = d.r + d.g + d.b;
        e = l1 * l1 * gc;
      } else {
        float ys = dot(s, LW);
        float dy = abs(ys - yc);
        vec3 dc = abs((s - vec3(ys)) - cc);
        float l1c = dc.r + dc.g + dc.b;
        e = dy * dy * gy + l1c * l1c * gcc;
      }
      float w = exp(r2 * gs + e);
      sum += s * w; wsum += w;
    }
  }
  o_col = vec4(sum / max(wsum, 1e-6) / 255.0, 1.0);
}`;

  // ---- undither strength: lerp original -> filtered ------------------------
  var FRAG_BLEND = `#version 300 es
precision highp float; precision highp int;
uniform sampler2D u_a;
uniform sampler2D u_b;
uniform float u_t;
out vec4 o_col;
void main(){
  ivec2 c = ivec2(gl_FragCoord.xy);
  o_col = vec4(mix(texelFetch(u_a, c, 0).rgb, texelFetch(u_b, c, 0).rgb, u_t), 1.0);
}`;

  // ---- stage 2a: single-step contour mask --------------------------------
  var FRAG_STRONG = `#version 300 es
precision highp float; precision highp int;
${COMMON}
uniform sampler2D u_src;
uniform float u_thresh;          // edge_factor * step
out vec4 o_col;
void main(){
  ivec2 c = ivec2(gl_FragCoord.xy);
  vec3 f = texelFetch(u_src, c, 0).rgb * 255.0;
  // restore.py leaves the last column/row at zero rather than reflecting
  float dx = (c.x + 1 < u_size.x) ? length(texelFetch(u_src, c + ivec2(1,0), 0).rgb * 255.0 - f) : 0.0;
  float dy = (c.y + 1 < u_size.y) ? length(texelFetch(u_src, c + ivec2(0,1), 0).rgb * 255.0 - f) : 0.0;
  o_col = vec4(max(dx, dy) > u_thresh ? 1.0 : 0.0, 0.0, 0.0, 1.0);
}`;

  // ---- stage 2b: separable 5x5 dilate (cv2.dilate, ones((5,5))) -----------
  var FRAG_DILATE = `#version 300 es
precision highp float; precision highp int;
uniform ivec2 u_size;
uniform sampler2D u_src;
uniform ivec2 u_step;            // (1,0) then (0,1)
out vec4 o_col;
void main(){
  ivec2 c = ivec2(gl_FragCoord.xy);
  float m = 0.0;
  for(int i = -2; i <= 2; ++i){
    ivec2 p = c + u_step * i;
    if(p.x < 0 || p.y < 0 || p.x >= u_size.x || p.y >= u_size.y) continue;  // dilate border = -inf
    m = max(m, texelFetch(u_src, p, 0).r);
  }
  o_col = vec4(m, 0.0, 0.0, 1.0);
}`;

  // ---- separable gaussian, on colour or on the mask ----------------------
  var FRAG_BLUR = `#version 300 es
precision highp float; precision highp int;
${COMMON}
uniform sampler2D u_src;
uniform ivec2 u_step;
uniform float u_sigma;
uniform int   u_radius;
uniform int   u_invert;          // deband blurs (~strong), i.e. 1 - mask
out vec4 o_col;
void main(){
  ivec2 c = ivec2(gl_FragCoord.xy);
  float g = -0.5 / (u_sigma * u_sigma);
  vec4 sum = vec4(0.0); float wsum = 0.0;
  for(int i = -24; i <= 24; ++i){
    if(i < -u_radius || i > u_radius) continue;
    float w = exp(float(i*i) * g);
    vec4 s = fetchM(u_src, c + u_step * i);
    if(u_invert == 1) s = vec4(1.0) - s;
    sum += s * w; wsum += w;
  }
  o_col = sum / wsum;
}`;

  // ---- stage 2c: blend the blurred image in where the mask allows --------
  var FRAG_DEBAND_MIX = `#version 300 es
precision highp float; precision highp int;
uniform sampler2D u_src;
uniform sampler2D u_blurred;
uniform sampler2D u_mask;
out vec4 o_col;
void main(){
  ivec2 c = ivec2(gl_FragCoord.xy);
  vec3 f = texelFetch(u_src, c, 0).rgb;
  vec3 b = texelFetch(u_blurred, c, 0).rgb;
  float m = texelFetch(u_mask, c, 0).r;
  o_col = vec4(clamp(mix(f, b, m), 0.0, 1.0), 1.0);
}`;

  // ---- stage 3: procedural enhance ---------------------------------------
  var FRAG_ENHANCE = `#version 300 es
precision highp float; precision highp int;
uniform sampler2D u_src;
uniform sampler2D u_lumBlur;     // gaussian sigma=3 of the luminance
uniform float u_amount, u_clarity, u_vibrance;
out vec4 o_col;
const vec3 W = vec3(0.3, 0.59, 0.11);
void main(){
  ivec2 c = ivec2(gl_FragCoord.xy);
  vec3 f = texelFetch(u_src, c, 0).rgb * 255.0;
  float y  = dot(f, W);
  float yb = texelFetch(u_lumBlur, c, 0).r * 255.0;
  f += vec3((y - yb) * u_clarity * u_amount);          // local contrast
  float mx = max(f.r, max(f.g, f.b)), mn = min(f.r, min(f.g, f.b));
  float sat = (mx - mn) / (mx + 1.0);
  float lum = dot(f, W);
  float gain = 1.0 + u_vibrance * u_amount * (1.0 - sat);
  f = vec3(lum) + (f - vec3(lum)) * gain;              // spare what is already saturated
  o_col = vec4(clamp(f, 0.0, 255.0) / 255.0, 1.0);
}`;

  var FRAG_LUMA = `#version 300 es
precision highp float; precision highp int;
uniform sampler2D u_src;
out vec4 o_col;
void main(){
  vec3 f = texelFetch(u_src, ivec2(gl_FragCoord.xy), 0).rgb;
  float y = dot(f, vec3(0.3, 0.59, 0.11));
  o_col = vec4(y, y, y, 1.0);
}`;

  // ---- display: before | after, split by a draggable seam -----------------
  var FRAG_DISPLAY = `#version 300 es
precision highp float; precision highp int;
uniform sampler2D u_before;
uniform sampler2D u_after;
uniform vec2  u_canvas;      // device px
uniform vec2  u_image;       // image px
uniform vec2  u_pan;         // image px at canvas origin
uniform float u_zoom;
uniform float u_split;       // 0..1 across the canvas
uniform float u_seam;        // seam half-width, device px
uniform vec3  u_bg;
uniform float u_lod;
out vec4 o_col;
void main(){
  vec2 px = vec2(gl_FragCoord.x, u_canvas.y - gl_FragCoord.y);
  vec2 img = u_pan + px / u_zoom;
  if(any(lessThan(img, vec2(0.0))) || any(greaterThanEqual(img, u_image))){
    o_col = vec4(u_bg, 1.0); return;
  }
  vec2 uv = img / u_image;
  float sx = u_split * u_canvas.x;
  vec3 c = (px.x < sx) ? textureLod(u_before, uv, u_lod).rgb
                       : textureLod(u_after,  uv, u_lod).rgb;
  float d = abs(px.x - sx);
  if(d < u_seam) c = mix(vec3(1.0), c, smoothstep(0.0, u_seam, d) * 0.35 + 0.65 * step(u_seam - 1.5, d));
  o_col = vec4(c, 1.0);
}`;

  // ------------------------------------------------------------------ utils
  function compile(gl, type, src) {
    var s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
      throw new Error("shader: " + gl.getShaderInfoLog(s) + "\n" + src);
    }
    return s;
  }

  function program(gl, fragSrc) {
    var p = gl.createProgram();
    gl.attachShader(p, compile(gl, gl.VERTEX_SHADER, VERT));
    gl.attachShader(p, compile(gl, gl.FRAGMENT_SHADER, fragSrc));
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
      throw new Error("link: " + gl.getProgramInfoLog(p));
    }
    return p;
  }

  // OpenCV: ksize = round(sigma * 4 * 2 + 1) | 1 for float input; radius = ksize>>1
  function gaussRadius(sigma) { return (((Math.round(sigma * 8 + 1) | 1) - 1) / 2) | 0; }

  // restore.py's defaults (its "spec" preset); the UI overrides these per image.
  var DEFAULT_PARAMS = { sigmaSpace: 1.8, passes: 2, k: 1.3, metric: "l1", strength: 1 };
  var SPLIT_Y_FRAC = 0.31, SPLIT_C_FRAC = 1.15;   // restore.SPLIT_Y_FRAC / SPLIT_C_FRAC

  // The prep.py variant whose measured post-undither band step stands in for a
  // parameter set: exact for the presets, nearest kernel size otherwise.
  function presetFor(P) {
    if (P.metric === "learned") return "learned";
    if (P.metric === "split") return "split";
    return P.sigmaSpace >= 1.5 ? "spec" : P.sigmaSpace >= 1.05 ? "tuned" : "tight";
  }

  function Renderer(canvas) {
    var gl = canvas.getContext("webgl2", { antialias: false, preserveDrawingBuffer: true });
    if (!gl) throw new Error("WebGL2 is required");
    this.gl = gl;
    this.canvas = canvas;
    // RGBA8 throughout: restore.py hands uint8 from one stage to the next, so
    // 8-bit render targets reproduce its quantisation rather than improving on it.

    var quad = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quad);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
    this.vao = gl.createVertexArray();
    gl.bindVertexArray(this.vao);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.bindVertexArray(null);

    this.progs = {
      bilateral: program(gl, FRAG_BILATERAL),
      strong: program(gl, FRAG_STRONG),
      dilate: program(gl, FRAG_DILATE),
      blur: program(gl, FRAG_BLUR),
      mix: program(gl, FRAG_DEBAND_MIX),
      blend: program(gl, FRAG_BLEND),
      luma: program(gl, FRAG_LUMA),
      enhance: program(gl, FRAG_ENHANCE),
      display: program(gl, FRAG_DISPLAY),
    };
    this.pool = [];
    this.fbo = gl.createFramebuffer();
    this.src = null;
    this.out = null;
    this.size = [0, 0];
    this.view = { zoom: 1, panX: 0, panY: 0, split: 0.5 };
    this.bg = [0.06, 0.08, 0.09];
  }

  Renderer.prototype._tex = function (w, h, display) {
    var gl = this.gl, t = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texStorage2D(gl.TEXTURE_2D, display ? Math.floor(Math.log2(Math.max(w, h))) + 1 : 1,
                    gl.RGBA8, w, h);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER,
                     display ? gl.LINEAR_MIPMAP_LINEAR : gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, display ? gl.NEAREST : gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return t;
  };

  Renderer.prototype._scratch = function (i) {
    while (this.pool.length <= i) {
      this.pool.push(this._tex(this.size[0], this.size[1], false));
    }
    return this.pool[i];
  };

  Renderer.prototype._pass = function (prog, target, setup) {
    var gl = this.gl, w = this.size[0], h = this.size[1];
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, target, 0);
    gl.viewport(0, 0, w, h);
    gl.useProgram(prog);
    var loc = gl.getUniformLocation(prog, "u_size");
    if (loc) gl.uniform2i(loc, w, h);
    setup(gl, prog);
    gl.bindVertexArray(this.vao);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.bindVertexArray(null);
  };

  Renderer.prototype._bind = function (prog, name, tex, unit) {
    var gl = this.gl;
    gl.activeTexture(gl.TEXTURE0 + unit);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.uniform1i(gl.getUniformLocation(prog, name), unit);
  };

  /** Upload the source image. `params` carries restore.py's measurements. */
  Renderer.prototype.setImage = function (img, params) {
    var gl = this.gl, w = img.width, h = img.height;
    if (this.size[0] !== w || this.size[1] !== h) {
      [this.src, this.out].concat(this.pool).forEach(function (t) { if (t) gl.deleteTexture(t); });
      this.pool = [];
      this.size = [w, h];
      this.src = this._tex(w, h, true);
      this.out = this._tex(w, h, true);
    }
    gl.bindTexture(gl.TEXTURE_2D, this.src);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, img);
    gl.generateMipmap(gl.TEXTURE_2D);
    this.params = params || {};
    this.setLearned(null);                 // belongs to the previous image
    return this;
  };

  /** A pre-rendered result for the current image (the learned restorer's
   *  output, baked by unditherer/infer.py); null clears it. */
  Renderer.prototype.setLearned = function (img) {
    var gl = this.gl;
    if (this.learned) { gl.deleteTexture(this.learned); this.learned = null; }
    if (!img) return this;
    if (img.width !== this.size[0] || img.height !== this.size[1]) {
      throw new Error("learned image is " + img.width + "×" + img.height + ", frame is " + this.size.join("×"));
    }
    this.learned = this._tex(this.size[0], this.size[1], false);
    gl.bindTexture(gl.TEXTURE_2D, this.learned);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, img);
    return this;
  };

  /** Run the enabled stages; the result lands in this.out.
   *  opts: {undither, deband, enhance, params?} — params as DEFAULT_PARAMS. */
  Renderer.prototype.process = function (opts) {
    var gl = this.gl, self = this, p = this.params;
    var q = p.q || 0;
    var P = Object.assign({}, DEFAULT_PARAMS, opts.params || {});
    var cur = this.src;
    var slot = 3;                                            // 0/1 ping-pong, 2 blend

    // The learned restorer replaces undither and deband together; its output is
    // baked offline, so here it is a texture swap.  Missing bake -> show the
    // original rather than silently substituting the bilateral.
    var learned = P.metric === "learned";
    if (opts.undither && learned) {
      if (this.learned) cur = this.learned;
    } else if (opts.undither && q > 0) {
      var radius = Math.max(1, Math.round(P.sigmaSpace * 1.5)); // OpenCV d = 0 -> radius from sigmaSpace
      var sr = P.k * q, start = cur;
      for (var pass = 0; pass < P.passes; pass++) {
        var dst = this._scratch(pass & 1);
        var input = cur;
        this._pass(this.progs.bilateral, dst, function (gl, prog) {
          self._bind(prog, "u_src", input, 0);
          gl.uniform1f(gl.getUniformLocation(prog, "u_sigmaColor"), sr);
          gl.uniform1f(gl.getUniformLocation(prog, "u_sigmaSpace"), P.sigmaSpace);
          gl.uniform1i(gl.getUniformLocation(prog, "u_radius"), radius);
          gl.uniform1i(gl.getUniformLocation(prog, "u_metric"), P.metric === "split" ? 1 : 0);
          gl.uniform1f(gl.getUniformLocation(prog, "u_sigmaY"), SPLIT_Y_FRAC * sr);
          gl.uniform1f(gl.getUniformLocation(prog, "u_sigmaC"), SPLIT_C_FRAC * sr);
        });
        cur = dst;
      }
      if (P.strength < 1) {
        var blended = this._scratch(2), filtered = cur;
        this._pass(this.progs.blend, blended, function (gl, prog) {
          self._bind(prog, "u_a", start, 0);
          self._bind(prog, "u_b", filtered, 1);
          gl.uniform1f(gl.getUniformLocation(prog, "u_t"), P.strength);
        });
        cur = blended;
      }
    }

    if (opts.deband && !(opts.undither && learned)) {
      // restore.py measures the step *after* undithering when undithering ran;
      // that number depends on the kernel, so take the matching prep.py variant
      var step = p.step || 0;
      if (opts.undither && q > 0) {
        var v = p.variants && p.variants[presetFor(P)];
        step = v ? v.step_after : (p.step_undithered || step);
      }
      if (step > 8.0) {
        var strong = this._scratch(slot++), tmpA = this._scratch(slot++),
            maskT = this._scratch(slot++), blurA = this._scratch(slot++),
            blurB = this._scratch(slot++), mixed = this._scratch(slot++);
        var input2 = cur;
        this._pass(this.progs.strong, strong, function (gl, prog) {
          self._bind(prog, "u_src", input2, 0);
          gl.uniform1f(gl.getUniformLocation(prog, "u_thresh"), 1.5 * step);
        });
        [[1, 0, strong, tmpA], [0, 1, tmpA, maskT]].forEach(function (s) {
          self._pass(self.progs.dilate, s[3], function (gl, prog) {
            self._bind(prog, "u_src", s[2], 0);
            gl.uniform2i(gl.getUniformLocation(prog, "u_step"), s[0], s[1]);
          });
        });
        // gaussian(~strong, sigma=2): invert on the first of the two 1-D passes
        var rMask = gaussRadius(2.0);
        [[1, 0, maskT, tmpA, 1], [0, 1, tmpA, maskT, 0]].forEach(function (s) {
          self._pass(self.progs.blur, s[3], function (gl, prog) {
            self._bind(prog, "u_src", s[2], 0);
            gl.uniform2i(gl.getUniformLocation(prog, "u_step"), s[0], s[1]);
            gl.uniform1f(gl.getUniformLocation(prog, "u_sigma"), 2.0);
            gl.uniform1i(gl.getUniformLocation(prog, "u_radius"), rMask);
            gl.uniform1i(gl.getUniformLocation(prog, "u_invert"), s[4]);
          });
        });
        var rBlur = gaussRadius(6.0);
        [[1, 0, input2, blurA], [0, 1, blurA, blurB]].forEach(function (s) {
          self._pass(self.progs.blur, s[3], function (gl, prog) {
            self._bind(prog, "u_src", s[2], 0);
            gl.uniform2i(gl.getUniformLocation(prog, "u_step"), s[0], s[1]);
            gl.uniform1f(gl.getUniformLocation(prog, "u_sigma"), 6.0);
            gl.uniform1i(gl.getUniformLocation(prog, "u_radius"), rBlur);
            gl.uniform1i(gl.getUniformLocation(prog, "u_invert"), 0);
          });
        });
        this._pass(this.progs.mix, mixed, function (gl, prog) {
          self._bind(prog, "u_src", input2, 0);
          self._bind(prog, "u_blurred", blurB, 1);
          self._bind(prog, "u_mask", maskT, 2);
        });
        cur = mixed;
      }
    }

    if (opts.enhance) {
      var lumA = this._scratch(slot++), lumB = this._scratch(slot++),
          lumC = this._scratch(slot++), enh = this._scratch(slot++);
      var input3 = cur, rLum = gaussRadius(3.0);
      this._pass(this.progs.luma, lumA, function (gl, prog) {
        self._bind(prog, "u_src", input3, 0);
      });
      [[1, 0, lumA, lumB], [0, 1, lumB, lumC]].forEach(function (s) {
        self._pass(self.progs.blur, s[3], function (gl, prog) {
          self._bind(prog, "u_src", s[2], 0);
          gl.uniform2i(gl.getUniformLocation(prog, "u_step"), s[0], s[1]);
          gl.uniform1f(gl.getUniformLocation(prog, "u_sigma"), 3.0);
          gl.uniform1i(gl.getUniformLocation(prog, "u_radius"), rLum);
          gl.uniform1i(gl.getUniformLocation(prog, "u_invert"), 0);
        });
      });
      this._pass(this.progs.enhance, enh, function (gl, prog) {
        self._bind(prog, "u_src", input3, 0);
        self._bind(prog, "u_lumBlur", lumC, 1);
        gl.uniform1f(gl.getUniformLocation(prog, "u_amount"), 0.5);
        gl.uniform1f(gl.getUniformLocation(prog, "u_clarity"), 0.5);
        gl.uniform1f(gl.getUniformLocation(prog, "u_vibrance"), 0.6);
      });
      cur = enh;
    }

    // copy the chain's tail into the display texture (mipmapped for zoom-out)
    this._pass(this.progs.mix, this.out, function (gl, prog) {
      self._bind(prog, "u_src", cur, 0);
      self._bind(prog, "u_blurred", cur, 1);
      self._bind(prog, "u_mask", cur, 2);
    });
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.bindTexture(gl.TEXTURE_2D, this.out);
    gl.generateMipmap(gl.TEXTURE_2D);
    return this;
  };

  Renderer.prototype.draw = function () {
    var gl = this.gl, self = this, c = this.canvas, v = this.view;
    if (!this.src) return;
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, c.width, c.height);
    gl.useProgram(this.progs.display);
    var prog = this.progs.display;
    this._bind(prog, "u_before", this.src, 0);
    this._bind(prog, "u_after", this.out, 1);
    gl.uniform2f(gl.getUniformLocation(prog, "u_canvas"), c.width, c.height);
    gl.uniform2f(gl.getUniformLocation(prog, "u_image"), this.size[0], this.size[1]);
    gl.uniform2f(gl.getUniformLocation(prog, "u_pan"), v.panX, v.panY);
    gl.uniform1f(gl.getUniformLocation(prog, "u_zoom"), v.zoom);
    gl.uniform1f(gl.getUniformLocation(prog, "u_split"), v.split);
    gl.uniform1f(gl.getUniformLocation(prog, "u_seam"), 1.5);
    gl.uniform1f(gl.getUniformLocation(prog, "u_lod"), Math.max(0, -Math.log2(v.zoom)));
    gl.uniform3f(gl.getUniformLocation(prog, "u_bg"), this.bg[0], this.bg[1], this.bg[2]);
    gl.bindVertexArray(this.vao);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.bindVertexArray(null);
  };

  Renderer.prototype.fitZoom = function () {
    return Math.min(this.canvas.width / this.size[0], this.canvas.height / this.size[1]);
  };

  Renderer.prototype.centre = function (zoom) {
    var v = this.view, c = this.canvas;
    v.zoom = zoom;
    v.panX = this.size[0] / 2 - c.width / (2 * zoom);
    v.panY = this.size[1] / 2 - c.height / (2 * zoom);
    return this;
  };

  global.Undither = global.Undither || {};
  global.Undither.Renderer = Renderer;
  global.Undither.gaussRadius = gaussRadius;
  global.Undither.presetFor = presetFor;
  global.Undither.DEFAULT_PARAMS = DEFAULT_PARAMS;
  global.Undither.SPLIT_Y_FRAC = SPLIT_Y_FRAC;
  global.Undither.SPLIT_C_FRAC = SPLIT_C_FRAC;
})(window);
