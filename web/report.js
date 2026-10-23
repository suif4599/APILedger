(function () {
  'use strict';

  const LIGHT_SERIES = [
    '#1f538d', '#2fa572', '#e8a838', '#c0392b',
    '#8e44ad', '#16a085', '#2980b9', '#d35400',
    '#27ae60', '#f39c12', '#7f8c8d', '#2c3e50',
  ];
  const DARK_SERIES = [
    '#5b9bd5', '#3fbf8a', '#f0b445', '#e5736a',
    '#b07fd8', '#2ec4a6', '#4aa8e0', '#e88a4a',
    '#4dd18a', '#f5c542', '#9aa5b1', '#a8b8cc',
  ];

  let DATA = { summary: {}, records: [] };

  // ===========================================
  // Theme (light / dark)
  // ===========================================
  // Chart text is drawn into canvas pixels and cannot be inverted by browser dark mode.
  // DOM colors come from CSS variables; chart colors come from PALETTE below.
  // Themes bind at echarts.init, so switching must dispose and recreate instances.
  const PALETTE = {
    light: {
      series: LIGHT_SERIES,
      cost: '#1f538d', tokens: '#2fa572', avgLine: '#c0392b',
      bucket: { cache_input: '#16a085', input: '#1f538d', output: '#e8a838' },
      note: '#6b7280', tipMuted: '#d1d5db',
      label: '#1a1a1a',
    },
    dark: {
      series: DARK_SERIES,
      cost: '#7fb3e0', tokens: '#3fbf8a', avgLine: '#e5736a',
      bucket: { cache_input: '#2ec4a6', input: '#7fb3e0', output: '#f0b445' },
      note: '#9ca3af', tipMuted: '#9ca3af',
      label: '#ffffff',
    },
  };
  if (typeof echarts !== 'undefined' && echarts.registerTheme) {
    echarts.registerTheme('apiLedgerLight', {});
    const AXIS_DARK = {
      axisLine: { lineStyle: { color: '#2c3340' } },
      axisTick: { lineStyle: { color: '#2c3340' } },
      axisLabel: { color: '#9ca3af' },
      splitLine: { lineStyle: { color: '#2c3340' } },
      nameTextStyle: { color: '#9ca3af' },
    };
    echarts.registerTheme('apiLedgerDark', {
      color: DARK_SERIES,
      categoryAxis: AXIS_DARK, valueAxis: AXIS_DARK,
      legend: { textStyle: { color: '#e5e7eb' } },
      tooltip: {
        backgroundColor: 'rgba(24,28,34,0.95)', borderColor: '#2c3340',
        textStyle: { color: '#e5e7eb' },
      },
    });
  }

  const THEME_KEY = 'apiledger-theme';
  let themeName = 'light';
  let P = PALETTE.light;

  function applyTheme(name) {
    themeName = PALETTE[name] ? name : 'light';
    P = PALETTE[themeName];
    const btn = document.getElementById('btn-theme');
    if (btn) btn.textContent = themeName === 'dark' ? 'Light' : 'Dark';
  }

  function setTheme(name) {
    applyTheme(name);
    document.documentElement.dataset.theme = themeName;
    try { localStorage.setItem(THEME_KEY, themeName); } catch (e) { /* file:// may be read-only */ }
  }

  function toggleTheme() {
    setTheme(themeName === 'dark' ? 'light' : 'dark');
    Object.keys(charts).forEach(id => disposeChart(id));
    renderCharts();
  }

  // ===========================================
  // State
  // ===========================================
  let ROWS = [];
  let filtered = ROWS;
  let page = 1;
  let sortKey = null;           // null = default compound sort (platform, project, date)
  let sortDir = -1;
  let pageSize = 30;            // rows per page (Infinity = all)

  const charts = {};
  const pieMetric = { stack: 'tokens' };
  let trendGran = 'day';
  let selectedCurrency = '';
  let currencyManual = false;

  // Stack order, bottom to top: cache_input, input, output.
  const TYPE_BUCKETS = [
    { key: 'cache_input', label: 'Cache input', match: t => t === 'cache_input' },
    { key: 'input', label: 'Input', match: t => t !== 'cache_input' && t !== 'output' },
    { key: 'output', label: 'Output', match: t => t === 'output' },
  ];
  function typeLabel(t) {
    const bucket = TYPE_BUCKETS.find(b => b.key === t);
    return bucket ? bucket.label : (t || '');
  }

  // ===========================================
  // Formatters
  // ===========================================
  function fmtNum(v) { return Number(v).toLocaleString('en-US', { maximumFractionDigits: 2 }); }
  function money(value, decimals) {
    return Number(value).toLocaleString('en-US', {
      minimumFractionDigits: decimals, maximumFractionDigits: decimals,
    });
  }
  function currencyPrefix() { return selectedCurrency ? selectedCurrency + ' ' : ''; }
  function fmtCost(v) { return currencyPrefix() + money(v, 4); }
  function fmtCost2(v) { return currencyPrefix() + money(v, 2); }
  function fmtTokensM(v) { return (Number(v) / 1e6).toLocaleString('en-US', { minimumFractionDigits: 3, maximumFractionDigits: 3 }) + ' M'; }
  function fmtMetric(v, metric) { return metric === 'cost' ? fmtCost(v) : fmtTokensM(v); }

  // Nice axis: interval is 1/2/5 x 10^n and at least v/steps; max is a multiple of interval.
  function niceAxis(v, steps = 9) {
    if (!(v > 0)) return { max: 1, interval: 1 };
    const raw = v / steps;
    const base = Math.pow(10, Math.floor(Math.log10(raw)));
    let interval = 10 * base;
    for (const m of [1, 2, 5]) {
      if (m * base >= raw) { interval = m * base; break; }
    }
    const max = Math.ceil((v - 1e-9) / interval) * interval;
    return { max: Number(max.toFixed(10)), interval };
  }

  // Detail table unit price: 2 decimals.
  function priceCell(row) {
    if (!row.unit_price) return '-';
    return money(row.unit_price, 2) + '/M';
  }

  // Unit price sequence: constant -> "CNY 2"; price change -> "CNY 2 -> CNY 5".
  function fmtRateSeq(seq) {
    if (!seq || !seq.length) return '';
    const f = v => currencyPrefix() + Number(v).toLocaleString('en-US', { maximumFractionDigits: 4 });
    if (seq.length === 1) return f(seq[0]);
    if (seq.length <= 3) return seq.map(f).join(' -> ');
    return f(seq[0]) + ' -> ... -> ' + f(seq[seq.length - 1]);
  }

  // ===========================================
  // Aggregation helpers (computed from filtered rows)
  // ===========================================
  function groupSum(rows, field, valueKey) {
    const map = new Map();
    for (const r of rows) {
      const k = r[field] || '(empty)';
      map.set(k, (map.get(k) || 0) + Number(r[valueKey] || 0));
    }
    return [...map.entries()]
      .map(([name, value]) => ({ name, value }))
      .sort((a, b) => b.value - a.value);
  }

  function dailySum(rows, valueKey) {
    const map = new Map();
    for (const r of rows) {
      const d = String(r.bill_start).slice(0, 10);
      if (!d) continue;
      map.set(d, (map.get(d) || 0) + Number(r[valueKey] || 0));
    }
    return [...map.entries()]
      .map(([period, total]) => ({ period, total }))
      .sort((a, b) => a.period.localeCompare(b.period));
  }

  // Group by platform x model, bucket types into cache_input/input/output, and
  // accumulate tokens and cost. Order is fixed by token total (Top 15) regardless
  // of the selected metric. priceCost = sum(unit_price * tokens).
  function typeStackGroups() {
    const map = new Map();
    // Ascending by date so the price sequence follows time order.
    const rows = [...filtered].sort((a, b) => String(a.bill_start).localeCompare(String(b.bill_start)));
    for (const r of rows) {
      const label = r.platform ? `${r.platform} / ${r.model || '(empty)'}` : (r.model || '(empty)');
      let g = map.get(label);
      if (!g) {
        g = { label, tokens: new Map(), cost: new Map(), tokenTotal: 0, costTotal: 0,
              priceCost: 0, priceSeq: {} };
        map.set(label, g);
      }
      const tokens = Number(r.tokens || 0);
      const bucket = TYPE_BUCKETS.find(b => b.match(r.type || '')).key;
      g.tokens.set(bucket, (g.tokens.get(bucket) || 0) + tokens);
      g.cost.set(bucket, (g.cost.get(bucket) || 0) + Number(r.cost || 0));
      g.tokenTotal += tokens;
      g.costTotal += Number(r.cost || 0);
      const price = Number(r.unit_price || 0);
      g.priceCost += price * tokens;
      if (price > 0) {                     // rows with no price stay out of the sequence
        const seq = g.priceSeq[bucket] || (g.priceSeq[bucket] = []);
        if (seq.indexOf(price) === -1) seq.push(price);
      }
    }
    return [...map.values()]
      .filter(g => g.tokenTotal > 0 || g.costTotal > 0)
      .sort((a, b) => b.tokenTotal - a.tokenTotal)
      .slice(0, 15);
  }

  function periodKey(d, gran) {
    const p = n => String(n).padStart(2, '0');
    if (gran === 'month') return d.getFullYear() + '-' + p(d.getMonth() + 1);
    if (gran === 'week') {
      const y = d.getFullYear();
      const week = Math.ceil((d - new Date(y, 0, 1)) / 86400000 / 7);
      return y + '-W' + String(week).padStart(2, '0');
    }
    return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate());
  }

  function periodSum(rows, valueKey, gran) {
    const map = new Map();
    let minDate = '', maxDate = '';
    for (const r of rows) {
      const p = String(r.bill_start).slice(0, 10);
      if (!p) continue;
      const key = periodKey(new Date(p + 'T00:00:00'), gran);
      map.set(key, (map.get(key) || 0) + Number(r[valueKey] || 0));
      if (!minDate || p < minDate) minDate = p;
      if (!maxDate || p > maxDate) maxDate = p;
    }
    // Fill gaps day by day (periods without usage count as 0).
    // Start/end must use raw dates: period keys like 2026-W18 are not valid dates.
    if (!minDate) return { keys: [], values: [] };
    const start = new Date(minDate + 'T00:00:00');
    const end = new Date(maxDate + 'T00:00:00');
    const keys = [], values = [];
    for (let d = start; d <= end; d.setDate(d.getDate() + 1)) {
      const k = periodKey(d, gran);
      if (keys.length === 0 || keys[keys.length - 1] !== k) {
        keys.push(k);
        values.push(map.get(k) || 0);
      }
    }
    return { keys, values };
  }

  function disposeChart(id) {
    if (charts[id]) {
      try { charts[id].dispose(); } catch (e) { /* already disposed */ }
      delete charts[id];
    }
  }

  function showEmpty(el, msg) {
    disposeChart(el.id);
    el.dataset.hadContent = 1;
    el.innerHTML = '<div class="chart-empty">' + msg + '</div>';
  }

  function getChart(id, el) {
    if (typeof echarts === 'undefined') return null;
    if (el.dataset.hadContent) {
      // The empty state cleared the container (canvas gone), so recreate the instance.
      el.innerHTML = '';
      delete el.dataset.hadContent;
      disposeChart(id);
    }
    if (!charts[id]) charts[id] = echarts.init(el, 'apiLedger' + (themeName === 'dark' ? 'Dark' : 'Light'), { locale: 'EN' });
    return charts[id];
  }

  // ===========================================
  // Summary cards (follow the filters)
  // ===========================================
  function renderSummary() {
    const totalCost = filtered.reduce((s, r) => s + Number(r.cost || 0), 0);
    const totalTokens = filtered.reduce((s, r) => s + Number(r.tokens || 0), 0);
    const models = new Set(filtered.map(r => r.model).filter(Boolean));
    const platforms = new Set(filtered.map(r => r.platform).filter(Boolean));
    const dates = filtered.map(r => String(r.bill_start).slice(0, 10)).filter(Boolean).sort();

    document.getElementById('c-cost').textContent = money(totalCost, 2);
    document.getElementById('c-tokens').textContent = fmtTokensM(totalTokens);
    document.getElementById('c-records').textContent = fmtNum(filtered.length);
    document.getElementById('c-models').textContent = fmtNum(models.size);
    document.getElementById('c-platforms').textContent = fmtNum(platforms.size);
    document.getElementById('date-hint').textContent =
      (dates.length ? dates[0] + ' ~ ' + dates[dates.length - 1] : 'No data') +
      ` | ${platforms.size} platforms | ${models.size} models` +
      (filtered.length !== ROWS.length ? ` (filtered ${filtered.length} / ${ROWS.length} rows)` : '');
  }

  // ===========================================
  // Trend: cost + tokens on two axes (day/week/month)
  // ===========================================
  function renderTrend() {
    const el = document.getElementById('chart-trend');
    if (!filtered.length) { showEmpty(el, 'No data for the current filters'); return; }
    const chart = getChart('trend', el);
    if (!chart) return;

    const cost = periodSum(filtered, 'cost', trendGran);
    const tokens = periodSum(filtered, 'tokens', trendGran);
    const keys = cost.keys.length >= tokens.keys.length ? cost.keys : tokens.keys;
    const costMap = new Map(cost.keys.map((k, i) => [k, cost.values[i]]));
    const tokenMap = new Map(tokens.keys.map((k, i) => [k, tokens.values[i]]));
    const costVals = keys.map(k => costMap.get(k) || 0);
    const tokenVals = keys.map(k => tokenMap.get(k) || 0);
    const avg = costVals.length ? costVals.reduce((a, b) => a + b, 0) / costVals.length : 0;

    chart.setOption({
      tooltip: {
        trigger: 'axis',
        formatter: params => {
          let s = params[0].axisValue;
          for (const p of params) {
            const v = p.seriesName === 'Cost' ? fmtCost(p.value) : fmtTokensM(p.value);
            s += '<br>' + p.marker + ' ' + p.seriesName + ': ' + v;
          }
          return s;
        },
      },
      legend: { top: 0, left: 'center' },
      grid: { left: 70, right: 70, top: 56, bottom: 72 },
      xAxis: { type: 'category', data: keys, axisLabel: { rotate: 45, fontSize: 10 } },
      yAxis: [
        { type: 'value', name: 'Cost', axisLabel: { fontSize: 10 } },
        { type: 'value', name: 'Tokens (M)', splitLine: { show: false },
          axisLabel: { fontSize: 10, formatter: v => (v / 1e6).toLocaleString('en-US', { maximumFractionDigits: 3 }) } },
      ],
      series: [
        {
          name: 'Cost', type: 'line', data: costVals, smooth: true,
          itemStyle: { color: P.cost }, areaStyle: { opacity: 0.08 },
          markLine: avg ? { data: [{ yAxis: avg }], silent: true, symbol: 'none',
            lineStyle: { type: 'dashed', color: P.avgLine },
            label: { formatter: 'Average', color: P.label, textBorderWidth: 0 } } : undefined,
        },
        {
          name: 'Tokens', type: 'line', yAxisIndex: 1, data: tokenVals, smooth: true,
          itemStyle: { color: P.tokens }, areaStyle: { opacity: 0.08 },
        },
      ],
    }, true);
  }

  // Model token / cost comparison: two horizontal bars per model,
  // axis in percent of total, absolute values on the right.
  function renderModelBar() {
    const el = document.getElementById('chart-model');
    if (!filtered.length) { showEmpty(el, 'No data for the current filters'); return; }
    const chart = getChart('model', el);
    if (!chart) return;
    const map = new Map();
    for (const r of filtered) {
      const k = r.model || '(empty)';
      const g = map.get(k) || { tokens: 0, cost: 0 };
      g.tokens += Number(r.tokens || 0);
      g.cost += Number(r.cost || 0);
      map.set(k, g);
    }
    const groups = [...map.entries()]
      .map(([name, v]) => ({ name, ...v }))
      .sort((a, b) => b.tokens - a.tokens);
    const totalTokens = groups.reduce((s, g) => s + g.tokens, 0);
    const totalCost = groups.reduce((s, g) => s + g.cost, 0);
    // Category index 0 is at the bottom; reverse so the largest model is on top.
    const rows = [...groups].reverse();
    chart.setOption({
      legend: { bottom: 0 },
      tooltip: {
        trigger: 'item', axisPointer: { type: 'shadow' },
        formatter: p => {
          const g = rows[p.dataIndex];
          if (!g) return '';
          const tp = totalTokens ? (g.tokens / totalTokens * 100).toFixed(2) : '0.00';
          const cp = totalCost ? (g.cost / totalCost * 100).toFixed(2) : '0.00';
          return '<b>' + g.name + '</b><br>'
               + 'Tokens: ' + fmtTokensM(g.tokens) + ' (' + tp + '%)<br>'
               + 'Cost: ' + fmtCost2(g.cost) + ' (' + cp + '%)';
        },
      },
      grid: { left: 130, right: 110, top: 20, bottom: 40 },
      // Axis max is a nice tick near the longest bar, not always 0-100%.
      xAxis: (() => {
        const maxPct = rows.reduce((m, g) =>
          Math.max(m, totalTokens ? g.tokens / totalTokens * 100 : 0,
                       totalCost ? g.cost / totalCost * 100 : 0), 0);
        const a = niceAxis(maxPct);
        return { type: 'value', max: a.max, interval: a.interval,
                 axisLabel: { formatter: '{value}%', fontSize: 10 } };
      })(),
      yAxis: { type: 'category', data: rows.map(g => g.name), axisLabel: { fontSize: 10 } },
      series: [
        {
          name: 'Tokens', type: 'bar',
          data: rows.map(g => totalTokens ? g.tokens / totalTokens * 100 : 0),
          itemStyle: { color: P.tokens, borderRadius: [0, 4, 4, 0] },
          barMaxWidth: 12,
          label: { show: true, position: 'right', color: P.label,
                   formatter: p => fmtTokensM(rows[p.dataIndex].tokens), fontSize: 10 },
        },
        {
          name: 'Cost', type: 'bar',
          data: rows.map(g => totalCost ? g.cost / totalCost * 100 : 0),
          itemStyle: { color: P.cost, borderRadius: [0, 4, 4, 0] },
          barMaxWidth: 12,
          label: { show: true, position: 'right', color: P.label,
                   formatter: p => fmtCost2(rows[p.dataIndex].cost), fontSize: 10 },
        },
      ],
    }, true);
  }

  // Model composition: platform x model 100% stacked bars.
  function renderStack() {
    const el = document.getElementById('chart-stack');
    const metric = pieMetric.stack;
    const groups = typeStackGroups();
    if (!groups.length) { showEmpty(el, 'No data for the current filters'); return; }
    const chart = getChart('stack', el);
    if (!chart) return;

    const bucketMap = metric === 'cost' ? 'cost' : 'tokens';
    const totalOf = g => metric === 'cost' ? g.costTotal : g.tokenTotal;
    // Weighted average price (CNY per 1M tokens): sum(unit_price * tokens) / sum(tokens).
    const avgRateText = g => {
      if (!g.tokenTotal) return '';
      const rate = g.priceCost / g.tokenTotal;
      return currencyPrefix() + rate.toLocaleString('en-US', { minimumFractionDigits: 3, maximumFractionDigits: 3 }) + '/M';
    };
    // Show the average label once, next to the topmost non-zero segment.
    const labelFor = (bucketIdx) => ({
      show: true, position: 'right', fontSize: 11, color: P.note,
      formatter: p => {
        const g = groups[groups.length - 1 - p.dataIndex];
        if (!g) return '';
        const vals = TYPE_BUCKETS.map(b => g[bucketMap].get(b.key) || 0);
        let topIdx = -1;
        for (let i = 0; i < vals.length; i++) if (vals[i] > 0) topIdx = i;
        return topIdx === bucketIdx ? avgRateText(g) : '';
      },
    });

    // Three percentage segments per row (each row totals 100).
    const series = TYPE_BUCKETS.map((b, i) => ({
      name: b.label, type: 'bar', stack: 'total', barMaxWidth: 22,
      data: groups.map(g => {
        const v = g[bucketMap].get(b.key) || 0;
        const total = totalOf(g);
        return total ? +(v * 100 / total).toFixed(2) : 0;
      }).reverse(),
      itemStyle: { color: P.bucket[b.key] },
      label: labelFor(i),
    }));
    const labels = groups.map(g => g.label).reverse();

    chart.setOption({
      tooltip: {
        trigger: 'axis', axisPointer: { type: 'shadow' },
        formatter: params => {
          let s = params[0].axisValue;
          for (const p of params) {
            // Series data was reversed; use the reverse index to get the original group.
            const g = groups[groups.length - 1 - p.dataIndex];
            const bucketKey = TYPE_BUCKETS[p.seriesIndex] ? TYPE_BUCKETS[p.seriesIndex].key : p.seriesName;
            const abs = g ? (g[bucketMap].get(bucketKey) || 0) : 0;
            s += '<br>' + p.marker + ' ' + p.seriesName + ': ' + fmtMetric(abs, metric) + ' (' + p.value + '%)';
            // Unit price: one value when constant, "CNY 2 -> CNY 5" across a price change.
            const rate = g ? fmtRateSeq(g.priceSeq[bucketKey]) : '';
            if (rate) s += ' <span class="tip-muted">| Unit price ' + rate + '</span>';
          }
          const g = groups[groups.length - 1 - params[0].dataIndex];
          if (!g) return s;
          return s + '<br><span class="tip-muted">Average: ' + avgRateText(g) + '</span>';
        },
      },
      legend: { bottom: 0 },
      grid: { left: 130, right: 120, top: 20, bottom: 40 },
      xAxis: { type: 'value', max: 100, axisLabel: { formatter: '{value}%', fontSize: 10 } },
      yAxis: { type: 'category', data: labels, axisLabel: { fontSize: 10 } },
      series,
    }, true);
  }

  // Daily average price per model: one line per model, expanding renderStack's average by day.
  // y = daily sum(unit_price * tokens) / sum(tokens) (CNY per 1M tokens), the same
  // basis as the composition chart, split by day. Rows without a price count in
  // neither numerator nor denominator. A model used on several platforms becomes
  // one line per platform; days without data are gaps (lines break there).
  function renderAvgPrice() {
    const el = document.getElementById('chart-price');
    if (!filtered.length) { showEmpty(el, 'No data for the current filters'); return; }

    // Only prefix the platform when a model appears on more than one platform.
    const platforms = new Map();
    for (const r of filtered) {
      const m = r.model || '(empty)';
      if (!platforms.has(m)) platforms.set(m, new Set());
      platforms.get(m).add(r.platform || '');
    }
    const labelOf = r => {
      const m = r.model || '(empty)';
      return (platforms.get(m).size > 1 && r.platform) ? r.platform + ' / ' + m : m;
    };

    const dates = new Set();
    const seriesMap = new Map();   // label -> Map(date -> {num, den, buckets})
    for (const r of filtered) {
      const tokens = Number(r.tokens || 0);
      const d = String(r.bill_start).slice(0, 10);
      const price = Number(r.unit_price || 0);
      if (!d || !(tokens > 0) || !(price > 0)) continue;
      const label = labelOf(r);
      let byDate = seriesMap.get(label);
      if (!byDate) { byDate = new Map(); seriesMap.set(label, byDate); }
      // Buckets follow TYPE_BUCKETS order; the tooltip shows their shares.
      const g = byDate.get(d) || { num: 0, den: 0, buckets: [] };
      g.num += price * tokens;
      g.den += tokens;
      const bi = TYPE_BUCKETS.findIndex(b => b.match(r.type || ''));
      g.buckets[bi] = (g.buckets[bi] || 0) + tokens;
      byDate.set(d, g);
      dates.add(d);
    }
    if (!dates.size) { showEmpty(el, 'No rows with a price for the current filters'); return; }

    const chart = getChart('price', el);
    if (!chart) return;
    const dateList = [...dates].sort();
    // Lines ordered by token total descending, consistent with the legend and charts.
    const lines = [...seriesMap.entries()]
      .map(([name, byDate]) => ({ name, byDate,
        tokens: [...byDate.values()].reduce((s, g) => s + g.den, 0) }))
      .sort((a, b) => b.tokens - a.tokens);
    const fmtPrice3 = v => currencyPrefix() + Number(v).toLocaleString('en-US', { minimumFractionDigits: 3, maximumFractionDigits: 3 }) + '/M';
    // Tooltip extra line: this model's token type shares for the day (2 decimals).
    const shareLine = g => {
      if (!g || !g.den) return '';
      const parts = TYPE_BUCKETS
        .map((b, i) => ({ label: b.label, n: g.buckets[i] || 0 }))
        .filter(x => x.n > 0)
        .map(x => x.label + ' ' + (x.n / g.den * 100).toFixed(2) + '%');
      return parts.length ? '<br><span class="tip-muted"> ' + parts.join(' | ') + '</span>' : '';
    };
    // The legend wraps to multiple rows; reserve bottom space so it does not cover date labels.
    const legendRows = Math.ceil(lines.length /
      Math.max(1, Math.floor((el.clientWidth || 900) / 170)));

    chart.setOption({
      tooltip: {
        trigger: 'axis',
        formatter: params => {
          const date = params[0].axisValue;
          let s = date;
          for (const p of params) {           // skip lines with no data on this day
            if (p.value == null) continue;
            s += '<br>' + p.marker + ' ' + p.seriesName + ': ' + fmtPrice3(p.value);
            const byDate = seriesMap.get(p.seriesName);
            s += shareLine(byDate ? byDate.get(date) : null);
          }
          return s;
        },
      },
      // Legend text color comes from the theme; only the font size is set here.
      legend: { bottom: 0, type: 'plain', itemGap: 10, textStyle: { fontSize: 11 } },
      grid: { left: 70, right: 30, top: 46, bottom: 72 + legendRows * 22 },
      xAxis: { type: 'category', data: dateList, axisLabel: { rotate: 45, fontSize: 10 } },
      yAxis: { type: 'value', name: 'Avg price /M',
               axisLabel: { fontSize: 10,
                 formatter: v => Number(v).toLocaleString('en-US', { maximumFractionDigits: 3 }) } },
      series: lines.map((l, i) => ({
        name: l.name, type: 'line', smooth: true, connectNulls: false,
        symbolSize: 5,
        itemStyle: { color: P.series[i % P.series.length] },
        data: dateList.map(d => {
          const g = l.byDate.get(d);
          return g && g.den ? +(g.num / g.den).toFixed(3) : null;
        }),
      })),
    }, true);
  }

  function renderCharts() {
    renderTrend();
    renderModelBar();
    renderStack();
    renderAvgPrice();
  }

  // ===========================================
  // Filters
  // ===========================================
  function applyFilters() {
    const start = document.getElementById('f-start').value;
    const end = document.getElementById('f-end').value;
    const pf = document.getElementById('f-platform').value;
    const pjf = document.getElementById('f-project').value;
    const mf = document.getElementById('f-model').value;
    const tf = document.getElementById('f-type').value;
    const kw = document.getElementById('f-keyword').value.trim().toLowerCase();

    filtered = ROWS.filter(r => {
      if (selectedCurrency && (r.currency || '') !== selectedCurrency) return false;
      if (start && r.bill_start.slice(0, 10) < start) return false;
      if (end && r.bill_start.slice(0, 10) > end) return false;
      if (pf && r.platform !== pf) return false;
      if (pjf && r.project !== pjf) return false;
      if (mf && r.model !== mf) return false;
      if (tf && r.type !== tf) return false;
      if (kw && !(r.model + r.project + r.platform + r.type).toLowerCase().includes(kw)) return false;
      return true;
    });
    page = 1;
    renderAll();
  }

  function resetFilters() {
    ['f-keyword'].forEach(id => document.getElementById(id).value = '');
    ['f-platform', 'f-project', 'f-model', 'f-type'].forEach(id => {
      const s = document.getElementById(id); s.selectedIndex = 0;
    });
    applyPreset('1m');  // default range: last month
  }

  function buildOptions(selId, key) {
    const set = new Set(ROWS.map(r => r[key]).filter(Boolean));
    const sel = document.getElementById(selId);
    for (const v of [...set].sort((a, b) => String(a).localeCompare(String(b), 'en-US'))) {
      const opt = document.createElement('option');
      opt.value = v;
      opt.textContent = key === 'type' ? typeLabel(v) : v;
      sel.appendChild(opt);
    }
  }

  // Quick ranges are relative to the latest date in the data; default is last month.
  function fmtDate(d) {
    const p = n => String(n).padStart(2, '0');
    return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate());
  }

  function maxBillDate() {
    return ROWS.reduce((m, r) => {
      const d = String(r.bill_start).slice(0, 10);
      return d > m ? d : m;
    }, '');
  }

  function markPreset(range) {
    document.querySelectorAll('#quick-presets .preset').forEach(b => {
      b.classList.toggle('active', b.dataset.range === range);
    });
  }

  function applyPreset(range) {
    if (range === 'all') {  // clear the date range
      document.getElementById('f-start').value = '';
      document.getElementById('f-end').value = '';
      markPreset(range);
      applyFilters();
      return;
    }
    const maxDate = maxBillDate();
    if (!maxDate) { markPreset(range); return; }
    const d = new Date(maxDate + 'T00:00:00');
    if (range === '7d') {
      d.setDate(d.getDate() - 7);
    } else {
      const months = { '1m': 1, '3m': 3, '6m': 6, '1y': 12 }[range] || 1;
      d.setMonth(d.getMonth() - months);
    }
    d.setDate(d.getDate() + 1);  // include the end point
    document.getElementById('f-start').value = fmtDate(d);
    document.getElementById('f-end').value = maxDate;
    markPreset(range);
    applyFilters();
  }

  // ===========================================
  // Detail table: default compound sort (platform, project, date) / column sort / paging
  // ===========================================
  function sortedRows() {
    const rows = [...filtered];
    if (sortKey === null) {
      rows.sort((a, b) =>
        String(a.bill_start).localeCompare(String(b.bill_start)) ||
        String(a.platform).localeCompare(String(b.platform), 'en-US') ||
        String(a.project).localeCompare(String(b.project), 'en-US') ||
        String(a.model).localeCompare(String(b.model), 'en-US') ||
        String(a.type).localeCompare(String(b.type), 'en-US'));
      return rows;
    }
    rows.sort((a, b) => {
      const va = a[sortKey], vb = b[sortKey];
      if (typeof va === 'number' && typeof vb === 'number') return (va - vb) * sortDir;
      return String(va || '').localeCompare(String(vb || ''), 'en-US') * sortDir;
    });
    return rows;
  }

  function changePage(delta) {
    page += delta;
    renderTable();
  }

  function renderTable() {
    const rows = sortedRows();
    const totalPages = pageSize === Infinity ? 1 : Math.max(1, Math.ceil(rows.length / pageSize));
    page = Math.max(1, Math.min(page, totalPages));
    document.getElementById('table-count').textContent = `${rows.length} rows`;
    document.getElementById('page-info').textContent = `${page} / ${totalPages}`;
    document.getElementById('btn-prev').disabled = page <= 1;
    document.getElementById('btn-next').disabled = page >= totalPages;

    const start = pageSize === Infinity ? 0 : (page - 1) * pageSize;
    const slice = rows.slice(start, pageSize === Infinity ? undefined : start + pageSize);
    const tbody = document.getElementById('table-body');
    tbody.innerHTML = '';
    if (!slice.length) {
      const tr = document.createElement('tr');
      tr.innerHTML = '<td colspan="9" class="table-empty">No matching data</td>';
      tbody.appendChild(tr);
      return;
    }
    for (const r of slice) {
      const tr = document.createElement('tr');
      tr.innerHTML = `
        <td>${String(r.bill_start).slice(0, 10)}</td>
        <td>${r.platform}</td>
        <td>${r.project}</td>
        <td>${r.model}</td>
        <td>${typeLabel(r.type)}</td>
        <td class="num">${fmtNum(r.tokens)}</td>
        <td class="num">${money(r.cost, 4)}</td>
        <td class="num">${priceCell(r)}</td>
        <td class="muted-cell">${String(r.source_file || '').split(',').map(s => s.trim()).filter(Boolean).join(' ')}</td>`;
      tbody.appendChild(tr);
    }
  }

  // ===========================================
  // Event wiring
  // ===========================================
  let eventsBound = false;

  function initEvents() {
    if (eventsBound) return;
    eventsBound = true;

    // The site shell toggles the theme; rebuild canvas charts when its attribute changes.
    if (typeof MutationObserver !== 'undefined') {
      new MutationObserver(() => {
        const name = document.documentElement.dataset.theme === 'light' ? 'light' : 'dark';
        if (name !== themeName) {
          applyTheme(name);
          Object.keys(charts).forEach(id => disposeChart(id));
          renderCharts();
        }
      }).observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
    }

    const themeBtn = document.getElementById('btn-theme');
    if (themeBtn) themeBtn.addEventListener('click', toggleTheme);

  // Header sorting
  document.querySelectorAll('th[data-k]').forEach(th => {
    th.addEventListener('click', () => {
      const k = th.dataset.k;
      if (sortKey === k) sortDir *= -1;
      else { sortKey = k; sortDir = -1; }
      document.querySelectorAll('th .arrow').forEach(a => a.remove());
      const arrow = document.createElement('span');
      arrow.className = 'arrow';
      arrow.textContent = sortDir === -1 ? ' v' : ' ^';
      th.appendChild(arrow);
      page = 1;
      renderTable();
    });
  });

  // Trend granularity
  document.querySelectorAll('#tabs-gran button').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('#tabs-gran button').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      trendGran = btn.dataset.g;
      renderTrend();
    });
  });

  // Stack metric switch
  [['tabs-stack', 'stack', renderStack]].forEach(([tabsId, key, render]) => {
    document.querySelectorAll('#' + tabsId + ' button').forEach(btn => {
      btn.addEventListener('click', () => {
        document.querySelectorAll('#' + tabsId + ' button').forEach(b => b.classList.remove('active'));
        btn.classList.add('active');
        pieMetric[key] = btn.dataset.m;
        render();
      });
    });
  });

  // Rows per page ("all" = no paging)
  document.getElementById('f-page-size').addEventListener('change', e => {
    pageSize = e.target.value === 'all' ? Infinity : Number(e.target.value);
    page = 1;
    renderTable();
  });

  document.querySelectorAll('#quick-presets .preset').forEach(btn => {
    btn.addEventListener('click', () => applyPreset(btn.dataset.range));
  });

  // Filters: selects and dates apply immediately; keyword applies on Enter or Apply.
  ['f-start', 'f-end', 'f-platform', 'f-project', 'f-model', 'f-type'].forEach(id => {
    document.getElementById(id).addEventListener('change', () => {
      if (id === 'f-start' || id === 'f-end') markPreset(null);  // manual dates clear the preset
      applyFilters();
    });
  });
  document.getElementById('f-keyword').addEventListener('keydown', e => {
    if (e.key === 'Enter') applyFilters();
  });
  document.getElementById('btn-apply').addEventListener('click', applyFilters);
  document.getElementById('btn-reset').addEventListener('click', resetFilters);

  window.addEventListener('resize', () => {
    Object.values(charts).forEach(c => c.resize());
    renderAvgPrice();   // legend rows and bottom padding depend on container width
  });
  }

  // ===========================================
  // Initialization
  // ===========================================
  function renderAll() {
    renderSummary();
    renderCharts();
    renderTable();
  }

  function rebuildOptions() {
    for (const selId of ['f-platform', 'f-project', 'f-model', 'f-type']) {
      const sel = document.getElementById(selId);
      while (sel.options.length > 1) sel.remove(1);
    }
    buildOptions('f-platform', 'platform');
    buildOptions('f-project', 'project');
    buildOptions('f-model', 'model');
    buildOptions('f-type', 'type');
  }

  function currencyList() {
    return [...new Set(ROWS.map(r => r.currency || '').filter(Boolean))].sort();
  }

  function updateCurrencyNotes() {
    document.querySelectorAll('[data-currency-note]').forEach(el => {
      el.textContent = selectedCurrency ? `(${selectedCurrency})` : '';
    });
  }

  function rebuildCurrency() {
    const tabs = document.getElementById('currency-tabs');
    const row = document.getElementById('currency-row');
    const list = currencyList();
    if (!tabs || !row) return;
    row.hidden = list.length === 0;
    if (!list.length) {
      selectedCurrency = '';
      currencyManual = false;
      tabs.textContent = '';
      updateCurrencyNotes();
      return;
    }
    if (!currencyManual || !list.includes(selectedCurrency)) {
      selectedCurrency = list.length === 1 ? list[0] : (list.includes('USD') ? 'USD' : list[0]);
    }
    tabs.textContent = '';
    for (const code of list) {
      const button = document.createElement('button');
      button.type = 'button';
      button.textContent = code;
      if (code === selectedCurrency) button.className = 'active';
      button.addEventListener('click', () => {
        selectedCurrency = code;
        currencyManual = true;
        tabs.querySelectorAll('button').forEach(b => b.classList.toggle('active', b.textContent === code));
        updateCurrencyNotes();
        applyFilters();
      });
      tabs.appendChild(button);
    }
    updateCurrencyNotes();
  }

  function mount(data) {
    DATA = data || { summary: {}, records: [] };
    ROWS = DATA.records || [];
    initEvents();
    applyTheme(document.documentElement.dataset.theme === 'light' ? 'light' : 'dark');
    rebuildOptions();
    currencyManual = false;
    rebuildCurrency();
    applyPreset('1m');
  }

  function update(data) {
    DATA = data || { summary: {}, records: [] };
    ROWS = DATA.records || [];
    rebuildOptions();
    rebuildCurrency();
    applyPreset('1m');
  }

  window.APILedgerReport = {
    mount,
    update,
    _test: {
      rows: () => ROWS,
      setFiltered: value => { filtered = value; },
      renderCharts,
      toggleTheme,
      palette: PALETTE,
      lightSeries: LIGHT_SERIES,
      darkSeries: DARK_SERIES,
    },
  };
})();
