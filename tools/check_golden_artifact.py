#!/usr/bin/env python3
"""Repository entry point for the shipped golden evidence validator."""
from pathlib import Path
import runpy
runpy.run_path(str(Path(__file__).resolve().parents[1]/'samples/golden_system/artifacts.py'),run_name='__main__')
