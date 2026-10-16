'use strict';
const $ = id => document.getElementById(id);
let csrf = '', mode = 'text', draft = null, generation = 0, drawingPointer = null, busy = false;
const canvas = $('drawing'), pen = canvas.getContext('2d');
pen.lineCap = 'round'; pen.lineJoin = 'round';
function clearDrawing() { pen.fillStyle = 'white'; pen.fillRect(0, 0, 800, 480); }
clearDrawing();
function status(message, error = false) { $('status').textContent = message; $('status').dataset.error = String(error); }
function invalidate() { generation++; draft = null; $('send').disabled = true; status('Preview your changes before sending.'); }
function signedIn(value) {
  $('login-panel').hidden = value; $('composer').hidden = !value; $('logout').hidden = !value;
}
async function api(url, options = {}) {
  options.headers = { ...(options.headers || {}), ...(csrf ? { 'X-CSRF-Token': csrf } : {}) };
  const response = await fetch(url, { credentials: 'same-origin', cache: 'no-store', ...options });
  let body;
  try { body = await response.json(); } catch { throw new Error('The mailbox returned an unexpected response.'); }
  if (!response.ok) {
    if (response.status === 401 && !url.endsWith('/login')) { csrf = ''; signedIn(false); }
    throw new Error(body.error || 'The mailbox could not complete that request.');
  }
  return body;
}
$('login-form').addEventListener('submit', async event => {
  event.preventDefault();
  const button = event.submitter; button.disabled = true;
  try {
    const result = await api('/api/sender/login', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ secret: $('secret').value }) });
    csrf = result.csrf; $('secret').value = ''; signedIn(true); status('Your mailbox is ready.');
  } catch (error) { status(error.message, true); } finally { button.disabled = false; }
});
$('logout').addEventListener('click', async () => {
  try {
    await api('/api/sender/logout', { method: 'POST' }); csrf = ''; signedIn(false); invalidate();
    $('note').value = ''; $('photo').value = ''; clearDrawing();
    $('preview-image').removeAttribute('src'); $('preview-image').hidden = true; $('empty-preview').hidden = false;
    status('Signed out.');
  } catch (error) { status(error.message, true); }
});
document.querySelectorAll('[data-mode]').forEach(button => button.addEventListener('click', () => {
  mode = button.dataset.mode;
  document.querySelectorAll('[data-mode]').forEach(tab => tab.setAttribute('aria-pressed', String(tab === button)));
  ['text', 'draw', 'photo'].forEach(name => { $(`${name}-panel`).hidden = name !== mode; });
  invalidate();
}));
['note', 'photo', 'fit'].forEach(id => $(id).addEventListener('input', invalidate));
$('clear').addEventListener('click', () => { clearDrawing(); invalidate(); });
function point(event) { const rect = canvas.getBoundingClientRect(); return [(event.clientX - rect.left) * 800 / rect.width, (event.clientY - rect.top) * 480 / rect.height]; }
canvas.addEventListener('pointerdown', event => {
  if (drawingPointer !== null) return;
  event.preventDefault(); drawingPointer = event.pointerId; canvas.setPointerCapture(event.pointerId);
  invalidate(); const [x, y] = point(event); pen.strokeStyle = 'black'; pen.fillStyle = 'black'; pen.lineWidth = Number($('brush').value);
  pen.beginPath(); pen.arc(x, y, pen.lineWidth / 2, 0, Math.PI * 2); pen.fill();
  pen.beginPath(); pen.moveTo(x, y);
});
canvas.addEventListener('pointermove', event => {
  if (event.pointerId !== drawingPointer) return;
  event.preventDefault(); const [x, y] = point(event); pen.lineTo(x, y); pen.stroke();
});
['pointerup', 'pointercancel', 'lostpointercapture'].forEach(name => canvas.addEventListener(name, event => {
  if (event.pointerId === drawingPointer) { drawingPointer = null; invalidate(); }
}));
function canvasBlob(source) { return new Promise((resolve, reject) => source.toBlob(blob => blob ? resolve(blob) : reject(new Error('Could not prepare this image.')), 'image/png')); }
async function photoBlob(file) {
  if (!file) throw new Error('Choose a photo first.');
  // Safari can decode its supported camera formats, then send a portable PNG.
  const image = new Image(), url = URL.createObjectURL(file);
  try {
    await new Promise((resolve, reject) => { image.onload = resolve; image.onerror = () => reject(new Error('This browser could not open that photo. Try a JPEG or PNG.')); image.src = url; });
    const scale = Math.min(1, 2400 / Math.max(image.naturalWidth, image.naturalHeight));
    const surface = document.createElement('canvas'); surface.width = Math.max(1, Math.round(image.naturalWidth * scale)); surface.height = Math.max(1, Math.round(image.naturalHeight * scale));
    surface.getContext('2d').drawImage(image, 0, 0, surface.width, surface.height);
    return await canvasBlob(surface);
  } finally { URL.revokeObjectURL(url); }
}
$('preview').addEventListener('click', async () => {
  if (busy) return; busy = true; $('preview').disabled = true; $('send').disabled = true;
  const revision = generation, selectedMode = mode, selectedFit = $('fit').value;
  const selectedText = $('note').value, selectedPhoto = $('photo').files[0];
  try {
    status('Preparing your preview…');
    const form = new FormData(); form.set('mode', selectedMode);
    if (selectedMode === 'text') form.set('text', selectedText);
    if (selectedMode === 'draw') form.set('image', await canvasBlob(canvas), 'drawing.png');
    if (selectedMode === 'photo') { form.set('image', await photoBlob(selectedPhoto), 'photo.png'); form.set('fit', selectedFit); }
    const result = await api('/api/sender/preview', { method: 'POST', body: form });
    if (revision !== generation) { status('Your note changed. Preview it again before sending.'); return; }
    const image = $('preview-image');
    image.src = result.preview_url;
    await image.decode();
    if (revision !== generation) { status('Your note changed. Preview it again before sending.'); return; }
    draft = result.draft_id;
    image.hidden = false; $('empty-preview').hidden = true;
    $('send').disabled = false; status('Ready when you are.');
  } catch (error) { draft = null; status(error.message, true); } finally { busy = false; $('preview').disabled = false; }
});
$('send').addEventListener('click', async () => {
  if (!draft || busy) return; busy = true; $('send').disabled = true; $('preview').disabled = true;
  try {
    await api('/api/sender/send', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ draft_id: draft }) });
    draft = null; status('Sent. It will appear the next time the frame checks.');
  } catch (error) { status(error.message, true); $('send').disabled = !draft; } finally { busy = false; $('preview').disabled = false; }
});
api('/api/sender/session').then(result => { csrf = result.csrf; signedIn(true); }).catch(() => signedIn(false));
if ('serviceWorker' in navigator) navigator.serviceWorker.register('/sw.js').catch(() => {});
