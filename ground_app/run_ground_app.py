"""Source-checkout launcher; all paths below are relative to this subproject."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / "src"))
from dji_h1_ground.__main__ import main

if __name__ == "__main__":
    main()
