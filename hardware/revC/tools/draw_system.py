"""Render the Chinese engineering block diagram as editable SVG and PNG."""
from pathlib import Path
from html import escape
import math
from PIL import Image,ImageDraw,ImageFont

R=Path(__file__).resolve().parents[1]
W,H=1800,1310
im=Image.new('RGB',(W,H),'white');d=ImageDraw.Draw(im)
font_path='C:/Windows/Fonts/msyh.ttc'
svg=[f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">','<rect width="100%" height="100%" fill="white"/>']
def font(size):return ImageFont.truetype(font_path,size)
def text(x,y,s,size=23,color='#172335'):
    d.text((x,y),s,font=font(size),fill=color)
    svg.append(f'<text x="{x}" y="{y+size}" font-family="Microsoft YaHei,Noto Sans CJK SC,sans-serif" font-size="{size}" fill="{color}">{escape(s)}</text>')
def wrap(s,width,size):
    out=[];line=''
    for c in s:
        if font(size).getlength(line+c)>width and line:out.append(line);line=''
        line+=c
    if line:out.append(line)
    return out
def box(x,y,w,h,title,lines,color='#2467a5',size=22):
    d.rounded_rectangle((x,y,x+w,y+h),radius=12,fill='#f7f9fc',outline=color,width=3)
    d.rounded_rectangle((x,y,x+w,y+46),radius=12,fill=color)
    svg.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="12" fill="#f7f9fc" stroke="{color}" stroke-width="3"/>')
    svg.append(f'<rect x="{x}" y="{y}" width="{w}" height="46" rx="12" fill="{color}"/>')
    text(x+15,y+6,title,24,'white');yy=y+58
    for line in lines:
        for s in wrap(line,w-30,size):text(x+15,yy,s,size);yy+=size+8
    assert yy<=y+h+5,(title,yy,y+h)
def arrow(points,color='#2467a5',both=False,width=4):
    d.line(points,fill=color,width=width,joint='curve')
    svg.append('<polyline points="'+' '.join(f'{x},{y}' for x,y in points)+f'" fill="none" stroke="{color}" stroke-width="{width}"/>')
    def head(a,b):
        angle=math.atan2(b[1]-a[1],b[0]-a[0]);v=[b,(b[0]-13*math.cos(angle-.45),b[1]-13*math.sin(angle-.45)),(b[0]-13*math.cos(angle+.45),b[1]-13*math.sin(angle+.45))]
        d.polygon(v,fill=color);svg.append('<polygon points="'+' '.join(f'{x:.2f},{y:.2f}' for x,y in v)+f'" fill="{color}"/>')
    head(points[-2],points[-1])
    if both:head(points[1],points[0])

text(45,20,'EasyCanad Rev C｜中文系统电路示意图',36)
text(45,70,'载板 45.72 × 40.64mm · SuperNano 三排直接插接 · 供电/接口框图，详细针脚以原理图和接线表为准',24,'#536175')
orange='#a76214';blue='#2467a5';green='#287253';purple='#6a4a9b'
box(45,135,345,150,'J3：俯仰接口兼供电',['1 地 / 2 稳压5V / 3 PWM','电源来自接收机5V总线','载板取消独立BEC插针'],orange)
box(450,135,240,150,'Q1：反接保护',['AO4407A','R1：100kΩ栅极下拉'],orange)
box(750,135,250,150,'保护后 5V 总线',['C1：470µF/16V','直接供给 J2 舵机'],orange)
box(1060,135,260,150,'JP1：电源跳线',['运行：短接','接带电USB前：拔掉'],orange)
box(1380,135,370,150,'逻辑 5V 电源',['供给 Nano、风标和TF模块','C2/C3/C4/C6 电源去耦','载板没有3.3V稳压或转换'],orange)
for a,b in [(390,450),(690,750),(1000,1060),(1320,1380)]:arrow([(a,205),(b,205)],orange)
arrow([(1550,285),(1550,330),(1035,330),(1035,400)],orange)
text(1070,335,'逻辑5V',21,orange)
arrow([(900,285),(900,310),(1775,310),(1775,475),(1740,475)],orange)
text(1540,284,'舵机总线',20,orange)

box(45,410,385,105,'J3：俯仰 PWM 输入',['与上方供电入口为同一个接口'],blue)
box(45,550,385,105,'J4：模式 PWM 输入',['1 地 / 2 NC / 3 PWM'],blue)
box(45,690,385,105,'J5：油门 PWM 采样',['1 地 / 2 NC / 3 PWM'],blue)
box(740,400,350,425,'SuperMini Nano328P',['ATmega328P，5V逻辑','JN1/JN2/JN3 直接插座','D2 / D3 / D7：PWM输入','A0：风标模拟输入','D9：鸭翼舵机输出','D4：标定 / D5：地面配置','D10～D13：SPI接口','D6：可选插卡检测','VIN 悬空'],blue,22)
for y,label,pin in [(465,'R2 1k + R5 100k下拉','D2'),(605,'R3 1k + R6 100k下拉','D3'),(745,'R4 1k + R7 100k下拉','D7')]:
    arrow([(430,y),(740,y)],blue);text(455,y-30,label,19,blue);text(697,y+5,pin,19,blue)
box(1320,410,420,165,'J2：鸭翼舵机输出',['1 地 / 2 总线5V / 3 PWM','D9 → R8 1k；R9 100k下拉','一个普通5V、9g舵机'],blue)
arrow([(1090,475),(1320,475)],blue);text(1160,440,'D9',23,blue)
box(1320,610,420,215,'操作区：按钮与配置',['SW1：板载标定按钮 → D4','J7：外置常开标定按钮 → D4','JP2：上电前接地 → D5','R11/R12：10k上拉','JP1为电源跳线，见上方供电图'],green,21)
arrow([(1320,665),(1090,665)],green);text(1130,628,'D4 / D5',22,green)

box(45,950,485,170,'J6：模拟霍尔风标',['1 地 / 2 逻辑5V / 3 模拟信号','0～5V 对应 0～360°','R10 1k + C5 100nF：输入滤波'],blue)
arrow([(530,1020),(610,1020),(610,790),(740,790)],blue)
text(625,905,'送 A0',22,blue)
box(1240,940,500,255,'J8：外接5V兼容TF模块',['D10 CS / D11 MOSI / D13 SCK','经 R14/R15/R16 各33Ω串阻','D12 MISO：模块返回5V逻辑','R13 10k上拉，D6插卡高（可选）','R17：10k片选上拉；R18：100k下拉','模块自带3.3V稳压和SPI电平转换'],purple,21)
arrow([(1000,825),(1000,885),(1175,885),(1175,1060),(1240,1060)],purple,True)
text(1020,846,'SPI 双向 + 可选CD',21,purple)
text(660,1070,'TF模块例：Pololu 2587',25,purple)
text(660,1110,'载板1脚接VDD，模块3V3输出不接',21,purple)
text(45,1210,'全部 GND 共地；J4/J5 中心针在载板悬空；输入按5V PWM设计；裸TF卡不能直接接J8。',23)
text(45,1252,'本轮仅完成硬件：TF日志固件尚未实现；控制回路不能因写卡阻塞。所有实物行为仍须台架验证。',23,'#536175')
svg.append('</svg>')
out=R/'exports';im.save(out/'系统电路示意图.png');(out/'系统电路示意图.svg').write_text('\n'.join(svg),encoding='utf8',newline='\n')
print('Chinese system diagram rendered')
