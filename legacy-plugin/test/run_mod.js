// Runs the D2RMM mod script outside D2RMM: files are read from a vanilla extract and written
// to an output "<mod>.mpq/data" folder, as D2RMM would. usage: node run_mod.js <mod.js> <vanilla data> <out data> [allZones] [bovines]
const fs = require('fs');
const path = require('path');
const [modFile, vanilla, out, allZones = '1', bovines = '1'] = process.argv.slice(2);
const p = (f) => f.replace(/\\/g, '/');
const find = (f) => {
  const o = path.join(out, p(f));
  if (fs.existsSync(o)) return o;
  const v = path.join(vanilla, p(f));
  if (fs.existsSync(v)) return v;
  // file names in the extract keep the game's case; match without it
  const dir = path.dirname(v);
  const hit = fs.existsSync(dir) && fs.readdirSync(dir).find((n) => n.toLowerCase() === path.basename(v).toLowerCase());
  if (!hit) throw new Error('no file ' + f);
  return path.join(dir, hit);
};
const write = (f, text) => {
  const o = path.join(out, p(f));
  fs.mkdirSync(path.dirname(o), { recursive: true });
  fs.writeFileSync(o, text);
};
const D2RMM = {
  readTsv(f) {
    const lines = fs.readFileSync(find(f), 'latin1').split(/\r?\n/).filter((l) => l.length);
    const headers = lines[0].split('\t');
    return { headers, rows: lines.slice(1).map((l) => Object.fromEntries(l.split('\t').map((v, i) => [headers[i], v]))) };
  },
  writeTsv(f, t) {
    write(f, [t.headers.join('\t'), ...t.rows.map((r) => t.headers.map((h) => r[h] ?? '').join('\t'))].join('\r\n') + '\r\n');
  },
  readJson(f) {
    let text = fs.readFileSync(find(f), 'utf8').replace(/^﻿/, '');
    // drop // comments and trailing commas outside strings (the game's JSON has both)
    let outText = '', inString = false;
    for (let i = 0; i < text.length; i++) {
      const c = text[i];
      if (inString) {
        outText += c;
        if (c === '\\') { outText += text[++i]; continue; }
        if (c === '"') inString = false;
      } else if (c === '"') { inString = true; outText += c; }
      else if (c === '/' && text[i + 1] === '/') { while (i < text.length && text[i] !== '\n') i++; outText += '\n'; }
      else outText += c;
    }
    return JSON.parse(outText.replace(/,(\s*[}\]])/g, '$1').replace(/[\u0000-\u001f]/g, (m) => (m === '\n' || m === '\r' || m === '\t' ? m : '')));
  },
  writeJson(f, j) {
    write(f, JSON.stringify(j, null, 2));
  },
};
const config = { allZones: allZones === '1', bovines: bovines === '1' };
new Function('D2RMM', 'config', fs.readFileSync(modFile, 'utf8'))(D2RMM, config);
console.log('mod ran');
