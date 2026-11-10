// Spinning PS1-style memory card icons, drawn with three.js.
//
// The page keeps window.mcIcons: Map key -> { canvas, text, mode, phase, angle } where
// mode is 'card' (spins, text is written on its label), 'idle' (a drive that is
// not a card: still) or 'all' (two cards). One shared WebGL renderer draws every
// icon in turn and copies the result into the icon's own 2D canvas, so there is
// a single GL context however many cards are plugged in, and the tiles can be
// rebuilt freely as long as they keep their canvas element.
//
// This file is inlined into ui.html at build time (see the Makefile).
import * as THREE from './three.module.min.js?v=160';

const W = 46, L = 63.5, H = 8.2;   // card size in mm
const PX = 10;                     // decal pixels per mm
const INK = '#26262a';

/* ── geometry ── */
function outline(notches) {
  const s = new THREE.Shape(), w = W / 2, l = L / 2, R = 9, r = 2.5;
  s.moveTo(-w + R, -l);
  s.lineTo(w - R, -l);
  s.absarc(w - R, -l + R, R, -Math.PI / 2, 0);             // grip end: big round corners
  if (notches) for (let i = 0; i < 4; i++) {                 // finger ridges on the right side
    const y = -l + 12 + i * 4.2;
    s.lineTo(w, y); s.lineTo(w - 1.1, y + 0.3); s.lineTo(w - 1.1, y + 1.9); s.lineTo(w, y + 2.2);
  }
  s.lineTo(w, l - r);
  s.absarc(w - r, l - r, r, 0, Math.PI / 2);                 // connector end: tight corners
  s.lineTo(-w + r, l);
  s.absarc(-w + r, l - r, r, Math.PI / 2, Math.PI);
  if (notches) for (let i = 3; i >= 0; i--) {                // and on the left side
    const y = -l + 12 + i * 4.2;
    s.lineTo(-w, y + 2.2); s.lineTo(-w + 1.1, y + 1.9); s.lineTo(-w + 1.1, y + 0.3); s.lineTo(-w, y);
  }
  s.lineTo(-w, -l + R);
  s.absarc(-w + R, -l + R, R, Math.PI, Math.PI * 1.5);
  return s;
}

const BEVEL = 0.7;
const bodyGeo = new THREE.ExtrudeGeometry(outline(true), {
  depth: H - 2 * BEVEL, bevelEnabled: true, bevelThickness: BEVEL, bevelSize: BEVEL, bevelSegments: 3, curveSegments: 14,
});
bodyGeo.translate(0, 0, -(H - 2 * BEVEL) / 2);

const faceGeo = new THREE.ShapeGeometry(outline(false), 14);
{ // map the whole top face onto the decal: u across the width, v along the length
  const p = faceGeo.attributes.position, uv = faceGeo.attributes.uv;
  for (let i = 0; i < p.count; i++) uv.setXY(i, (p.getX(i) + W / 2) / W, (p.getY(i) + L / 2) / L);
}

const plastic = new THREE.MeshStandardMaterial({ color: 0xa2a2a6, roughness: 0.72, metalness: 0.02 });
const dark = new THREE.MeshStandardMaterial({ color: 0x1d1d21, roughness: 0.6 });
const pin = new THREE.MeshStandardMaterial({ color: 0xc9a44a, roughness: 0.35, metalness: 0.8 });
const seam = new THREE.MeshStandardMaterial({ color: 0x77777b, roughness: 0.8 });

function buildCard() {
  const g = new THREE.Group();
  g.add(new THREE.Mesh(bodyGeo, plastic));
  // the line where the two shell halves meet
  const line = new THREE.Mesh(new THREE.BoxGeometry(W + 2 * BEVEL + 0.06, L - 22, 0.22), seam);
  line.position.set(0, 9, -0.6);
  g.add(line);
  // connector end: the opening and its contacts
  const mouth = new THREE.Mesh(new THREE.BoxGeometry(W - 9, 0.5, H - 3.4), dark);
  mouth.position.set(0, L / 2 + BEVEL - 0.2, 0);
  g.add(mouth);
  for (let i = 0; i < 8; i++) {
    const c = new THREE.Mesh(new THREE.BoxGeometry(2.2, 0.5, 0.9), pin);
    c.position.set(-15.2 + i * 4.1 + (i > 1 ? 0.9 : 0) + (i > 4 ? 0.9 : 0), L / 2 + BEVEL - 0.1, -0.9);
    g.add(c);
  }
  // grip end: the recessed window
  const win = new THREE.Mesh(new THREE.BoxGeometry(W - 20, 0.5, H - 3.6), seam);
  win.position.set(0, -L / 2 - BEVEL + 0.2, 0);
  g.add(win);
  // printed and moulded details of the top face
  const face = new THREE.Mesh(faceGeo, new THREE.MeshStandardMaterial({ transparent: true, roughness: 0.8, polygonOffset: true, polygonOffsetFactor: -2 }));
  face.position.z = H / 2 + 0.03;
  g.add(face);
  g.userData.face = face;
  const back = new THREE.Mesh(faceGeo, new THREE.MeshStandardMaterial({ map: backTexture(), transparent: true, roughness: 0.8, polygonOffset: true, polygonOffsetFactor: -2 }));
  back.rotation.y = Math.PI;
  back.position.z = -H / 2 - 0.03;
  g.add(back);
  return g;
}

