# report_check — 报告图表 JS 校验

## 用途

报告 `output/report.html` 里的图表逻辑（聚合口径、单价吸附、tooltip 文案、空态）全在前端 JS，
pytest 只能断言"HTML 里有这个 id"，真实逻辑过去只能靠打开浏览器肉眼核对。
本工具在 node 里用最小桩件跑一遍报告内嵌脚本，把这部分变成可重复的检查。

## 运行

```bash
pixi run check-report
# 或指定报告路径
node tools/report_check/check.mjs output/report.html
```

报告需先生成（`pixi run python main.py`，或用 `--no-open` 不弹浏览器）。
找不到报告时退出码 2，检查失败时退出码 1，全部通过退出码 0。

## 依赖

- `node >= 18`（见 `pixi.toml` 的 `nodejs`）
- 不联网、不需要浏览器：DOM 与 ECharts 用桩件替代

## 检查项

只断言稳定不变量，不写死布局数字：

1. 四个图表容器都拿到了 ECharts option（脚本没崩、函数没改名）
2. 均价曲线每个点都落在当日该模型原始单价范围内（±5% 吸附容差）—— 均价口径错了会在这里暴露
3. 均价 tooltip 的类型占比合计 100.00% 且保留 2 位小数
4. 各图表 tooltip 文案不含 `undefined` / `NaN`
5. 空态（筛选无结果 / 记录全无单价）不抛异常，且给出对应空态提示

## 限制

- **不检查排版**：桩件不做布局计算，图例换行、标签遮挡、刻度美观这类问题它看不见，
  只能在浏览器里看。本工具管"数据对不对"，不管"好不好看"。
- 依赖报告脚本作用域内的变量名（`filtered` / `ROWS` / `renderCharts`）。
  这些名字被改会直接报错（会响），但改动 `renderCharts` 的语义时需同步看这里。
- 只覆盖"有数据"与"空态"两端，不覆盖极端数据形态（如单点序列、跨年区间）。
