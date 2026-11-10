#!/usr/bin/env python3
"""subject2_sim.py — 科目二场地泊车仿真（连接 open_space_parking_sim）"""
import sys
from pathlib import Path

THIS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(THIS_DIR))

import open_space_parking_sim

if __name__ == "__main__":
    open_space_parking_sim.main()
