# APILedger

API bill ledger CLI: import a zip archive of platform bills, store everything in SQLite, and generate a self-contained HTML report that opens in your browser.

## Installation

### 1. Add the input to your flake

```nix
# flake.nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    home-manager.url = "github:nix-community/home-manager";
    apiledger.url = "github:<owner>/APILedger";
  };
}
```

### 2. Import the module file

```nix
{ inputs, ... }:
{
  imports = [ inputs.apiledger.homeManagerModules.default ];
}
```

`inputs.apiledger.homeManagerModules.default` is the same module as the file `nix/hm.nix`, so importing the path directly works too:

```nix
imports = [ "${inputs.apiledger}/nix/hm.nix" ];
```

### 3. Example `home.nix`

```nix
{ inputs, ... }:
{
  imports = [ inputs.apiledger.homeManagerModules.default ];

  programs.apiledger = {
    enable = true;
    # dataDir = "${config.xdg.dataHome}/apiledger";  # default
    # presets = ./my-presets;                        # optional extra presets
  };
}
```

Module options:

| Option | Default | Description |
| :--- | :--- | :--- |
| `programs.apiledger.enable` | `false` | Install the CLI and set `APILEDGER_ROOT`. |
| `programs.apiledger.package` | flake package | Package to install. |
| `programs.apiledger.dataDir` | `${config.xdg.dataHome}/apiledger` | Persistent ledger database, default report, cache, and user presets. |
| `programs.apiledger.presets` | `null` | Optional directory of extra JSON presets, linked to `<dataDir>/presets`. |

The package can also be used without home-manager:

```nix
environment.systemPackages = [ inputs.apiledger.packages.${pkgs.system}.default ];
```

or directly:

```bash
nix run github:<owner>/APILedger -- usage_data.zip
nix build github:<owner>/APILedger
```

## Usage

```bash
apiledger usage_data_2026-09-21_2026-10-20.zip

# write to a file without opening anything
apiledger usage_data_2026-09-21_2026-10-20.zip -o report.html
```

The archive may contain `.csv` and `.xlsx` bills. Every entry is matched against the platform presets; matching bills are imported, other entries (for example a cost summary next to a token-detail file) are skipped.

Import behavior:

- **New bills replace overlapping old bills**: bills of the same platform whose filename date ranges overlap are removed before the new data is written (re-exports and corrections can simply be imported again).
- Bills of the same platform with non-overlapping date ranges are complementary data and are accumulated.
- Re-importing the same archive is idempotent.
- Files in the archive are read directly; there is no input directory to manage.
- Without `-o` the report is written to `$APILEDGER_ROOT/data/report.html` and opened with the system default browser.
- With `-o outfile.html` the report is written to that path and **not** opened, which is what scripts should use.

### CLI options

| Option | Behavior |
| :--- | :--- |
| `-o <file>` | Write the report to `<file>` without opening it |
| `-h`, `--help` | Show help |

Bills must match a platform preset, otherwise the entry is skipped with a notice pointing at `presets/_template.json`.

## HTML report

The report is a **single self-contained HTML file**: all rows are embedded as JSON, ECharts is inlined (downloaded once into `$APILEDGER_ROOT/.cache/echarts.min.js`, with a CDN fallback), no server required, works offline.

- **Filter bar** (date range / platform / project / model / type / keyword) — filters the summary cards, all charts, and the detail table together
- **Summary cards**: total cost / total tokens / record count / model count / platform count (follow the filters)
- **Charts** (all recomputed in the browser from the filtered rows):
  - Usage trend: cost + tokens on two axes, daily/weekly/monthly granularity, averages
  - Model token / cost comparison: two horizontal bars per model, axis in percent of total, absolute values on the right
  - Model token / cost composition: platform × model 100% stacked bars (cache input / input / output) with the price per 1M tokens on the right; hover shows type shares and unit prices, with price changes shown as `CNY 2 -> CNY 5`
  - Model average price over time: one line per model, daily weighted average `Σ(unit price × tokens) / Σtokens` per 1M tokens, same basis as the composition chart; hover shows type shares; a model used on several platforms is split per platform
- **Detail table**: sorted by platform → project → date by default, clickable column sorting and pagination; columns are date / platform / project / model / type / tokens / cost / unit price / source file
- **Light / dark theme**: follows the system by default (`prefers-color-scheme`), toggled in the top-right corner and remembered in `localStorage`. Chart text lives in canvas pixels, so the report ships its own dark palette (CSS variables plus an ECharts theme)

## Input format

The repository's `usage_data_2026-09-21_2026-10-20.zip` is an example archive: it contains an `amount-*.csv` token detail file and a `cost-*.csv` daily cost summary.
The DeepSeek preset matches the detail file (`price × amount` reproduces the summary exactly) and the summary is skipped as unrecognized auxiliary data.

