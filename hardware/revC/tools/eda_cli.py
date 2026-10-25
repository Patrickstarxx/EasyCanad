"""Run the installed EasyEDA Pro CLI and retain its response as design evidence."""
import argparse
import base64
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
EXE = r"C:\Program Files\lceda-pro\lceda-pro.exe"


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--save")
    p.add_argument("--script")
    p.add_argument("--session")
    p.add_argument("--brief", action="store_true")
    p.add_argument("--extract-files", action="store_true")
    p.add_argument("args", nargs="*")
    a = p.parse_args()
    args = a.args
    if a.script:
        args = ["invoke", "--session", a.session, "--ext-uuid", "eda",
                "--timeout", "180000", "--code", pathlib.Path(a.script).read_text(encoding="utf-8")]
    result = subprocess.run([EXE, *args], capture_output=True, timeout=240)
    try:
        raw = result.stdout.decode("utf-8").strip()
    except UnicodeDecodeError:
        raw = result.stdout.decode("gb18030").strip()
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        print(raw, result.stderr)
        return 1
    if a.extract_files and data.get('ok'):
        for f in data.get('value',{}).get('files',[]):
            name = pathlib.Path(f['name']).name
            out = ROOT / 'exports' / name
            out.parent.mkdir(parents=True,exist_ok=True)
            out.write_bytes(base64.b64decode(f['b64']))
            f.pop('b64')
            f['saved']=str(out)
    if a.save:
        out = ROOT / "evidence" / a.save
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    if a.brief and data.get('ok'):
        print(json.dumps({'ok':True,'durationMs':data.get('durationMs'),'saved':a.save},indent=2))
    else:
        print(json.dumps(data, ensure_ascii=False, indent=2))
    return 0 if data.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
