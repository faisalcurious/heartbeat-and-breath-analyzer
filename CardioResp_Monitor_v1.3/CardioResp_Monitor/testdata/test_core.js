const C = require('../web/core.js');
const fs = require('fs');
const L = JSON.parse(fs.readFileSync(__dirname + '/log.json'));
const R = C.computeReport(L);
const show = k => console.log(k.padEnd(14), typeof R[k] === 'number' ? R[k].toFixed(2) : R[k]);
['durMin','hrMean','hrMin','hrMax','rmssd','sdnn','pnn50','validPct','rr','perCV','amp','ti','te','ie','sighs','longestPause','motionSec','postureChanges','rsa','hrPerBreath','sqiH','sqiR','sd1','sd2'].forEach(show);
console.log('pauses', R.pauses.length, 'findings:'); R.findings.forEach(x => console.log('  ', x[0], x[1]));
const pt = {name:'Test Patient', id:'P-0042', sex:'female', place:'front (sternum)', post:'lying on back', notes:'Evening recording after dinner. Mild cough for 3 days.'};
const d = new Date('2026-09-26T21:30:00');
const pdf = C.buildPdf(L, R, pt, d);
fs.writeFileSync(__dirname + '/report.pdf', Buffer.from(pdf));
const csv = C.buildCsv(L, R, pt, d);
fs.writeFileSync(__dirname + '/export.csv', csv);
console.log('pdf bytes', pdf.length, 'csv lines', csv.split('\n').length);
// live analysis from last status message in the stream
if (!fs.existsSync(__dirname + '/stream.jsonl')) { console.log('stream.jsonl not present (run host_test/dump_stream ../testdata) - live analysis test skipped'); process.exit(0); }
const lines = fs.readFileSync(__dirname + '/stream.jsonl', 'utf8').trim().split('\n');
let S = null; for (let i = lines.length - 1; i >= 0; i--) { const o = JSON.parse(lines[i]).m; if (o.t === 's') { S = o; break; } }
const sec = C.liveAnalysis(S, C.ageProfile(S.age), {rsa: R.rsa});
for (const s of sec) { console.log('##', s.title); for (const [l, t] of s.items) console.log('  [' + l + ']', t); }
// age profile boundaries
for (const y of [0, 0.05, 0.1, 0.99, 1, 2.9, 3, 5.9, 6, 11.9, 12, 17.9, 18, 39.9, 40, 64.9, 65, 90]) process.stdout.write(y + '->' + C.ageProfile(y).id + ' ');
console.log();
