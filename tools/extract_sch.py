"""Extract schematic content from a JLCEDA Pro .epru stream."""
import json
import sys
import zipfile
import collections


def load_epru(path):
    z = zipfile.ZipFile(path)
    name = [n for n in z.namelist() if n.endswith(".epru")][0]
    data = z.read(name).decode("utf-8", errors="replace")
    z.close()
    out = []
    for ln in data.split("\n"):
        if not ln.strip():
            continue
        if "||" in ln:
            cmd, payload = ln.split("||", 1)
        else:
            cmd, payload = ln, ""
        try:
            c = json.loads(cmd)
        except Exception:
            continue
        pls = []
        for part in payload.split("|"):
            part = part.strip()
            if not part:
                continue
            try:
                pls.append(json.loads(part))
            except Exception:
                pls.append(part)
        out.append((c, pls))
    return out


def doc_objects(entries, doc_uuid):
    """Yield (cmd, payloads) for objects belonging to doc_uuid."""
    in_doc = False
    for c, pls in entries:
        if c.get("type") == "DOCHEAD":
            in_doc = any(isinstance(p, dict) and p.get("uuid") == doc_uuid for p in pls)
            continue
        if in_doc:
            yield c, pls


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else r"PCB\triger-sensor-current-test_backup\triger-sensor-current-test_2026-08-19-18-40.epro2"
    entries = load_epru(path)

    sch_uuid = "ca1c10d680cb80bb"
    objs = list(doc_objects(entries, sch_uuid))
    print(f"SCH_PAGE objects: {len(objs)}")

    comps = []
    for c, pls in objs:
        if c.get("type") == "COMPONENT":
            comps.append((c, pls))
    print(f"components: {len(comps)}")
    for c, pls in comps[:3]:
        print("\nCMD:", json.dumps(c, ensure_ascii=False))
        for p in pls:
            print("PL:", json.dumps(p, ensure_ascii=False)[:2000])
