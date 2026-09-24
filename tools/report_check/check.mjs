/**
 * APILedger - 报告图表 JS 校验
 *
 * 用途: 无需浏览器, 跑一遍 output/report.html 内嵌的图表脚本, 校验"数据口径 /
 *       tooltip 文案 / 空态"这类渲染出来看不出、只能靠肉眼的隐性问题。
 *       校验项全部是稳定不变量 (不写死布局数字):
 *         1. 四个图表容器都拿到了 ECharts option (脚本没崩、没改名)
 *         2. 浅色/暗色两套主题都注册了不透明画布背景, 且同一轮渲染里四图同主题
 *         3. 切到暗色后: 使用暗色画布 + 暗色系列配色, option 里不残留浅色硬编码
 *         4. 均价曲线每个点都落在当日该模型原始单价范围 (含 5% 吸附容差) 内
 *         5. 均价 tooltip 的类型占比合计 100.00% 且保留 2 位小数
 *         6. 各图表 tooltip 文案不含 undefined / NaN
 *         7. 空态 (无筛选结果 / 全零单价) 不抛异常, 且给出对应的空态提示
 *
 * 用法: pixi run check-report
 *       node tools/report_check/check.mjs <report.html 路径>   (默认 output/report.html)
 *
 * 依赖: node >= 18 (DOM / ECharts 用最小桩件替代, 不联网)。报告需先由 main.py 生成。
 *
 * 输出: 逐项 ok / FAIL, 有问题时退出码 1, 找不到报告时退出码 2。
 *
 * 限制: 桩件不计算布局 —— **不能**发现排版问题 (图例换行、标签遮挡、刻度美观),
 *       那类问题只能在浏览器里看。本工具管的是"数据对不对", 不是"好不好看"。
 */

import fs from 'fs';

const REPORT_PATH = process.argv[2] || 'output/report.html';

if (!fs.existsSync(REPORT_PATH)) {
  console.error(`[ERROR] 找不到报告文件: ${REPORT_PATH}`);
  console.error('        先运行 pixi run python main.py 生成报告, 或传入报告路径');
  process.exit(2);
}

const html = fs.readFileSync(REPORT_PATH, 'utf-8');
const scriptStart = html.indexOf('const DATA = ');
if (scriptStart < 0) {
  console.error('[ERROR] 报告里找不到内嵌脚本 (缺 "const DATA = "), 报告可能已损坏');
  process.exit(2);
}
const scriptBody = html.slice(scriptStart, html.lastIndexOf('</script>')).trim();

const CHART_IDS = ['chart-trend', 'chart-model', 'chart-stack', 'chart-price'];

// ═══════════════════════════════════════════
// 最小 DOM / ECharts 桩件
// ═══════════════════════════════════════════
// 元素按 id 复用 (与真实 DOM 一致), echarts.init 收到的元素带 id →
// 采集到的 option 能直接对应到图表, 不依赖渲染顺序。
function makeEnv() {
  const els = new Map();
  const options = {};
  const themes = {};   // registerTheme 注册的主题
  const inits = [];    // echarts.init 调用记录 { id, theme }
  const el = id => ({
    id, value: '', textContent: '', innerHTML: '', disabled: false, selectedIndex: 0,
    clientWidth: 1400, dataset: {}, style: {},
    classList: { toggle() {}, add() {}, remove() {} },
    appendChild() {}, addEventListener() {}, remove() {},
  });
  const getEl = id => { if (!els.has(id)) els.set(id, el(id)); return els.get(id); };

  global.document = {
    getElementById: getEl, querySelectorAll: () => [], createElement: () => el(''),
    documentElement: { dataset: {}, style: {} },   // 主题写在 <html data-theme> 上
  };
  global.window = global;
  global.addEventListener = () => {};
  global.localStorage = {
    store: {},
    getItem(k) { return Object.prototype.hasOwnProperty.call(this.store, k) ? this.store[k] : null; },
    setItem(k, v) { this.store[k] = String(v); },
  };
  global.echarts = {
    registerTheme: (name, cfg) => { themes[name] = cfg; },
    init: (e, theme) => {
      inits.push({ id: e && e.id, theme });
      return {
        setOption: o => { if (e && e.id) options[e.id] = o; },
        dispose() {},
        resize() {},
      };
    },
  };
  return { els, options, themes, inits, getEl, document: global.document, localStorage: global.localStorage };
}

