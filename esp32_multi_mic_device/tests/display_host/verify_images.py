from pathlib import Path
import json
import sys

out = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(out/'pythonlibs'))
import zxingcpp
from PIL import Image, ImageDraw, ImageFont

checked = 0
for path in sorted(out.glob('*-qr.ppm')):
    image = Image.open(path)
    results = zxingcpp.read_barcodes(image)
    expected = 'WIFI:T:WPA;S:ROBOT-1A2B;P:12345678;;' if 'wifi-qr' in path.name else 'http://192.168.4.1/setup'
    assert len(results) == 1 and results[0].text == expected, (path.name, results)
    checked += 1
assert checked == 16, checked
print(f'PASS: independently decoded {checked} QR screenshots at native resolution.')

for path in out.glob('*.ppm'):
    Image.open(path).save(path.with_suffix('.png'))
font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 18)
small_font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 15)
columns = [('large-r1','ST7789 横屏',1.4),('large-r0','ST7789 竖屏',1.15),('small-r1','ST7735 横屏',2),('small-r0','ST7735 竖屏',2)]
rows = [('home','天气主页'),('expression','待机表情'),('compact','底部状态条'),('full','全屏回答'),('wifi-qr','连接热点'),('url-qr','初始化入口')]
cell_w, cell_h = 475, 414
sheet = Image.new('RGB',(cell_w*4, cell_h*len(rows)+68),'#e9efeb')
draw = ImageDraw.Draw(sheet)
draw.text((20,16),'设备实际绘制代码的像素预览 · 主机模拟，不代表实机拍摄',font=font,fill='#203b30')
for col,(prefix,title,scale) in enumerate(columns):
    for row,(suffix,label) in enumerate(rows):
        im = Image.open(out/f'{prefix}-{suffix}.png')
        im = im.resize((round(im.width*scale),round(im.height*scale)),Image.Resampling.NEAREST)
        x = col*cell_w+(cell_w-im.width)//2
        y = row*cell_h+102
        sheet.paste(im,(x,y))
        draw.text((col*cell_w+18,row*cell_h+66),title+' · '+label,font=small_font,fill='#203b30')
sheet.save(out/'display-review.png')
# A compact image for the user: the default ST7789 panel, both orientations.
preview = sheet.crop((0,0,cell_w*2,cell_h*3+68))
preview.save(out/'display-preview.png')

catalog = json.loads((out/'expressions.json').read_text(encoding='utf-8'))
assert len(catalog['categories']) == 8 and len(catalog['items']) == 48
assert all(sum(item['category'] == category for item in catalog['items']) == 6 for category in range(8))
for style in ['eyes', 'body']:
    fingerprints = set()
    for i in range(48):
        thumbnail = Image.open(out/f'thumb-{style}-{i}.bmp')
        assert thumbnail.size == (96,96) and thumbnail.mode == 'RGB'
        fingerprints.add(thumbnail.tobytes())
    assert len(fingerprints) == 48, (style, 'duplicate thumbnails', len(fingerprints))
print('PASS: 8 balanced categories; 48 distinct eye thumbnails and 48 distinct body thumbnails.')

# Every row is one category; pairs are drawn by the same renderer as the device.
gallery = Image.new('RGB', (1320, 8*205+70), '#e9efeb')
draw = ImageDraw.Draw(gallery)
draw.text((18,18),'48 组内置动态表情 · 发光表情 / 可选全身角色 · 静态一帧',font=font,fill='#203b30')
for category,name in enumerate(catalog['categories']):
    top = 70+category*205
    draw.text((18,top),name,font=font,fill='#203b30')
    for i in range(6):
        item = catalog['items'][category*6+i]
        x = i*220+12
        for side,style in enumerate(['eyes','body']):
            gallery.paste(Image.open(out/f'thumb-{style}-{item["id"]}.bmp'),(x+side*102,top+32))
        draw.text((x,top+140),f'{item["id"]+1:02} {item["name"]}',font=small_font,fill='#203b30')
        draw.text((x,top+165),item['gesture'],font=small_font,fill='#536c63')
gallery.save(out/'expression-catalog.png')
