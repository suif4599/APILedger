"""
APILedger - 项目路径解析

所有模块的目录常量统一从这里取, 避免每个模块各自 dirname(__file__) 猜项目根。

项目根 = 本文件所在目录 (src/) 的上一级; 可用环境变量 APILEDGER_ROOT 覆盖,
便于从别处调用或让测试指向临时目录。

目录约定:
  data/    用户数据与产物 (input 账单 / archive 归档 / 数据库 / report.html)
  presets/ 平台导入预设 (每平台一个 .py, 属于代码, 纳入版本控制)
  .cache/  运行时第三方下载缓存 (echarts.min.js), 不入库
"""

import os

SRC_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.abspath(
    os.environ.get("APILEDGER_ROOT") or os.path.dirname(SRC_DIR)
)

DATA_DIR = os.path.join(PROJECT_ROOT, "data")
INPUT_DIR = os.path.join(DATA_DIR, "input")
ARCHIVE_DIR = os.path.join(DATA_DIR, "archive")
DB_PATH = os.path.join(DATA_DIR, "api_ledger.db")

PRESETS_DIR = os.path.join(PROJECT_ROOT, "presets")

CACHE_DIR = os.path.join(PROJECT_ROOT, ".cache")
ECHARTS_PATH = os.path.join(CACHE_DIR, "echarts.min.js")

TEMPLATE_PATH = os.path.join(SRC_DIR, "report_template.html")