// 跑一遍报告脚本, 末尾追加 tail (可访问脚本作用域内的 filtered / ROWS / renderCharts 等)
function runReport(env, tail) {
  return new Function(scriptBody + '\n' + tail)();
}

// ═══════════════════════════════════════════
// 断言
// ═══════════════════════════════════════════
let failed = 0;
function ok(cond, msg, details) {
  console.log((cond ? '     ok  ' : '   FAIL  ') + msg);
  if (!cond) {
    failed++;
    for (const d of (details || []).slice(0, 5)) console.log('           - ' + d);
  }
}

// ═══════════════════════════════════════════
// 1. 全量数据下渲染, 检查图表与数据口径
// ═══════════════════════════════════════════
console.log(`\n报告: ${REPORT_PATH}`);
console.log('\n── 渲染与数据口径 ──');

const env = makeEnv();
const { ROWS, PALETTE } = runReport(env, 'filtered = ROWS; renderCharts(); return { ROWS, PALETTE };');

if (!ROWS.length) {
  console.log('     --  报告内无记录, 跳过数据类检查 (只做空态检查)');
} else {
  for (const id of CHART_IDS) {
    ok(!!env.options[id], `图表 ${id} 渲染出 option`);
  }

  // ── 画布必须有不透明背景: 图例/坐标轴文字画在 canvas 像素里, 浏览器/扩展的强制
  //    暗色只能反色 DOM → 透明画布会让变暗的 DOM 压住图内文字; 两套主题都要给画布底 ──
  const lum = hex => {                         // 相对亮度 (0 黑 ~ 1 白), 判断底色明暗
    const m = /^#([0-9a-f]{6})$/i.exec(String(hex));
    if (!m) return -1;
    const n = parseInt(m[1], 16);
    const ch = [n >> 16 & 255, n >> 8 & 255, n & 255].map(v => {
      v /= 255; return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
    });
    return 0.2126 * ch[0] + 0.7152 * ch[1] + 0.0722 * ch[2];
  };
  const lightCfg = env.themes['apiLedgerLight'], darkCfg = env.themes['apiLedgerDark'];
  ok(!!(lightCfg && lightCfg.backgroundColor && darkCfg && darkCfg.backgroundColor),
     '浅色/暗色主题都注册了不透明画布背景 (暗色环境下图内文字不被压住)',
     [`已注册主题: ${Object.keys(env.themes).join(', ') || '无'}`]);
  ok(lum(lightCfg && lightCfg.backgroundColor) > 0.5 && lum(darkCfg && darkCfg.backgroundColor) < 0.2,
     '浅色主题画布底浅 / 暗色主题画布底深',
     [`浅色 ${lightCfg && lightCfg.backgroundColor} / 暗色 ${darkCfg && darkCfg.backgroundColor}`]);
  const mixed = env.inits.filter(i => i.theme !== 'apiLedgerLight').map(i => `${i.id}: ${i.theme}`);
  ok(env.inits.length > 0 && mixed.length === 0, '默认(浅色)渲染下所有图表都用浅色主题', mixed);

  // ── 图内数值标注色 (条形右侧的绝对值): 浅色沿用 ECharts 默认, 暗色必须显式给浅色 ──
  const lightLabels = ((env.options['chart-model'] || {}).series || []).map(s => s.label && s.label.color);
  ok(lightLabels.length > 0 && lightLabels.every(c => c === PALETTE.light.label),
     '浅色下数值标注沿用默认色 (加暗色不该改动浅色观感)', [`实际: ${lightLabels.join(', ')}`]);

  // ── 均价曲线: 每个点必须是当日该模型原始单价的加权平均 (吸附只在 ±5% 内偏移) ──
  const priceOpt = env.options['chart-price'];
  if (!priceOpt) {
    ok(false, '均价曲线未渲染, 无法校验');
  } else {
    // 序列名可能是 "模型" 或 "平台 · 模型" (同模型跨平台时拆线)
    const rawRows = (name, date) => {
      const sep = name.indexOf(' · ');
      const pf = sep < 0 ? null : name.slice(0, sep);
      const mdl = sep < 0 ? name : name.slice(sep + 3);
      return ROWS.filter(r =>
        String(r.bill_start).slice(0, 10) === date &&
        (r.model || '(空)') === mdl &&
        (!pf || (r.platform || '') === pf) &&
        Number(r.unit_price) > 0);
    };
    const bad = [];
    let points = 0;
    priceOpt.series.forEach(s => {
      s.data.forEach((v, i) => {
        if (v == null) return;
        points++;
        const date = priceOpt.xAxis.data[i];
        const rows = rawRows(s.name, date);
        if (!rows.length) { bad.push(`${s.name} ${date}: 找不到对应原始记录`); return; }
        const prices = rows.map(r => Number(r.unit_price));
        const lo = Math.min(...prices) * 0.95, hi = Math.max(...prices) * 1.05;
        if (!Number.isFinite(v) || v <= 0 || v < lo - 1e-9 || v > hi + 1e-9) {
          bad.push(`${s.name} ${date}: 均价 ${v} 超出原始单价 [${lo.toFixed(4)}, ${hi.toFixed(4)}]`);
        }
      });
    });
    ok(bad.length === 0, `均价曲线 ${points} 个点均落在当日原始单价范围内 (±5% 吸附容差)`, bad);

    // ── tooltip 的类型占比: 合计 100.00%, 保留 2 位小数 ──
    const tip = priceOpt.tooltip.formatter;
    const shareBad = [];
    let tipRows = 0;
    for (let i = 0; i < priceOpt.xAxis.data.length; i++) {
      const date = priceOpt.xAxis.data[i];
      const params = priceOpt.series
        .map(s => ({ axisValue: date, seriesName: s.name, marker: '●', value: s.data[i] }))
        .filter(p => p.value != null);
      if (!params.length) continue;
      for (const line of tip(params).split('<br>').filter(l => l.includes('%'))) {
        tipRows++;
        const pcts = [...line.matchAll(/([\d.]+)%/g)].map(m => m[1]);
        const sum = pcts.reduce((a, p) => a + Number(p), 0);
        const twoDecimals = pcts.every(p => (p.split('.')[1] || '').length === 2);
        if (!twoDecimals || Math.abs(sum - 100) > 0.02) {
          shareBad.push(`${date}: 合计 ${sum.toFixed(2)}% | ${line.replace(/<[^>]+>/g, '')}`);
        }
      }
    }
    ok(shareBad.length === 0, `均价 tooltip 类型占比合计 100.00% / 2 位小数 (检查 ${tipRows} 行)`, shareBad);
  }

  // ── 各图表 tooltip 文案: 不出现 undefined / NaN ──
  const textBad = [];
  for (const id of CHART_IDS) {
    const opt = env.options[id];
    if (!opt || !opt.tooltip || typeof opt.tooltip.formatter !== 'function') continue;
    let samples = [];
    if (id === 'chart-price') {
      const i = Math.floor(opt.xAxis.data.length / 2);
      samples = [opt.series.map(s => ({ axisValue: opt.xAxis.data[i], seriesName: s.name, marker: '●', value: s.data[i] }))];
    } else if (id === 'chart-stack') {
      const i = Math.floor(opt.series[0].data.length / 2);
      samples = [opt.series.map(s => ({ axisValue: opt.yAxis.data[i], dataIndex: i, seriesName: s.name, marker: '■', value: s.data[i] }))];
    } else if (id === 'chart-trend') {
      const i = Math.floor(opt.xAxis.data.length / 2);
      samples = [opt.series.map(s => ({ axisValue: opt.xAxis.data[i], seriesName: s.name, marker: '●', value: s.data[i] }))];
    } else {
      samples = [opt.series.map((s, si) => ({ dataIndex: 0, seriesIndex: si, seriesName: s.name, marker: '■', value: s.data[0] }))];
    }
    for (const params of samples) {
      if (!params.length) continue;
      const out = String(opt.tooltip.formatter(params));
      if (/undefined|NaN/.test(out)) textBad.push(`${id}: ${out.slice(0, 120)}`);
    }
  }
  ok(textBad.length === 0, '各图表 tooltip 文案不含 undefined / NaN', textBad);
}

