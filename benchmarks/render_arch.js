// Render NeoGraph architecture diagram to PNG.
//
// Server-side ECharts via `canvas`. Produces docs/images/architecture.png
// — a clean horizontal tree showing the layered module structure after
// today's extraction work (GraphCompiler / Scheduler / NodeExecutor /
// CheckpointCoordinator all broken out from GraphEngine).

const echarts = require('echarts');
const { createCanvas } = require('canvas');
const fs = require('fs');

echarts.setPlatformAPI({
    createCanvas: (width, height) => createCanvas(width, height)
});

// Color palette — muted, GitHub-friendly.
const USER   = '#24292e';
const CORE   = '#2ea44f';   // green — main engine
const INTERN = '#1f8a5e';   // darker green — internal classes
const PLUGIN = '#0366d6';   // blue — plugin registries
const LLM    = '#8250df';   // purple — optional modules
const UTIL   = '#d97706';   // amber — utilities

const data = {
    name: 'User Code',
    itemStyle: { color: USER },
    children: [{
        name: 'neograph::core',
        itemStyle: { color: CORE },
        label: { fontWeight: 'bold' },
        children: [{
            name: 'GraphCompiler',
            itemStyle: { color: INTERN },
            children: [
                { name: 'NodeFactory', itemStyle: { color: PLUGIN } },
                { name: 'ReducerRegistry', itemStyle: { color: PLUGIN } },
                { name: 'ConditionRegistry', itemStyle: { color: PLUGIN } }
            ]
        }, {
            name: 'GraphEngine',
            itemStyle: { color: INTERN },
            label: { fontWeight: 'bold' },
            children: [
                { name: 'Scheduler', itemStyle: { color: INTERN } },
                { name: 'NodeExecutor', itemStyle: { color: INTERN } },
                { name: 'CheckpointCoordinator', itemStyle: { color: INTERN } }
            ]
        }]
    }, {
        name: 'neograph::llm',
        itemStyle: { color: LLM }
    }, {
        name: 'neograph::mcp',
        itemStyle: { color: LLM }
    }, {
        name: 'neograph::util',
        itemStyle: { color: UTIL }
    }]
};

const canvas = createCanvas(1600, 680);
const chart = echarts.init(canvas);

// Reserve the bottom 220px for the legend + deps band.
chart.setOption({
    animation: false,
    backgroundColor: '#ffffff',
    title: {
        text: 'NeoGraph architecture',
        subtext: 'core owns routing + execution + checkpoint lifecycle as distinct classes; llm / mcp / util link optionally',
        left: 24,
        top: 16,
        textStyle: { fontSize: 20, fontWeight: 'bold', color: '#24292e' },
        subtextStyle: { fontSize: 13, color: '#586069' }
    },
    tooltip: { show: false },
    series: [{
        type: 'tree',
        data: [data],
        top: 80,
        bottom: 240,
        left: 80,
        right: 280,
        orient: 'LR',
        layout: 'orthogonal',
        edgeShape: 'polyline',
        edgeForkPosition: '50%',
        symbol: 'roundRect',
        symbolSize: [14, 14],
        initialTreeDepth: -1,
        expandAndCollapse: false,
        roam: false,
        label: {
            position: 'right',
            verticalAlign: 'middle',
            align: 'left',
            fontSize: 13,
            color: '#24292e',
            backgroundColor: '#ffffff',
            padding: [6, 10, 6, 10],
            borderRadius: 5,
            borderWidth: 1,
            borderColor: '#e1e4e8',
            lineHeight: 16,
            distance: 8
        },
        leaves: {
            label: { position: 'right', verticalAlign: 'middle', align: 'left' }
        },
        lineStyle: { color: '#d1d5da', width: 1.5, curveness: 0 },
        emphasis: { disabled: true }
    }]
});

// ── Legend band ────────────────────────────────────────────────────────
const ctx = canvas.getContext('2d');
const LEGEND_Y = 460;

// Section header
ctx.fillStyle = '#24292e';
ctx.font = 'bold 14px sans-serif';
ctx.textAlign = 'left';
ctx.fillText('What each internal class owns', 24, LEGEND_Y);

const legendRows = [
    { label: 'GraphCompiler',         desc: 'Parses JSON → CompiledGraph (channels, nodes, edges, barriers, interrupts, retry policy).', color: INTERN },
    { label: 'Scheduler',             desc: 'Signal-dispatch routing + barrier accumulation. Planning-only — no threading, no checkpoint.', color: INTERN },
    { label: 'NodeExecutor',          desc: 'Per-super-step node invocation: retry loop, Taskflow parallel fan-out, Send dispatch.', color: INTERN },
    { label: 'CheckpointCoordinator', desc: 'save_super_step / load_for_resume / record_pending_write / clear_pending_writes.', color: INTERN },
    { label: 'NodeFactory / ReducerRegistry / ConditionRegistry', desc: 'Plugin points — user-registered types, reducers, and conditions wired in at compile().', color: PLUGIN }
];

let y = LEGEND_Y + 22;
ctx.font = '13px sans-serif';
for (const row of legendRows) {
    // Color swatch
    ctx.fillStyle = row.color;
    ctx.fillRect(28, y - 10, 10, 10);
    // Label
    ctx.fillStyle = '#24292e';
    ctx.font = 'bold 13px sans-serif';
    ctx.fillText(row.label, 46, y);
    // Description
    const labelWidth = ctx.measureText(row.label).width;
    ctx.fillStyle = '#586069';
    ctx.font = '13px sans-serif';
    ctx.fillText('— ' + row.desc, 46 + labelWidth + 8, y);
    y += 22;
}

// ── Deps footer band ──────────────────────────────────────────────────
const FOOTER_Y = 620;
ctx.fillStyle = '#f6f8fa';
ctx.fillRect(0, FOOTER_Y, canvas.width, canvas.height - FOOTER_Y);
ctx.strokeStyle = '#e1e4e8';
ctx.lineWidth = 1;
ctx.beginPath();
ctx.moveTo(0, FOOTER_Y);
ctx.lineTo(canvas.width, FOOTER_Y);
ctx.stroke();

ctx.textAlign = 'left';
ctx.fillStyle = '#24292e';
ctx.font = 'bold 13px sans-serif';
const bundledLabel = 'Bundled under deps/:';
const bundledWidth = ctx.measureText(bundledLabel).width;
ctx.fillText(bundledLabel, 24, FOOTER_Y + 22);

ctx.fillStyle = '#586069';
ctx.font = '13px sans-serif';
ctx.fillText('yyjson · Taskflow · cpp-httplib · moodycamel::ConcurrentQueue · Clay',
             24 + bundledWidth + 12, FOOTER_Y + 22);

ctx.fillStyle = '#24292e';
ctx.font = 'bold 13px sans-serif';
const systemLabel = 'System:';
const systemWidth = ctx.measureText(systemLabel).width;
ctx.fillText(systemLabel, 24, FOOTER_Y + 44);

ctx.fillStyle = '#586069';
ctx.font = '13px sans-serif';
ctx.fillText('OpenSSL (only when linking neograph::llm or neograph::mcp)',
             24 + systemWidth + 12, FOOTER_Y + 44);

ctx.font = 'italic 12px sans-serif';
ctx.fillStyle = '#6a737d';
ctx.textAlign = 'right';
ctx.fillText('core has zero network dependencies · httplib is never exposed to user code',
             canvas.width - 24, FOOTER_Y + 44);

const out = fs.createWriteStream('/tmp/bench/architecture.png');
const stream = canvas.createPNGStream();
stream.pipe(out);
out.on('finish', () => console.log('wrote architecture.png'));
