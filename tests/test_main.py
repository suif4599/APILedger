"""main.py - 命令行导入 + 报告生成 (数据库与目录已隔离)"""

import builtins
import os

import pytest

from conftest import make_paratera_row
import report as report
from db import Database


@pytest.fixture
def main_env(tmp_path, import_env, monkeypatch, paratera_xlsx_factory):
    """将 main 的 Database、报告输出目录都指向临时环境"""
    import main

    db_path = tmp_path / "main.db"
    monkeypatch.setattr(main, "Database", lambda: Database(str(db_path)))
    out_dir = tmp_path / "data"
    monkeypatch.setattr(report, "DATA_DIR", str(out_dir))
    return import_env, paratera_xlsx_factory, out_dir


def run_main(monkeypatch, *args, inputs=None):
    import sys

    import main

    if inputs is not None:
        it = iter(inputs)
        monkeypatch.setattr(builtins, "input", lambda *a: next(it))
    opened = []
    monkeypatch.setattr(main, "open_in_browser", lambda path: opened.append(path))
    monkeypatch.setattr(sys, "argv", ["main.py", *args])
    main.main()
    return opened


class TestMainImport:
    def test_import_and_summary(self, main_env, capsys, monkeypatch):
        (input_dir, _), factory, _ = main_env
        factory(os.path.join(input_dir, "paratera_2026-08-01_2026-08-31.csv"),
                [make_paratera_row(desc="输入:1,000tokens", cost=0.5)])

        run_main(monkeypatch, "--no-open")
        out = capsys.readouterr().out
        assert "发现 1 个待导入文件" in out
        assert "已写入" in out and "导入完成" in out
        assert "报告已生成" in out

        # 第二次运行: 重新放置同一文件 (首次运行已归档) → 幂等, 数值不变
        factory(os.path.join(input_dir, "paratera_2026-08-01_2026-08-31.csv"),
                [make_paratera_row(desc="输入:1,000tokens", cost=0.5)])
        run_main(monkeypatch, "--no-open")
        out = capsys.readouterr().out
        assert "替换旧账单 1 个" in out

    def test_empty_input_dir(self, main_env, capsys, monkeypatch):
        run_main(monkeypatch, "--no-open")
        assert "没有待导入文件" in capsys.readouterr().out

    def test_dry_run_does_not_archive(self, main_env, capsys, monkeypatch):
        (input_dir, archive_dir), factory, _ = main_env
        path = factory(os.path.join(input_dir, "paratera_2026-08-01_2026-08-31.csv"),
                       [make_paratera_row()])
        run_main(monkeypatch, "--dry-run")
        out = capsys.readouterr().out
        assert "dry-run" in out
        assert os.path.exists(path)  # 未归档
        assert "报告已生成" not in out  # dry-run 不生成报告

    def test_report_written_and_opened(self, main_env, capsys, monkeypatch):
        (input_dir, _), factory, out_dir = main_env
        factory(os.path.join(input_dir, "paratera_2026-08-01_2026-08-31.csv"),
                [make_paratera_row()])
        opened = run_main(monkeypatch)
        assert os.path.exists(os.path.join(str(out_dir), "report.html"))
        assert len(opened) == 1 and opened[0].endswith("report.html")

    def test_no_open_flag(self, main_env, capsys, monkeypatch):
        (input_dir, _), factory, _ = main_env
        factory(os.path.join(input_dir, "paratera_2026-08-01_2026-08-31.csv"),
                [make_paratera_row()])
        opened = run_main(monkeypatch, "--no-open")
        assert opened == []
        assert "报告已生成" in capsys.readouterr().out

    def test_no_report_flag(self, main_env, capsys, monkeypatch):
        (input_dir, _), factory, out_dir = main_env
        factory(os.path.join(input_dir, "paratera_2026-08-01_2026-08-31.csv"),
                [make_paratera_row()])
        run_main(monkeypatch, "--no-open", "--no-report")
        assert not os.path.exists(os.path.join(str(out_dir), "report.html"))


