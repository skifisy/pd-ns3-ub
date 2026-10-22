# -*- coding: utf-8 -*-
import sys
import os

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 parse_trace.py <case_dir> [true|false]")
        sys.exit(1)

    # 获取当前脚本所在目录的绝对路径
    script_dir = os.path.dirname(os.path.abspath(__file__))
    task_stats_script = os.path.join(script_dir, "task_statistics.py")
    cal_throughput_script = os.path.join(script_dir, "cal_throughput.py")

    # 可选参数：是否输出到 test 子目录（默认 False -> 输出到 output）
    flag_true = False
    if len(sys.argv) > 2:
        try:
            flag_true = sys.argv[2].lower() == 'true'
        except Exception:
            flag_true = False

    flag_str = "True" if flag_true else "False"

    # 使用当前解释器调用子脚本，避免环境不一致
    py = sys.executable or "python3"
    cmd1 = f"\"{py}\" \"{task_stats_script}\" {sys.argv[1]} {flag_str}"
    cmd2 = f"\"{py}\" \"{cal_throughput_script}\" {sys.argv[1]} {flag_str}"

    os.system(cmd1)
    os.system(cmd2)
