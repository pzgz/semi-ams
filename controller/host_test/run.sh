#!/usr/bin/env bash
# 电脑上跑仿真测试（不需要开发板）
set -e
cd "$(dirname "$0")"
CXX="${CXX:-c++}"
$CXX -std=c++17 -Wall -Wextra -O1 -o /tmp/semiams_ctrl_test test_jsonmac.cpp
/tmp/semiams_ctrl_test
