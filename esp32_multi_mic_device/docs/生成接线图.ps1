$svgParts = [System.Collections.Generic.List[string]]::new()
$svgParts.Add('<svg xmlns="http://www.w3.org/2000/svg" width="1500" height="1120" viewBox="0 0 1500 1120"><rect width="1500" height="1120" fill="#f5f7fb"/><style>text{font-family:Microsoft YaHei,Arial,sans-serif;fill:#17243c} .title{font-size:28px;font-weight:bold}.label{font-size:17px}.small{font-size:15px}.box{fill:white;stroke:#a7b4c9;stroke-width:2}.wire{fill:none;stroke-width:3}</style>')
function Label($x,$y,$value,$css='label') { $escaped = [System.Security.SecurityElement]::Escape($value); $svgParts.Add("<text x='$x' y='$y' class='$css'>$escaped</text>") }
function Box($x,$y,$w,$h) { $svgParts.Add("<rect x='$x' y='$y' width='$w' height='$h' rx='12' class='box'/>") }
Label 35 45 '多麦克风兼容工程 · 逐线接线图' 'title'
Label 35 76 'ESP32-S3 | 保持现有 GPIO | 接头按信号名称接线，非开发板物理排针图' 'small'
Box 35 105 270 580
Label 55 139 'ESP32-S3 / 主板' 'title'
$names=@('3V3_MIC','GND','GPIO42 · BCLK_A','GPIO41 · WS_A','GPIO21 · SD_A','GPIO18 · BCLK_B','GPIO38 · WS_B','GPIO11 · SD_B')
for($i=0;$i -lt $names.Count;$i++){ Label 55 (180+$i*55) $names[$i] }
$colors=@('#d34242','#525b6b','#bd8110','#2374ba','#25864b','#bd8110','#2374ba','#25864b')
$micPositions=@(105,310,515)
for($m=0;$m -lt 3;$m++){
 $top=$micPositions[$m]; Box 1050 $top 410 180
 Label 1070 ($top+29) ('J_MIC'+($m+1)+' · INMP441') 'title'
 $labels=@('1 VDD → 3V3_MIC','2 GND → GND','3 SCK → '+$(if($m -eq 2){'GPIO18'}else{'GPIO42'}),'4 WS → '+$(if($m -eq 2){'GPIO38'}else{'GPIO41'}),'5 SD → '+$(if($m -eq 2){'GPIO11'}else{'GPIO21'}),'6 L/R → '+$(if($m -eq 1){'3V3（右声道）'}else{'GND（左声道）'}))
 for($p=0;$p -lt 6;$p++){
   $endY=$top+52+$p*22; Label 1070 $endY $labels[$p] 'small'
   $index= switch($p){0{0}1{1}2{if($m -eq 2){5}else{2}}3{if($m -eq 2){6}else{3}}4{if($m -eq 2){7}else{4}}5{if($m -eq 1){0}else{1}}}
   $startY=175+$index*55; $bend=370+$index*65+$m*12
   $svgParts.Add("<path d='M305 $startY H$bend V$($endY-5) H1050' class='wire' stroke='$($colors[$index])'/>")
 }
}
Label 55 638 'GPIO11/18/38 为新增预留' 'small'
Label 55 665 'MIC1 左 / MIC2 右共享 SD_A' 'small'
Box 35 720 450 210
Label 55 755 'MAX98357A / 喇叭' 'title'
Label 55 790 'BCLK → GPIO42；LRC → GPIO41'
Label 55 820 'DIN → GPIO40；SD/EN → GPIO39'
Label 55 850 'VIN → 5V_AUDIO；GND → 系统地'
Label 55 880 'SPK+ ─ 喇叭 ─ SPK-；两端不接地'
Box 510 720 470 210
Label 530 755 '屏幕 / 按钮 / 四路 SG90' 'title'
Label 530 790 '屏 SCK4 / MOSI5 / RST6 / DC7 / CS15'
Label 530 820 '屏 VCC→3V3；GND→地；ST7735 BL→16'
Label 530 850 '按钮 GPIO2 / GPIO47 → 常开按钮 → 地'
Label 530 880 '舵机信号 12/13/14/17；独立供电并共地'
Box 1005 720 455 210
Label 1025 755 '步进预留 / 电源分支' 'title'
Label 1025 790 'STEP8 / DIR9 / nSLEEP10 → DRV8834'
Label 1025 820 'VM → 5V_MOTOR；A1/A2、B1/B2 → 两相'
Label 1025 850 '5V_LOGIC → ESP32 VIN'
Label 1025 880 '逻辑 / 功放 / 电机在电源入口分支'
Label 35 977 '接点规则：同名网相连；不同颜色线交叉不代表连接。两组 BCLK、WS、SD 禁止互接。' 'label'
Label 35 1010 '每只麦克风 VDD/GND 就近 100nF；SD_A、SD_B 预留 100kΩ 下拉；MIC2 / MIC3 可暂不装。' 'label'
Label 35 1043 '默认仅 MIC1；/mic count 1|2|3 设置数量。支持选路录音；独立时钟不保证阵列同步，尚未实物验证。' 'label'
Label 35 1080 '详细针表、线束走向、供电及验证步骤：同目录《接线方案.md》 · 2026-10-02' 'small'
$svgParts.Add('</svg>')
$svgParts -join "`n" | Set-Content -Encoding utf8 (Join-Path $PSScriptRoot '详细接线图.svg')