class TestReplaceImportFlow:
    """默认流程: 新账单替换同平台日期重叠的旧账单"""

    def test_summary_and_replacement(self, main_env, capsys, monkeypatch):
        (input_dir, _), factory, _ = main_env
        factory(os.path.join(input_dir, "paratera_2026-08-01_2026-08-31.csv"),
                [make_paratera_row(desc="输入:1,000tokens", cost=0.5)])
        run_main(monkeypatch, "--no-open")
        out = capsys.readouterr().out
        assert "未发现重叠账单, 数据累加" in out
        assert "导入完成: 处理 1 个文件 | 写入 1 条" in out

        # 区段重叠的新账单 → 替换旧账单
        factory(os.path.join(input_dir, "paratera_2026-08-15_2026-08-31.csv"),
                [make_paratera_row(desc="输入:9,999tokens", cost=9.9)])
        run_main(monkeypatch, "--no-open")
        out = capsys.readouterr().out
        assert "替换旧账单 1 个: paratera_2026-08-01_2026-08-31.csv" in out
        assert "替换旧账单 1 个" in out.split("=" * 20)[-1]  # 汇总

    def test_force_yes_ignored_without_legacy(self, main_env, capsys, monkeypatch):
        (input_dir, _), factory, _ = main_env
        factory(os.path.join(input_dir, "paratera_2026-08-01_2026-08-31.csv"),
                [make_paratera_row()])
        run_main(monkeypatch, "--no-open", "--force")
        out = capsys.readouterr().out
        assert "--force / --yes 仅在 --legacy 下生效" in out
        assert "导入完成" in out  # 仍按替换流程导入

    def test_legacy_flow_still_available(self, main_env, capsys, monkeypatch):
        """--legacy: 回退到旧的冲突裁决流程 (无变化 / 冲突提示)"""
        (input_dir, _), factory, _ = main_env
        factory(os.path.join(input_dir, "paratera_2026-08-01_2026-08-31.csv"),
                [make_paratera_row(desc="输入:1,000tokens", cost=0.5)])
        run_main(monkeypatch, "--no-open", "--legacy")
        assert "新增 1 条" in capsys.readouterr().out

        factory(os.path.join(input_dir, "paratera_2026-08-01_2026-08-31.csv"),
                [make_paratera_row(desc="输入:1,000tokens", cost=0.5)])
        run_main(monkeypatch, "--no-open", "--legacy")
        assert "无变化 1 条" in capsys.readouterr().out


class TestConflictResolution:
    """旧流程 (--legacy) 冲突裁决: --force / --yes / 交互逐条 (y/n/a/s/q)"""

    def _seed_conflict(self, main_env, monkeypatch):
        """导入一份, 再放置区段重叠但数值不同的文件, 返回 (input_dir, factory)"""
        (input_dir, _), factory, _ = main_env
        factory(os.path.join(input_dir, "paratera_2026-08-01_2026-08-31.csv"),
                [make_paratera_row(desc="输入:1,000tokens", cost=0.5)])
        run_main(monkeypatch, "--no-open", "--legacy")

        factory(os.path.join(input_dir, "paratera_2026-08-15_2026-08-31.csv"),
                [make_paratera_row(desc="输入:9,999tokens", cost=9.9)])
        return input_dir, factory

    def test_force_overwrites(self, main_env, capsys, monkeypatch):
        self._seed_conflict(main_env, monkeypatch)
        run_main(monkeypatch, "--no-open", "--legacy", "--force")
        out = capsys.readouterr().out
        assert "--force 模式: 覆盖 1 条冲突" in out

    def test_yes_skips_conflicts(self, main_env, capsys, monkeypatch):
        self._seed_conflict(main_env, monkeypatch)
        run_main(monkeypatch, "--no-open", "--legacy", "--yes")
        out = capsys.readouterr().out
        assert "跳过 1 条冲突" in out
        assert "1 条冲突" in out.split("=" * 20)[-1]  # 汇总

    def test_interactive_skip_one(self, main_env, capsys, monkeypatch):
        self._seed_conflict(main_env, monkeypatch)
        run_main(monkeypatch, "--no-open", "--legacy", inputs=["n"])
        out = capsys.readouterr().out
        assert "冲突 1/1" in out
        assert "本次: tokens=9,999" in out

    def test_interactive_overwrite_one(self, main_env, capsys, monkeypatch):
        self._seed_conflict(main_env, monkeypatch)
        run_main(monkeypatch, "--no-open", "--legacy", inputs=["y"])
        out = capsys.readouterr().out
        assert "已写入" in out

    def test_interactive_overwrite_all(self, main_env, capsys, monkeypatch):
        self._seed_conflict(main_env, monkeypatch)
        run_main(monkeypatch, "--no-open", "--legacy", inputs=["a"])
        out = capsys.readouterr().out
        assert "覆盖剩余 1 条冲突" in out

    def test_interactive_skip_all(self, main_env, capsys, monkeypatch):
        self._seed_conflict(main_env, monkeypatch)
        run_main(monkeypatch, "--no-open", "--legacy", inputs=["s"])
        out = capsys.readouterr().out
        assert "跳过剩余 1 条冲突" in out

    def test_interactive_abort(self, main_env, capsys, monkeypatch):
        (input_dir, factory) = self._seed_conflict(main_env, monkeypatch)
        run_main(monkeypatch, "--no-open", "--legacy", inputs=["q"])
        out = capsys.readouterr().out
        assert "已中止导入" in out
        # 文件未归档, 仍在 input/ 中
        assert os.path.exists(os.path.join(input_dir, "paratera_2026-08-15_2026-08-31.csv"))

    def test_invalid_then_valid_input(self, main_env, capsys, monkeypatch):
        self._seed_conflict(main_env, monkeypatch)
        run_main(monkeypatch, "--no-open", "--legacy", inputs=["x", "n"])
        out = capsys.readouterr().out
        assert "无效输入" in out
