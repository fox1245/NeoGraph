// Render concurrent-bench PNGs.
//
//   docs/images/bench-concurrent-throughput.png
//   docs/images/bench-concurrent-latency.png
//   docs/images/bench-concurrent-rss.png
//
// Feeds from the 1 CPU / 512 MB results in
// benchmarks/concurrent/results.jsonl — the tighter profile, where
// the scaling story is sharpest. The 2 CPU / 1 GB numbers tell the
// same story with a gentler slope.
//
// The headline charts show asyncio mode across all Python frameworks
// + NeoGraph. The mp (multiprocessing) mode bypasses the GIL across N
// processes, which flattens the P99 curve but saturates at pool size
// — the asyncio curves are the story (the GIL ceiling is universal,
// not a LangGraph-specific problem).

const echarts = require('echarts');
const { createCanvas } = require('canvas');
const fs = require('fs');

echarts.setPlatformAPI({ createCanvas: (w, h) => createCanvas(w, h) });

// Per-framework color (NeoGraph green stays the hero color).
const COLORS = {
    'neograph':       '#2ea44f',
    'langgraph':      '#8250df',
    'haystack':       '#1f77b4',
    'pydantic-graph': '#17a2b8',
    'llamaindex':     '#d3822a',
    'autogen':        '#c0392b',
};

const LABELS = {
    'neograph':       'NeoGraph (asio::thread_pool)',
    'langgraph':      'LangGraph asyncio',
    'haystack':       'Haystack asyncio',
    'pydantic-graph': 'pydantic-graph asyncio',
    'llamaindex':     'LlamaIndex asyncio',
    'autogen':        'AutoGen asyncio',
};

const ASYNCIO_ENGINES = [
    'neograph', 'langgraph', 'haystack', 'pydantic-graph', 'llamaindex', 'autogen'
];

// ── Parse results.jsonl ───────────────────────────────────────────────

const RESULTS_PATH = process.env.RESULTS_PATH
    || '/root/Coding/NeoGraph/benchmarks/concurrent/results.jsonl';
const PROFILE = '1:512m';

const rows = fs.readFileSync(RESULTS_PATH, 'utf8')
    .split('\n').filter(Boolean).map(JSON.parse);

function pickAsyncio(engine) {
    return rows
        .filter(r => r.profile === PROFILE)
        .filter(r => r.engine === engine)
        .filter(r => {
            if (engine === 'neograph') return true;              // NG has single mode
            return r.mode === 'asyncio';
        })
        .filter(r => r.status === 'ok' || r.status === undefined)  // drop timeout/oom rows
        .sort((a, b) => a.concurrency - b.concurrency);
}

function throughput(row) {
    const wallSec = Math.max(row.total_wall_ms, 1) / 1000.0;
    return row.concurrency / wallSec;
}

// ── Chart infrastructure ─────────────────────────────────────────────

const CANVAS = { width: 1500, height: 680 };
const FOOTER_TEXT =
    'Profile: 1 CPU / 512 MB (Docker --cpus=1 --memory=512m)  ·  Workload: 3-node seq counter chain  ·  See benchmarks/concurrent/ for reproduction';

function withFooter(canvas, footerText) {
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = '#f6f8fa';
    ctx.fillRect(0, canvas.height - 32, canvas.width, 32);
    ctx.strokeStyle = '#e1e4e8';
    ctx.beginPath();
    ctx.moveTo(0, canvas.height - 32);
    ctx.lineTo(canvas.width, canvas.height - 32);
    ctx.stroke();
    ctx.fillStyle = '#6a737d';
    ctx.font = '12px sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText(footerText, canvas.width / 2, canvas.height - 12);
}

function renderLine(optsBase, outPath) {
    const canvas = createCanvas(CANVAS.width, CANVAS.height);
    const chart = echarts.init(canvas);
    chart.setOption(Object.assign({}, optsBase, { animation: false, backgroundColor: '#ffffff' }));
    withFooter(canvas, FOOTER_TEXT);
    const stream = canvas.createPNGStream();
    stream.pipe(fs.createWriteStream(outPath));
}

// Shared series config for the asyncio-mode charts.
function buildSeries(yExtractor, labelFormatter) {
    return ASYNCIO_ENGINES.map(engine => ({
        name: LABELS[engine],
        type: 'line',
        symbol: 'circle',
        symbolSize: 8,
        data: pickAsyncio(engine).map(r => [r.concurrency, yExtractor(r)]),
        itemStyle: { color: COLORS[engine] },
        lineStyle: {
            color: COLORS[engine],
            width: engine === 'neograph' ? 3.5 : 2.2,
        },
        label: {
            show: engine === 'neograph',  // only label NG — 6 label clouds is unreadable
            position: 'top',
            formatter: labelFormatter,
            fontSize: 11,
            color: COLORS[engine],
            fontWeight: 'bold'
        }
    }));
}

// ── Chart 1: Throughput (req/sec, log Y) ─────────────────────────────