/* ── the top face, painted on a canvas ── */
function roundRect(c, x, y, w, h, r) {
  c.beginPath(); c.moveTo(x + r, y); c.arcTo(x + w, y, x + w, y + h, r); c.arcTo(x + w, y + h, x, y + h, r);
  c.arcTo(x, y + h, x, y, r); c.arcTo(x, y, x + w, y, r); c.closePath();
}
function spaced(c, text, x, y, gap) { // centred text with letter spacing
  let w = 0; for (const ch of text) w += c.measureText(ch).width + gap; w -= gap;
  let at = x - w / 2; c.textAlign = 'left';
  for (const ch of text) { c.fillText(ch, at, y); at += c.measureText(ch).width + gap; }
}
function paintFace(text) {
  const cv = document.createElement('canvas'); cv.width = W * PX; cv.height = L * PX;
  const c = cv.getContext('2d'), mm = v => v * PX, Y = v => (L - v) * PX; // card mm, y up from the grip end
  c.textBaseline = 'middle';

  // two moulding marks by the connector
  for (const x of [5.2, W - 5.2]) {
    c.fillStyle = 'rgba(0,0,0,.16)'; c.beginPath(); c.arc(mm(x), Y(L - 4.6), mm(1.35), 0, 7); c.fill();
    c.fillStyle = 'rgba(255,255,255,.22)'; c.beginPath(); c.arc(mm(x + 0.15), Y(L - 4.75), mm(0.8), 0, 7); c.fill();
  }
  // raised arrow: which way the card goes in
  const tri = d => { c.beginPath(); c.moveTo(mm(W - 10.5), Y(L - 9.5) + d); c.lineTo(mm(W - 5.2), Y(L - 16.5) + d); c.lineTo(mm(W - 15.8), Y(L - 16.5) + d); c.closePath(); };
  c.fillStyle = 'rgba(0,0,0,.2)'; tri(mm(0.35)); c.fill();
  c.fillStyle = 'rgba(255,255,255,.2)'; tri(0); c.fill();

  // print
  c.fillStyle = INK;
  c.font = `italic 700 ${mm(5.6)}px "Helvetica Neue", Helvetica, Arial, sans-serif`;
  c.textAlign = 'center'; c.fillText('PlayStation', mm(W / 2 - 1), Y(L - 22.2));
  c.font = `700 ${mm(3.5)}px "Helvetica Neue", Helvetica, Arial, sans-serif`;
  spaced(c, 'MEMORY CARD', mm(W / 2), Y(L - 28.6), mm(0.55));

  // recessed label area
  const rx = 3.6, ry = 9.6, rw = W - 7.2, rh = 21.6;
  roundRect(c, mm(rx), Y(ry + rh), mm(rw), mm(rh), mm(1.6));
  c.fillStyle = 'rgba(0,0,0,.075)'; c.fill();
  c.lineWidth = mm(0.35); c.strokeStyle = 'rgba(0,0,0,.28)'; c.stroke();
  c.save(); c.clip();                                       // light catching the lower edge of the recess
  roundRect(c, mm(rx), Y(ry + rh) - mm(0.5), mm(rw), mm(rh), mm(1.6));
  c.strokeStyle = 'rgba(255,255,255,.3)'; c.stroke(); c.restore();

  // engraved maker name at the grip end
  c.font = `700 ${mm(4)}px Georgia, "Times New Roman", serif`;
  c.fillStyle = 'rgba(255,255,255,.3)'; spaced(c, 'SONY', mm(W / 2), Y(4.9) + mm(0.22), mm(1.1));
  c.fillStyle = 'rgba(0,0,0,.34)'; spaced(c, 'SONY', mm(W / 2), Y(4.9), mm(1.1));

  // the card's name, written on the label
  if (text) {
    c.save();
    c.translate(mm(W / 2), Y(ry + rh / 2)); c.rotate(-0.05);
    let size = mm(9);
    const face = '"Segoe Print", "Bradley Hand", "Marker Felt", "Comic Sans MS", cursive';
    c.font = `700 ${size}px ${face}`;
    const fit = mm(rw - 4) / c.measureText(text).width;
    if (fit < 1) { size = Math.max(mm(2.6), size * fit); c.font = `700 ${size}px ${face}`; }
    c.textAlign = 'center'; c.fillStyle = '#1b2f73';
    c.fillText(text, 0, 0, mm(rw - 3));
    c.restore();
  }
  const t = new THREE.CanvasTexture(cv);
  t.colorSpace = THREE.SRGBColorSpace; t.anisotropy = 4;
  return t;
}
// The underside: a shallow label well, the model number and two screw wells.
let backTex;
function backTexture() {
  if (backTex) return backTex;
  const cv = document.createElement('canvas'); cv.width = W * PX; cv.height = L * PX;
  const c = cv.getContext('2d'), mm = v => v * PX, Y = v => (L - v) * PX;
  c.textBaseline = 'middle';
  roundRect(c, mm(4), Y(L - 7), mm(W - 8), mm(30), mm(1.6));
  c.fillStyle = 'rgba(0,0,0,.07)'; c.fill();
  c.lineWidth = mm(0.35); c.strokeStyle = 'rgba(0,0,0,.26)'; c.stroke();
  c.fillStyle = 'rgba(0,0,0,.42)';
  c.font = `700 ${mm(2.6)}px "Helvetica Neue", Helvetica, Arial, sans-serif`;
  spaced(c, 'SCPH-1020', mm(W / 2), Y(L - 13), mm(0.4));
  c.font = `${mm(1.9)}px "Helvetica Neue", Helvetica, Arial, sans-serif`;
  spaced(c, 'MEMORY CARD  15 BLOCKS', mm(W / 2), Y(L - 17.5), mm(0.3));
  for (const x of [8, W - 8]) {
    c.fillStyle = 'rgba(0,0,0,.3)'; c.beginPath(); c.arc(mm(x), Y(9), mm(1.9), 0, 7); c.fill();
    c.fillStyle = 'rgba(0,0,0,.35)'; c.beginPath(); c.arc(mm(x), Y(9), mm(1.1), 0, 7); c.fill();
  }
  backTex = new THREE.CanvasTexture(cv);
  backTex.colorSpace = THREE.SRGBColorSpace; backTex.anisotropy = 4;
  return backTex;
}

