#!/usr/bin/env python3
"""
KunQuant Terminal Launcher
Starts the full-stack native C++ daemon + Python AI service + Web Trading Desk
"""

import os
import sys
import subprocess

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
START_SCRIPT = os.path.join(ROOT_DIR, "scripts", "start_quant.sh")

def main():
    if not os.path.exists(START_SCRIPT):
        print(f"[!] 启动脚本未找到: {START_SCRIPT}")
        sys.exit(1)
    
    cmd = [START_SCRIPT] + sys.argv[1:]
    try:
        subprocess.run(cmd)
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()