// ═══════════════════════════════════════════
// 2. 暗色主题: 切主题后重建实例, 用暗色画布与暗色系列配色
// ═══════════════════════════════════════════
console.log('\n── 暗色主题 ──');

// 浅色系列色不应出现在暗色 option 里 (漏改一处 = 暗色下某条线/某个标签糊在深底上)
const LIGHT_ONLY = ['#ffffff', '#1a1a1a', '#1f538d', '#2fa572', '#c0392b'];
const darkEnv = makeEnv();
const darkRun = runReport(darkEnv,
  'filtered = ROWS; toggleTheme(); return { ROWS, CHART_COLORS, CHART_COLORS_DARK, PALETTE };');

if (!darkRun.ROWS.length) {
  console.log('     --  报告内无记录, 跳过暗色检查 (只做空态检查)');
} else {
  const darkThemed = darkEnv.inits.filter(i => i.theme === 'apiLedgerDark').map(i => i.id);
  ok(CHART_IDS.every(id => darkThemed.includes(id)),
     '切到暗色后四个图表都用暗色主题重建 (主题在 init 时绑定, 必须重建实例)',
     [`未重建: ${CHART_IDS.filter(id => !darkThemed.includes(id)).join(', ') || '无'}`]);
  ok(darkEnv.document.documentElement.dataset.theme === 'dark', '主题选择写到 <html data-theme>');
  ok(darkEnv.localStorage.getItem('apiledger-theme') === 'dark', '主题选择被记住 (localStorage)');
  ok(String(darkEnv.getEl('btn-theme').textContent).includes('浅色'),
     '主题按钮文案切到"浅色" (点击可切回)', [`实际: ${darkEnv.getEl('btn-theme').textContent}`]);

  const dOpt = darkEnv.options['chart-price'];
  if (!dOpt) {
    ok(false, '暗色下均价曲线未渲染, 无法校验配色');
  } else {
    const used = dOpt.series.map(s => s.itemStyle && s.itemStyle.color);
    const want = used.map((_, i) => darkRun.CHART_COLORS_DARK[i % darkRun.CHART_COLORS_DARK.length]);
    ok(used.length > 0 && used.every((c, i) => c === want[i]),
       `暗色下均价曲线改用暗色系列配色 (${used.length} 条)`, [`实际: ${used.join(', ')}`]);
    const legColor = dOpt.legend && dOpt.legend.textStyle && dOpt.legend.textStyle.color;
    ok(!legColor, '图例文字色不写死 (交给主题, 否则暗色下深字压深底看不清)',
       [`legend.textStyle.color = ${legColor}`]);
    const darkLabels = ((darkEnv.options['chart-model'] || {}).series || []).map(s => s.label && s.label.color);
    ok(darkLabels.length > 0 && darkLabels.every(c => c === darkRun.PALETTE.dark.label),
       '暗色下数值标注改用浅色 (默认色压在深底上看不见)', [`实际: ${darkLabels.join(', ')}`]);
    const leaked = [];
    for (const id of CHART_IDS) {
      const json = JSON.stringify(darkEnv.options[id] || {});
      for (const c of LIGHT_ONLY) if (json.includes(c)) leaked.push(`${id}: ${c}`);
    }
    ok(leaked.length === 0, '暗色 option 里不残留浅色硬编码', leaked);
  }
}

// ═══════════════════════════════════════════
// 3. 空态: 不抛异常, 且给出对应提示
// ═══════════════════════════════════════════
console.log('\n── 空态 ──');

const CASES = [
  { name: '筛选无结果', tail: 'filtered = []; renderCharts();', expect: '当前筛选无数据' },
  { name: '有记录但全无单价', tail: 'filtered = ROWS.map(r => ({ ...r, unit_price: 0 })); renderCharts();',
    expect: '当前筛选无可计算单价的记录' },
];

for (const c of CASES) {
  const e = makeEnv();
  try {
    runReport(e, c.tail);
    const el = e.els.get('chart-price');
    const hint = el ? String(el.innerHTML) : '';
    ok(hint.includes(c.expect), `空态「${c.name}」不抛异常且提示正确`);
  } catch (err) {
    ok(false, `空态「${c.name}」抛出异常`, [String(err && err.message || err)]);
  }
}

console.log(failed ? `\n结果: ${failed} 项 FAIL\n` : '\n结果: 全部通过\n');
process.exit(failed ? 1 : 0);
