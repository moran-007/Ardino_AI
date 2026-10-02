"""Prepare reference QR modules and a minimal subset of the installed real fonts."""
from pathlib import Path
import json
import re
import sys

out = Path(sys.argv[1]).resolve()
libs = Path(sys.argv[2])
sys.path.insert(0, str(out / 'pythonlibs'))
import qrcode

payloads = ['WIFI:T:WPA;S:ROBOT-1A2B;P:12345678;;', 'http://192.168.4.1/setup']
fixtures = []
for payload in payloads:
    qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_M, border=0)
    qr.add_data(payload, optimize=0)
    qr.make(fit=True)
    modules = qr.get_matrix()
    assert len(modules) <= 29
    fixtures.append('{%s,%d,"%s"}' % (json.dumps(payload), len(modules), ''.join('1' if c else '0' for row in modules for c in row)))
(out/'qr_fixtures.h').write_text('#pragma once\nstruct QrFixture{const char *payload;int size;const char *modules;};\ninline const QrFixture qrFixtures[]={\n'+',\n'.join(fixtures)+'\n};\n')
fonts = (libs/'U8g2_for_Adafruit_GFX/src/u8g2_fonts.c').read_text(encoding='latin-1')
names = ['wqy16_t_gb2312','wqy12_t_gb2312','logisoso32_tn','helvB18_tn','7x14_tr','6x10_tr']
parts = ['#include "u8g2_fonts.h"\n']
for name in names:
    parts.append(re.search(r'const uint8_t u8g2_font_'+name+r'\[.*?;\n',fonts,re.S)[0])
(out/'test_fonts.c').write_text('\n'.join(parts), encoding='latin-1')
print('Prepared reference QR fixtures and six installed U8g2 fonts.')