### Platform presets

A preset is a JSON file: the bundled ones live in the package under `share/apiledger/presets/`, and user presets can be added to `$APILEDGER_ROOT/presets/` (same filename overrides a bundled preset).

The program matches a bill by filename keyword first, then by header keywords; a bill matching no preset is skipped. See `presets/_template.json` for a complete starting point.

| Field | Required | Description |
| :--- | :---: | :--- |
| `name` | no | Preset name shown in the log (defaults to the filename stem) |
| `match_filename` | yes | Filename keywords; any one hit makes the preset a candidate (an empty list matches nothing by filename) |
| `match_headers` | no | Header keywords; all must be present for the preset to match |
| `column_mapping` | yes | Standard field → list of header keywords (substring, case/space-insensitive, longest keyword first) |
| `defaults` | no | Fixed values for fields missing from the bill (e.g. `platform`) |
| `type_map` | no | Raw type → standard type |
| `skip_types` | no | Raw types whose rows are skipped entirely |
| `computed` | no | Field → arithmetic expression (`+ - * / ( )` and field names), evaluated in declaration order |
| `model_map` | no | Raw model name → unified model name |
| `parse` | no | Row-level rules (see below) |

`parse` supports:

| Rule | Description |
| :--- | :--- |
| `skip_if` | `{ "field": ["value", ...] }` — skip the row when the mapped field (or a raw column with that name) equals one of the values |
| `extract` | `{ "column", "pattern", "group_type", "group_tokens" }` — parse type and tokens out of a single cell with a regular expression; no match skips the row |
| `date_format` | `{ "field", "from": "yyyyMMdd" }` — convert a compact date to `YYYY-MM-DD` |

### Standard fields

| Field | Type | Description | Format |
| :---: | :---: | :--- | :--- |
| `bill_start` | TEXT | Bill start time | `yyyy-MM-dd` or `yyyy-MM-dd HH:mm:ss` |
| `platform` | TEXT | Platform name | Any text |
| `project` | TEXT | Project name | Any text |
| `model` | TEXT | Model name | Any text |
| `type` | TEXT | Billing type | `cache_input` / `input` / `output` |
| `tokens` | INTEGER | Token count | Non-negative integer |
| `cost` | REAL | Amount | Non-negative float, CNY |
| `unit_price` | REAL | Unit price | Non-negative float, CNY per 1M tokens |

Timestamps are truncated to the day (bills are daily buckets); daily bills only need a date column.
Columns not referenced by `column_mapping` are ignored.

## Data layout

The ledger is persistent: `$APILEDGER_ROOT/data/api_ledger.db` keeps all imported history, which is what makes replacement of overlapping bills and accumulation of complementary bills work across runs.
`$APILEDGER_ROOT` defaults to `$XDG_DATA_HOME/apiledger` (`~/.local/share/apiledger`); in the home-manager module it is the `dataDir` option.
`-o` only chooses where the report is written; it does not affect the ledger.
`APILEDGER_SHARE_DIR` overrides where the bundled presets and report template are read from (a development escape hatch; the package bakes the correct path).

```text
$APILEDGER_ROOT/
├── data/
│   ├── api_ledger.db           # SQLite database
│   └── report.html             # generated report
├── .cache/
│   └── echarts.min.js          # downloaded once
└── presets/                    # optional user presets (JSON)
```

## Database schema

### Table `api_records` (aggregate, one row per key)

| Column | Type | Description |
| :---: | :---: | ----- |
| `id` | INTEGER PK | Auto increment |
| `date` | TEXT | Date (`YYYY-MM-DD`) |
| `platform` | TEXT | Platform |
| `project` | TEXT | Project |
| `model` | TEXT | Model |
| `type` | TEXT | Billing type |
| `tokens` | INTEGER | Tokens |
| `cost` | REAL | Amount |
| `unit_price` | REAL | Unit price (CNY per 1M tokens) |
| `source_file` | TEXT | Source bills, comma separated |

Unique constraint: `(date, platform, project, model, type)`.

### Table `record_sources` (contributions, one row per key × source file)

Records each bill's contribution to a key. Aggregate rows are the sum of their contributions.
When a new bill overlaps an old one, the old contributions are deleted; complementary bills accumulate.
A key whose contributions are all deleted is removed as well (no empty `tokens=0` rows).

Indexes exist on `date`, `platform`, `project`, `model`, and `type`.

## Building from source

```bash
nix build
./result/bin/apiledger usage_data.zip
```

For development, `nix develop` provides CMake, Ninja, curl, and Node.js:

```bash
nix develop -c cmake -S . -B build -G Ninja
nix develop -c cmake --build build
```

The report chart checker runs against a generated report:

```bash
node tools/report_check/check.mjs ~/.local/share/apiledger/data/report.html
```
