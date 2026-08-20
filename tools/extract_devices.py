"""Full extraction: components, devices, symbols, nets from a JLCEDA Pro .epru."""
import json
import sys
import zipfile
import collections
import re


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


def docs_by_type(entries):
    """Return {docType: {uuid: {objects: [(cmd, pls)...]}}}"""
    result = collections.defaultdict(dict)
    cur = None
    for c, pls in entries:
        if c.get("type") == "DOCHEAD":
            doc_type = None
            uuid = None
            for p in pls:
                if isinstance(p, dict):
                    doc_type = p.get("docType", doc_type)
                    uuid = p.get("uuid", uuid)
            cur = (doc_type, uuid)
            if cur not in result[doc_type]:
                result[doc_type][uuid] = []
            continue
        if cur:
            result[cur[0]][cur[1]].append((c, pls))
    return result


def obj_attrs(obj_list):
    """Collect ATTR payloads keyed by parentId."""
    by_parent = collections.defaultdict(dict)
    for c, pls in obj_list:
        if c.get("type") != "ATTR":
            continue
        for p in pls:
            if not isinstance(p, dict) or "key" not in p:
                continue
            parent = p.get("parentId")
            by_parent[parent][p["key"]] = p.get("value")
    return by_parent


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else r"PCB\triger-sensor-current-test_backup\triger-sensor-current-test_2026-08-19-18-40.epro2"
    entries = load_epru(path)
    docs = docs_by_type(entries)

    # ---- DEVICE metadata ----
    print("=" * 70)
    print("DEVICE documents")
    print("=" * 70)
    for uuid, objs in sorted(docs.get("DEVICE", {}).items()):
        meta = None
        for c, pls in objs:
            if c.get("type") == "META":
                meta = pls[0] if pls else None
        print(f"\n{uuid}")
        print(json.dumps(meta, ensure_ascii=False)[:1500] if meta else "(no META)")

    # ---- SYMBOL names ----
    print("\n" + "=" * 70)
    print("SYMBOL documents (name)")
    print("=" * 70)
    sym_names = {}
    for uuid, objs in sorted(docs.get("SYMBOL", {}).items()):
        meta = None
        for c, pls in objs:
            if c.get("type") == "META":
                meta = pls[0] if pls else None
        name = (meta or {}).get("name") if isinstance(meta, dict) else None
        sym_names[uuid] = name
        print(f"{uuid}  ->  {name}")
