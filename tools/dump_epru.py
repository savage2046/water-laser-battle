"""Dump sample objects from a JLCEDA Pro .epru document stream."""
import json
import sys
import zipfile


def load_epru(path):
    """Return list of (cmd_obj, payload_obj) tuples for every line."""
    if path.endswith(".epro2"):
        z = zipfile.ZipFile(path)
        name = [n for n in z.namelist() if n.endswith(".epru")][0]
        data = z.read(name).decode("utf-8", errors="replace")
        z.close()
    else:
        with open(path, "rb") as f:
            data = f.read().decode("utf-8", errors="replace")
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
        p = None
        if payload.strip():
            try:
                p = json.loads(payload)
            except Exception:
                p = payload
        out.append((c, p))
    return out


def dump_doc(entries, doc_uuid, wanted_types, out, max_items=10):
    """Dump objects belonging to a document."""
    in_doc = False
    count = 0
    for c, p in entries:
        if c.get("type") == "DOCHEAD":
            in_doc = (c.get("uuid") == doc_uuid or (isinstance(p, dict) and p.get("uuid") == doc_uuid))
            continue
        if not in_doc:
            continue
        t = c.get("type")
        if t in wanted_types:
            count += 1
            if count <= max_items:
                out.append(f"--- {t} [{count}] id={c.get('id')}")
                out.append(json.dumps(p, ensure_ascii=False)[:2500] if p is not None else "(no payload)")
    return count


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else r"PCB\triger-sensor-current-test_backup\triger-sensor-current-test_2026-08-19-18-40.epro2"
    entries = load_epru(path)
    print(f"loaded {len(entries)} objects")
    doc_uuids = {
        "SCH_PAGE": "ca1c10d680cb80bb",
        "PCB": "ceb442a27ffb448c",
    }
    for docname, uuid in doc_uuids.items():
        print(f"\n########## {docname} ({uuid}) ##########")
        out = []
        for t in (["COMPONENT", "NET", "TRACK", "VIA", "COPPER", "POLY", "WIRE", "TEXT", "RULE", "META"] if docname == "PCB" else
                  ["COMPONENT", "PART", "PIN", "WIRE", "NET", "TEXT", "META"]):
            n = dump_doc(entries, uuid, [t], out)
            if n:
                print(f"  [type {t}: {n} objects]")
        print("\n".join(out[:120]))
