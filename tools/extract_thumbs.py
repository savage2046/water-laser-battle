# -*- coding: utf-8 -*-
"""Extract project_images (WebP thumbnails) from gun-board.eprj2."""
import sqlite3, base64, os

DB = r"PCB\gun-board.eprj2"
OUT = r"PCB\_thumbs"
os.makedirs(OUT, exist_ok=True)

con = sqlite3.connect(DB)
con.text_factory = lambda b: b.decode("utf-8", "replace")
cur = con.cursor()
cur.execute("SELECT id, uuid, ticket, url, length(image_data) FROM project_images")
for r in cur.fetchall():
    img_id, uuid, ticket, url, ln = r
    cur2 = con.cursor()
    cur2.execute("SELECT image_data FROM project_images WHERE id=?", (img_id,))
    b64 = cur2.fetchone()[0]
    data = base64.b64decode(b64)
    ext = "webp"
    # check magic
    if data[:4] == b"\x89PNG":
        ext = "png"
    elif data[:2] == b"\xff\xd8":
        ext = "jpg"
    p = os.path.join(OUT, f"thumb_{img_id}_{uuid[:8]}_t{ticket}.{ext}")
    with open(p, "wb") as f:
        f.write(data)
    print(p, len(data), "bytes ->", url[:100])
con.close()
