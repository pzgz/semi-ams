#!/usr/bin/env bash
# 在电脑上编译并运行状态机 / 电机 / 按键逻辑的仿真测试（不需要开发板）
# 用法：bash host_test/run.sh        详细日志：bash host_test/run.sh -v
set -e
cd "$(dirname "$0")"
OUT="$(mktemp -d)/fsm_test"
c++ -std=c++17 -Wall -Wextra -I../src -o "$OUT" test_main.cpp ../src/fsm.cpp ../src/bambu.cpp
"$OUT" "$@"
