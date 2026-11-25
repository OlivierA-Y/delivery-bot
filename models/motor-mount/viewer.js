import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import DATA from './geometry.json';

const host = document.querySelector('#viewport');
const renderer = new THREE.WebGLRenderer({antialias: true, preserveDrawingBuffer: true});
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
renderer.setClearColor(0xf1f0eb);
renderer.outputColorSpace = THREE.SRGBColorSpace;
host.appendChild(renderer.domElement);
renderer.domElement.setAttribute('aria-label', 'Interactive 3D motor axle clamp assembly. Drag to orbit; scroll to zoom.');
const scene = new THREE.Scene();
scene.add(new THREE.HemisphereLight(0xffffff, 0x77746b, 2.7));
const key = new THREE.DirectionalLight(0xffffff, 3.5); key.position.set(200, -300, 500); scene.add(key);
const fill = new THREE.DirectionalLight(0xffffff, 1.7); fill.position.set(-300, 200, 120); scene.add(fill);
const camera = new THREE.PerspectiveCamera(34, 1, 1, 5000); camera.up.set(0, 0, 1);
const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true; controls.dampingFactor = 0.1; controls.minDistance = 70; controls.maxDistance = 2200;
const grid = new THREE.GridHelper(1000, 40, 0xcecdc4, 0xe1dfd5); grid.rotation.x = Math.PI / 2; grid.position.z = -50;
scene.add(grid);
const root = new THREE.Group(); scene.add(root);
const objects = [];
for (const part of DATA.parts) {
  const geo = new THREE.BufferGeometry();
  geo.setAttribute('position', new THREE.Float32BufferAttribute(part.vertices, 3)); geo.setIndex(part.indices); geo.computeVertexNormals();
  const mat = new THREE.MeshStandardMaterial({color: part.color,
    roughness: part.role === 'deck' ? 0.85 : 0.46, metalness: ['deck', 'printed', 'wheel'].includes(part.role) ? 0.05 : 0.48});
  const mesh = new THREE.Mesh(geo, mat); mesh.userData = part; root.add(mesh); objects.push(mesh);
  const edges = new THREE.LineSegments(new THREE.EdgesGeometry(geo, 35), new THREE.LineBasicMaterial({color: 0x22282e, transparent: true, opacity: 0.22}));
  mesh.add(edges);
}
let exploded = 0, full = false, wheel = true, chosen;
const descriptions = {
  upper: 'METAL · Upper clamp cap. The existing cap may be reusable after its shape, axle fit and hole diameter are measured.',
  lower: 'METAL · Separate lower saddle with an axle-shaped seat. This provides the missing metal support beneath your axle. Example geometry only.',
  backer: 'STEEL · Backing plate beneath the plywood. Spreads bolt loads over a wider area; shown 3 mm thick as a preliminary example.',
  deck: 'PLYWOOD · 12 mm lower deck. Through-bolted to the metal assembly. Detail mode crops the deck so you can inspect the mounting stack.',
  axle: 'REFERENCE · The stationary axle has a flat seat to resist motor reaction torque. Diameter, flat profile and usable length still need measuring.',
  bolt: 'HARDWARE · Example M6 × 70 through-bolt. Diameter, grade and final length must be selected after checking your actual cap and stack height.',
  washer_top: 'HARDWARE · Washer under the bolt head. Illustrative dimensions.',
  washer_bottom: 'HARDWARE · Washer under the backing plate. Illustrative dimensions.',
  nut: 'HARDWARE · Locking-nut envelope. Thread and locking insert are not modelled.',
  wheel: 'REFERENCE · Simplified motor/wheel envelope, assumed Ø165 × 65 mm. Actual motor shape and dimensions are unknown.',
  printed: 'PETG · Optional cable-routing clip. Fit-check with the actual cable; printed parts do not carry motor or axle loads.'
};
function select(mesh) {
  if (chosen) chosen.material.emissive.setHex(0);
  chosen = mesh;
  if (mesh) {
    mesh.material.emissive.setHex(0x302010);
    document.querySelector('#part-title').textContent = mesh.userData.name;
    document.querySelector('#part-description').textContent = descriptions[mesh.userData.role];
  }
}
function update() {
  for (const mesh of objects) {
    const p = mesh.userData;
    mesh.visible = (full ? p.group !== 'detail' : p.group === 'right' || p.group === 'detail') && (wheel || !['wheel','axle'].includes(p.role));
    mesh.position.set(...p.explode.map(v => v * exploded));
  }
  document.querySelector('#spread-readout').textContent = `${Math.round(exploded * 100)}%`;
  document.querySelector('#view-note').textContent = full ? 'Full deck · two mirrored motor mounts · caster support omitted' : 'Right motor mount · deck cropped for visibility';
}
function frame(under = false) {
  const target = full ? new THREE.Vector3(0, 0, 20) : new THREE.Vector3(wheel ? 178 : 142, 0, exploded > 0.1 ? 35 : 22);
  controls.target.copy(target);
  const offset = full ? new THREE.Vector3(-650, -750, under ? -650 : 610) : new THREE.Vector3(-280, -300, under ? -240 : 240);
  if (exploded > 0.1) offset.multiplyScalar(1.28);
  camera.position.copy(target).add(offset);
  controls.update();
}
document.querySelector('#spread').addEventListener('input', e => {exploded = +e.target.value / 100; update();});
document.querySelector('#assembled').addEventListener('click', () => {exploded = 0; document.querySelector('#spread').value = 0; update(); frame();});
document.querySelector('#exploded').addEventListener('click', () => {exploded = 1; document.querySelector('#spread').value = 100; update(); frame();});
document.querySelector('#under').addEventListener('click', () => frame(true));
document.querySelector('#reset').addEventListener('click', () => frame());
document.querySelector('#full').addEventListener('change', e => {full = e.target.checked; update(); frame();});
document.querySelector('#wheel').addEventListener('change', e => {wheel = e.target.checked; update(); frame();});
document.querySelectorAll('[data-role]').forEach(b => b.addEventListener('click', () => select(objects.find(o => o.userData.group === 'right' && o.userData.role === b.dataset.role))));
const ray = new THREE.Raycaster(); let down;
renderer.domElement.addEventListener('pointerdown', e => {down = [e.clientX, e.clientY];});
renderer.domElement.addEventListener('pointerup', e => {
  if (!down || Math.hypot(e.clientX-down[0],e.clientY-down[1]) > 5) return;
  const r = renderer.domElement.getBoundingClientRect();
  ray.setFromCamera(new THREE.Vector2((e.clientX-r.left)/r.width*2-1, -(e.clientY-r.top)/r.height*2+1), camera);
  const hits = ray.intersectObjects(objects.filter(o => o.visible), false); if (hits.length) select(hits[0].object);
});
new ResizeObserver(() => {const r = host.getBoundingClientRect(); renderer.setSize(r.width,r.height); camera.aspect = r.width/r.height; camera.updateProjectionMatrix();}).observe(host);
update(); frame(); select(objects.find(o => o.userData.group === 'right' && o.userData.role === 'lower'));
renderer.setAnimationLoop(() => {controls.update(); renderer.render(scene,camera);});
window.mountViewer = {renderer, scene, camera, objects, parameters: DATA.parameters,
  state: () => ({full,wheel,exploded,selected:chosen?.userData.name}),
  show: (mode) => {full = mode === 'full'; document.querySelector('#full').checked=full; update(); frame(mode === 'under');}};
