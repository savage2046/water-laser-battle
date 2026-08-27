# -*- coding: utf-8 -*-
"""Compare history_data pattern across .eprj2 projects."""
import sqlite3, sys, os

for f in [
    r"D:\ESP32\water-laser-battle\PCB\gun-board.eprj2",
    r"D:\ESP32\water-laser-battle\PCB\triger-sensor-current-test.eprj2",
    r"D:\ESP32\water-laser-battle\PCB\gun-wifi-test.eprj2",
]:
    name = os.path.basename(f)
    try:
        con = sqlite3.connect(f)
        cur = con.cursor()
        cur.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='history_data'")
        if cur.fetchone():
            cur.execute("SELECT count(*), max(length(dataStr)) FROM history_data")
            cnt, mx = cur.fetchone()
            cur.execute("SELECT count(*) FROM components")
            ncomp = cur.fetchone()[0]
            cur.execute("SELECT count(*) FROM devices")
            ndev = cur.fetchone()[0]
            cur.execute("SELECT count(*) FROM documents")
            ndoc = cur.fetchone()[0]
            print(f"{name}: history rows={cnt} maxlen={mx} | components={ncomp} devices={ndev} documents={ndoc}")
        else:
            print(f"{name}: no history_data table")
        con.close()
    except Exception as e:
        print(f"{name}: ERR {e}")
