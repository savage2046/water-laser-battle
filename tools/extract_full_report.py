"""Full report: BOM + net labels + device meta from JLCEDA Pro .epru."""
import json
import sys
import zipfile
import collections


def split_payload(payload):
    """Split 'json1|json2|...' at top level using a JSON decoder."""
    out = []
    rest = payload.strip()
    dec = json.JSONDecoder()
    while rest:
        rest = rest.lstrip()
        if not rest:
            break
        try:
            val, end = dec.raw_decode(rest)
            out.append(val)
            rest = rest[end:].lstrip()
            if rest.startswith("|"):
                rest = rest[1:]
            else:
                break
        except Exception:
            # not JSON (e.g. trailing junk); keep raw remainder
            out.append(rest)
            break
    return out


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
        out.append((c, split_payload(payload)))
    return out


def docs_by_type(entries):
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


def meta_of(obj_list):
    for c, pls in obj_list:
        if c.get("type") == "META":
            return pls[0] if pls else None
    return None


def deep_meta(obj_list):
    """META payload may be a dict or a JSON string; return parsed dict."""
    m = meta_of(obj_list)
    if isinstance(m, str):
        try:
            return json.loads(m)
        except Exception:
            return {"raw": m[:500]}
    return m


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else r"PCB\triger-sensor-current-test_backup\triger-sensor-current-test_2026-08-19-18-40.epro2"
    entries = load_epru(path)
    docs = docs_by_type(entries)

    # Device meta (deep)
    devices = {}
    for uuid, objs in docs.get("DEVICE", {}).items():
        devices[uuid] = deep_meta(objs)
    # Footprint meta
    footprints = {}
    for uuid, objs in docs.get("FOOTPRINT", {}).items():
        footprints[uuid] = deep_meta(objs)
    # Symbol meta
    symbols = {}
    for uuid, objs in docs.get("SYMBOL", {}).items():
        symbols[uuid] = deep_meta(objs)

    # ---- Schematic components ----
    sch_uuid = "ca1c10d680cb80bb"
    sch_objs = docs["SCH_PAGE"][sch_uuid]
    comps = [x for x in sch_objs if x[0].get("type") == "COMPONENT"]
    attrs_by_parent = collections.defaultdict(dict)
    for c, pls in sch_objs:
        if c.get("type") != "ATTR":
            continue
        for p in pls:
            if not isinstance(p, dict) or "key" not in p:
                continue
            attrs_by_parent[p.get("parentId")][p["key"]] = p.get("value")

    print("=" * 100)
    print("SCHEMATIC COMPONENTS  (SCH_PAGE P1)")
    print("=" * 100)
    rows = []
    for c, pls in comps:
        cid = c.get("id")
        part_id = None
        pos = {}
        for p in pls:
            if isinstance(p, dict):
                part_id = p.get("partId", part_id)
                pos = {"x": p.get("x"), "y": p.get("y"), "rot": p.get("rotation")}
        at = attrs_by_parent.get(cid, {})
        dev_uuid = at.get("Device")
        sym_uuid = at.get("Symbol")
        dev = devices.get(dev_uuid, {}) or {}
        rows.append({
            "id": cid,
            "partId": part_id,
            "designator": at.get("Designator"),
            "name": at.get("Name"),
            "device": dev.get("title") if isinstance(dev, dict) else dev_uuid,
            "lcsc": at.get("LCSC Part Number"),
            "lcsc_name": at.get("LCSC Part Name"),
            "footprint": at.get("Footprint"),
            "pos": pos,
            "dev_uuid": dev_uuid,
        })

    def sort_key(r):
        d = r["designator"] or r["name"] or r["partId"] or ""
        m = re.match(r"([A-Za-z]+)(\d+)", d)
        return (m.group(1), int(m.group(2))) if m else (d, 0)

    import re
    rows.sort(key=sort_key)
    for r in rows:
        print(f"{str(r['designator']):>10} | {str(r['name'] or ''):>10} | {str(r['device'])[:44]:<44} | LCSC {str(r['lcsc']):<12} | {str(r['lcsc_name'])[:36]:<36} | fp={str(r['footprint'])[:30]} | dev={r['dev_uuid']}")

    # ---- Net labels found ----
    print("\n" + "=" * 100)
    print("NET LABELS / PORT SYMBOLS")
    print("=" * 100)
    nets = collections.Counter()
    for c, pls in sch_objs:
        if c.get("type") != "ATTR":
            continue
        for p in pls:
            if not isinstance(p, dict):
                continue
            if p.get("key") in ("Name", "Global Net Name") and p.get("value"):
                nets[p["value"]] += 1
            if p.get("key") == "NET" and p.get("value"):
                nets["NET:" + p["value"]] += 1
    for k, v in nets.most_common():
        print(f"  {k:<24} x{v}")

    # ---- Device full meta sample ----
    print("\n" + "=" * 100)
    print("DEVICE FULL META (first 3 + every distinct title)")
    print("=" * 100)
    seen = set()
    for uuid, m in devices.items():
        title = (m or {}).get("title") if isinstance(m, dict) else None
        if not isinstance(m, dict) or title in seen:
            continue
        seen.add(title)
        print(f"\n### {title}  [{uuid}]")
        print(json.dumps(m, ensure_ascii=False)[:1200])
