// Polls /history repeatedly and accumulates per-note stats across the full sweep.
// Run for ~20 minutes to cover one full effective sweep cycle (~16 min at ESP32 NSDF rate).
const http = require('http');
const fs   = require('fs');

const byNote = {};
const byMiss = {};   // GT>0 but detected_hz==0 (missed detections per note)
const POLL_INTERVAL_MS = 8000;   // every 8 sec
const DURATION_MS      = 1200000; // 20 minutes (one full effective sweep ~16 min + margin)
const REPORT_FILE      = 'sweep_report.txt';

function fetchHistory(cb) {
    http.get('http://192.168.86.163/history?n=200', res => {
        let data = '';
        res.on('data', d => data += d);
        res.on('end', () => { try { cb(JSON.parse(data)); } catch(e) { cb([]); } });
    }).on('error', () => cb([]));
}

function ingest(frames) {
    let added = 0;
    for (const f of frames) {
        if (f.ground_truth_hz <= 0) continue;
        const key = f.ground_truth_hz.toFixed(2);
        if (f.detected_hz <= 0) {
            // GT fired but no detection — count as miss (dedup by ts in report)
            if (!byMiss[key]) byMiss[key] = new Set();
            byMiss[key].add(f.ts_us);
            continue;
        }
        if (!byNote[key]) byNote[key] = [];
        byNote[key].push({ cents: f.cents_error, peak: f.nsdf_peak_val, ts: f.ts_us });
        added++;
    }
    return added;
}

function dedup() {
    // Remove duplicate frames (same ts_us seen in overlapping polls)
    for (const k of Object.keys(byNote)) {
        const seen = new Set();
        byNote[k] = byNote[k].filter(f => {
            if (seen.has(f.ts)) return false;
            seen.add(f.ts); return true;
        });
    }
    // byMiss already uses Sets so duplicates are already excluded
}

function report() {
    dedup();
    const sr = 44100, half = 2048;

    // All notes that had any GT activity (hit or miss)
    const allKeys = new Set([...Object.keys(byNote), ...Object.keys(byMiss)]);
    const notes = [...allKeys].sort((a,b) => parseFloat(a)-parseFloat(b));

    const lines = [];
    lines.push('\n=== Per-note accuracy (unique frames) ===');
    lines.push('hz          n  miss  mean    std     min     max   avgpk  periods  verdict');
    lines.push('--------- ---- ----  ------- ------- ------- -----  -----  ------- -------');
    for (const hz of notes) {
        const frames = byNote[hz] || [];
        const miss   = byMiss[hz] ? byMiss[hz].size : 0;
        if (frames.length === 0) {
            const periods = half / (sr / parseFloat(hz));
            lines.push(
                hz.padEnd(9)+'    0  '+
                miss.toString().padStart(4)+'  '+
                '     -       -       -     -      -  '+
                periods.toFixed(1).padStart(7)+'  NODET'
            );
            continue;
        }
        const errs = frames.map(f => f.cents);
        const mean = errs.reduce((a,b)=>a+b,0)/errs.length;
        const std  = Math.sqrt(errs.map(e=>(e-mean)**2).reduce((a,b)=>a+b,0)/errs.length);
        const avgpk = frames.map(f=>f.peak).reduce((a,b)=>a+b,0)/frames.length;
        const periods = half / (sr / parseFloat(hz));
        const verdict = Math.abs(mean) > 50 ? 'WRONG' : std > 30 ? 'UNSTBL' : std > 15 ? 'NOISY' : 'OK';
        lines.push(
            hz.padEnd(9)+' '+
            frames.length.toString().padStart(4)+'  '+
            miss.toString().padStart(4)+'  '+
            mean.toFixed(1).padStart(6)+'  '+
            std.toFixed(1).padStart(6)+'  '+
            Math.min(...errs).toFixed(1).padStart(6)+'  '+
            Math.max(...errs).toFixed(1).padStart(5)+'  '+
            avgpk.toFixed(3).padStart(6)+'  '+
            periods.toFixed(1).padStart(7)+'  '+
            verdict
        );
    }
    const hit = Object.keys(byNote).length;
    lines.push('\nNotes hit: ' + hit + '/17   Notes with GT activity: ' + notes.length + '/17');

    const output = lines.join('\n');
    process.stdout.write('\n');   // end the \r status line cleanly
    console.log(output);
    fs.writeFileSync(REPORT_FILE, output + '\n');
    console.log('(report also saved to ' + REPORT_FILE + ')');
}

let polls = 0;
let done  = false;
const start = Date.now();

function poll() {
    fetchHistory(frames => {
        const added = ingest(frames);
        polls++;
        if (!done)
            process.stdout.write('\rpoll ' + polls + ', elapsed ' + Math.round((Date.now()-start)/1000) + 's, unique frames: ' + Object.values(byNote).flat().length + '      ');
    });
    if (Date.now() - start < DURATION_MS) {
        setTimeout(poll, POLL_INTERVAL_MS);
    } else {
        done = true;
        report();
    }
}

console.log('Polling http://192.168.86.163/history every 8s for 20 minutes...');
poll();