/* ── scene ── */
let renderer;
try {
  renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true, powerPreference: 'low-power' });
  renderer.setClearColor(0x000000, 0);
} catch (e) { renderer = null; }

if (renderer) {
  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera(26, 46 / 60, 10, 600);
  scene.add(new THREE.HemisphereLight(0xffffff, 0x3a3d48, 1.3));
  const key = new THREE.DirectionalLight(0xffffff, 2.5); key.position.set(40, 70, 130); scene.add(key);
  // rim lights in the page's green and violet, from behind so the front stays grey
  const rimA = new THREE.DirectionalLight(0x39ff9f, 0.8); rimA.position.set(-110, 40, -150); scene.add(rimA);
  const rimB = new THREE.DirectionalLight(0x7c5cff, 0.8); rimB.position.set(110, -30, -150); scene.add(rimB);

  const tilt = new THREE.Group(); tilt.rotation.set(-0.3, 0, -0.1); scene.add(tilt);
  const spin = new THREE.Group(); tilt.add(spin);
  const a = buildCard(), b = buildCard();
  spin.add(a, b);

  const decals = new Map(); // label text -> texture
  const decal = text => {
    if (!decals.has(text)) {
      if (decals.size > 24) { // forget labels no icon shows any more
        const used = new Set([...window.mcIcons.values()].map(e => e.mode === 'card' ? e.text || '' : ''));
        for (const [k, t] of decals) if (!used.has(k)) { t.dispose(); decals.delete(k); }
      }
      decals.set(text, paintFace(text));
    }
    return decals.get(text);
  };
  const still = matchMedia('(prefers-reduced-motion: reduce)');
  let rw = 0, rh = 0;

  function draw(e, time) {
    const cv = e.canvas, w = cv.width, h = cv.height;
    if (!w || !h) return;
    if (w !== rw || h !== rh) { renderer.setSize(w, h, false); rw = w; rh = h; }
    camera.aspect = w / h;
    const two = e.mode === 'all';
    camera.position.set(0, 0, two ? 205 : 172);
    camera.updateProjectionMatrix();
    const turning = e.mode !== 'idle' && !still.matches;
    // e.angle pins the view (used by the tests); otherwise cards turn and idle drives stand still
    spin.rotation.y = e.angle != null ? e.angle : turning ? time * 0.0011 + (e.phase || 0) : -0.55;
    a.userData.face.material.map = decal(e.mode === 'card' ? e.text || '' : '');
    b.visible = two;
    if (two) {
      a.position.set(-8, 2, 7); a.rotation.z = 0.12;
      b.position.set(9, -2, -7); b.rotation.z = -0.1;
      b.userData.face.material.map = decal('');
    } else { a.position.set(0, 0, 0); a.rotation.z = 0; }
    renderer.render(scene, camera);
    const c = cv.getContext('2d');
    c.clearRect(0, 0, w, h);
    c.drawImage(renderer.domElement, 0, 0, w, h);
    e.drawn3d = true;
  }

  function frame(time) {
    requestAnimationFrame(frame);
    if (document.hidden) return;
    for (const e of window.mcIcons.values())
      if (e.canvas.isConnected && (e.mode !== 'idle' && !still.matches && e.angle == null || !e.drawn3d || e.dirty)) { e.dirty = false; draw(e, time); }
  }
  renderer.domElement.addEventListener('webglcontextlost', ev => ev.preventDefault());
  renderer.domElement.addEventListener('webglcontextrestored', () => { for (const e of window.mcIcons.values()) e.drawn3d = false; });
  window.mcIcons3d = true;
  requestAnimationFrame(frame);
}
