/**
 * SensoriX <-> Wokwi  |  Puente en Google Apps Script
 * El ESP32 (Wokwi) ENVIA lecturas con ?action=push
 * La pagina SensoriX las CONSULTA con ?action=read
 */
const API_KEY  = 'sensorix123';  // Cambiala. Debe ser IGUAL en el .ino y en la pagina
const MAX_HIST = 30;             // lecturas guardadas (limite de Script Properties ~9 KB)

function doGet(e) {
  const p = (e && e.parameter) || {};
  let out;
  try {
    if (p.key !== API_KEY) out = { ok: false, error: 'Clave API incorrecta' };
    else out = (p.action === 'push') ? guardar_(p) : leer_(p);
  } catch (err) {
    out = { ok: false, error: String(err) };
  }
  const json = JSON.stringify(out);
  if (p.callback) // JSONP: respaldo si el navegador bloquea CORS
    return ContentService.createTextOutput(p.callback + '(' + json + ');').setMimeType(ContentService.MimeType.JAVASCRIPT);
  return ContentService.createTextOutput(json).setMimeType(ContentService.MimeType.JSON);
}

function guardar_(p) {
  const temp = parseFloat(p.temp), hum = parseFloat(p.hum);
  if (isNaN(temp) || isNaN(hum)) return { ok: false, error: 'Lectura invalida (DHT22 sin datos)' };
  const lock = LockService.getScriptLock();
  lock.waitLock(10000);
  try {
    const props = PropertiesService.getScriptProperties();
    const hist = JSON.parse(props.getProperty('hist') || '[]');
    const seq = Number(props.getProperty('seq') || 0) + 1;
    hist.push({
      seq: seq,
      site: String(p.site || '').slice(0, 60),   // NOMBRE del SITE
      temperature: temp,
      humidity: hum,
      waterDetected: p.water === '1',
      level: isNaN(parseInt(p.lvl)) ? null : parseInt(p.lvl),   // nivel de agua 0-100 %
      ts: new Date().toISOString()
    });
    while (hist.length > MAX_HIST) hist.shift();
    props.setProperty('hist', JSON.stringify(hist));
    props.setProperty('seq', String(seq));
    return { ok: true, seq: seq };
  } finally {
    lock.releaseLock();
  }
}

function leer_(p) {
  const props = PropertiesService.getScriptProperties();
  const since = parseInt(p.since) || 0;
  const hist = JSON.parse(props.getProperty('hist') || '[]');
  return {
    ok: true,
    seq: Number(props.getProperty('seq') || 0),
    readings: hist.filter(function (r) { return r.seq > since; })
  };
}

// Opcional: ejecutala a mano para borrar todo el historial del puente
function limpiar() { PropertiesService.getScriptProperties().deleteAllProperties(); }