renderLine({
    title: {
        text: 'Throughput under concurrent load — requests per second (log scale, higher is better)',
        subtext: 'NeoGraph scales with batch size via asio::thread_pool dispatch. Every Python asyncio runtime plateaus — the GIL serializes coroutines regardless of framework.',
        left: 'center', top: 20, itemGap: 10,
        textStyle: { fontSize: 18, fontWeight: 'bold', color: '#24292e' },
        subtextStyle: { fontSize: 12, color: '#586069' }
    },
    grid: { left: 110, right: 48, top: 120, bottom: 130 },
    legend: {
        bottom: 50,
        textStyle: { fontSize: 12, color: '#24292e' },
        itemGap: 20
    },
    tooltip: { trigger: 'axis' },
    xAxis: {
        type: 'log',
        name: 'Concurrency N',
        nameLocation: 'middle',
        nameGap: 32,
        nameTextStyle: { fontSize: 12, color: '#586069' },
        axisLabel: { fontSize: 12, formatter: v => v.toLocaleString() },
        splitLine: { lineStyle: { color: '#eaecef' } }
    },
    yAxis: {
        type: 'log',
        name: 'req / sec (log)',
        nameLocation: 'middle',
        nameRotate: 90,
        nameGap: 72,
        nameTextStyle: { fontSize: 12, color: '#586069' },
        axisLabel: { fontSize: 11, formatter: v => v.toLocaleString() + ' req/s' },
        splitLine: { lineStyle: { color: '#eaecef' } }
    },
    series: buildSeries(throughput, p => Math.round(p.value[1]).toLocaleString())
}, '/root/Coding/NeoGraph/docs/images/bench-concurrent-throughput.png');

// ── Chart 2: P99 latency (µs, log Y) ─────────────────────────────────

renderLine({
    title: {
        text: 'Tail latency — P99 per request (log scale, lower is better)',
        subtext: 'NeoGraph stays in microseconds as load scales. Every Python asyncio runtime climbs linearly — once the GIL serializes N coroutines, P99 ≈ N × per-call cost.',
        left: 'center', top: 20, itemGap: 10,
        textStyle: { fontSize: 18, fontWeight: 'bold', color: '#24292e' },
        subtextStyle: { fontSize: 12, color: '#586069' }
    },
    grid: { left: 110, right: 48, top: 120, bottom: 130 },
    legend: {
        bottom: 50,
        textStyle: { fontSize: 12, color: '#24292e' },
        itemGap: 20
    },
    tooltip: { trigger: 'axis' },
    xAxis: {
        type: 'log',
        name: 'Concurrency N',
        nameLocation: 'middle',
        nameGap: 32,
        nameTextStyle: { fontSize: 12, color: '#586069' },
        axisLabel: { fontSize: 12, formatter: v => v.toLocaleString() },
        splitLine: { lineStyle: { color: '#eaecef' } }
    },
    yAxis: {
        type: 'log',
        name: 'P99 latency (log)',
        nameLocation: 'middle',
        nameRotate: 90,
        nameGap: 80,
        nameTextStyle: { fontSize: 12, color: '#586069' },
        axisLabel: {
            fontSize: 11,
            formatter: v => {
                if (v >= 1_000_000) return (v / 1_000_000) + ' s';
                if (v >= 1_000) return (v / 1_000) + ' ms';
                return v + ' µs';
            }
        },
        splitLine: { lineStyle: { color: '#eaecef' } }
    },
    series: buildSeries(
        r => Math.max(r.p99_us, 1),
        p => {
            const v = p.value[1];
            if (v >= 1_000_000) return (v / 1_000_000).toFixed(1) + ' s';
            if (v >= 1000) return (v / 1000).toFixed(1) + ' ms';
            return v + ' µs';
        }
    )
}, '/root/Coding/NeoGraph/docs/images/bench-concurrent-latency.png');

// ── Chart 3: Peak RSS (MB) ───────────────────────────────────────────

renderLine({
    title: {
        text: 'Peak resident memory under concurrent load (MB, lower is better)',
        subtext: 'NeoGraph stays near 5–10 MB at 10k concurrent. Python asyncio runtimes grow linearly — each coroutine holds a stack frame plus framework per-run state.',
        left: 'center', top: 20, itemGap: 10,
        textStyle: { fontSize: 18, fontWeight: 'bold', color: '#24292e' },
        subtextStyle: { fontSize: 12, color: '#586069' }
    },
    grid: { left: 100, right: 48, top: 120, bottom: 130 },
    legend: {
        bottom: 50,
        textStyle: { fontSize: 12, color: '#24292e' },
        itemGap: 20
    },
    tooltip: { trigger: 'axis' },
    xAxis: {
        type: 'log',
        name: 'Concurrency N',
        nameLocation: 'middle',
        nameGap: 32,
        nameTextStyle: { fontSize: 12, color: '#586069' },
        axisLabel: { fontSize: 12, formatter: v => v.toLocaleString() },
        splitLine: { lineStyle: { color: '#eaecef' } }
    },
    yAxis: {
        type: 'value',
        name: 'Peak RSS',
        nameLocation: 'middle',
        nameRotate: 90,
        nameGap: 56,
        nameTextStyle: { fontSize: 12, color: '#586069' },
        axisLabel: { fontSize: 11, formatter: '{value} MB' },
        splitLine: { lineStyle: { color: '#eaecef' } }
    },
    series: buildSeries(
        r => r.peak_rss_kb / 1024,
        p => p.value[1].toFixed(1) + ' MB'
    )
}, '/root/Coding/NeoGraph/docs/images/bench-concurrent-rss.png');

console.log('rendered 3 concurrent bench charts');
