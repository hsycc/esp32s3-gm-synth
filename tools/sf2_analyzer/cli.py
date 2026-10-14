from __future__ import annotations
import argparse,json
from pathlib import Path
from .core import Sf2
from .report import make_report

def _mib(x): return None if x is None else int(x*1024*1024)

def main(argv=None):
    p=argparse.ArgumentParser(description="Analyze SF2 compatibility and PCM residency requirements")
    p.add_argument("sf2",type=Path); p.add_argument("--json",dest="json_path",type=Path); p.add_argument("--psram-mib",type=float)
    p.add_argument("--reserve-mib",type=float,default=0.0); p.add_argument("--pretty",action="store_true")
    a=p.parse_args(argv); sf2=Sf2(a.sf2).parse(); report=make_report(sf2,_mib(a.psram_mib),_mib(a.reserve_mib) if a.psram_mib is not None else None)
    text=json.dumps(report,indent=2 if a.pretty else None,sort_keys=True)
    if a.json_path: a.json_path.parent.mkdir(parents=True,exist_ok=True); a.json_path.write_text(text+"\n",encoding="utf-8")
    print(text); return 0
