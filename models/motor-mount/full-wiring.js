'use strict';

const wire = (path, kind) => `<path class="wire ${kind}" d="${path}"/>`;
const junction = (x, y, kind) => `<circle class="dot ${kind}" cx="${x}" cy="${y}" r="3.4"/>`;
const label = (x, y, value, style = '', anchor = 'start') =>
  `<text x="${x}" y="${y}" class="${style}" text-anchor="${anchor}">${value}</text>`;
const box = (x, y, width, height, cls = 'box') =>
  `<rect class="${cls}" x="${x}" y="${y}" width="${width}" height="${height}" rx="5"/>`;

function sheet(id, height, description, content) {
  document.getElementById(id).innerHTML =
    `<svg viewBox="0 0 1000 ${height}" role="img" aria-label="${description}">${content.join('')}</svg>`;
}

sheet('power-sheet', 350, 'Battery positive and negative wiring to both drivers and the Pi converter', [
  box(12, 91, 185, 106), label(26, 116, 'Battery pack / BMS', 'heading'),
  label(182, 144, 'PACK +', '', 'end'), label(182, 177, 'PACK −', '', 'end'),
  wire('M197 139H243', 'power'), box(243, 122, 156, 34, 'part'),
  label(321, 144, 'Manual disconnect', '', 'middle'),
  wire('M399 139H484V65H730M484 139V245H730M484 139V151H730', 'power'),
  wire('M197 172H534V95H730M534 172V275H730M534 172H730', 'ground'),
  junction(484, 151, 'power'), junction(534, 172, 'ground'),
  label(550, 58, 'BAT+ branch', 'small'), label(550, 89, 'BAT− return', 'small'),
  box(730, 32, 252, 78), label(744, 54, 'Left ZS-X11H V2', 'heading'),
  label(744, 71, 'VCC'), label(744, 101, 'Power GND'),
  box(730, 117, 252, 77), label(744, 135, 'Right ZS-X11H V2', 'heading'),
  label(744, 156, 'VCC'), label(744, 178, 'Power GND'),
  box(730, 216, 252, 80), label(744, 238, 'DC–DC converter', 'heading'),
  label(744, 261, 'IN+'), label(744, 282, 'IN−'),
  label(966, 260, 'OUT+ 5 V', '', 'end'), label(966, 282, 'OUT−', '', 'end'),
  label(12, 323, 'Converter OUT+/OUT− continue to the Pi’s USB-C power input through a compatible supply lead.'),
  label(12, 344, 'Use a non-isolated converter for the common-ground layout shown. Keep motor return current on the main power wiring.', 'small')
]);

function motorSheet(side, pwm, direction, hallPins, buffer, transistor) {
  const content = [
    box(12, 30, 201, 360), label(26, 54, side + ' ZS-X11H V2', 'heading'),
    box(361, 30, 263, 360), label(373, 54, side + ' hoverboard motor', 'heading'),
    box(812, 30, 176, 601), label(825, 54, 'ESP32-S3-Zero', 'heading'),
    label(198, 77, 'VCC', '', 'end'), label(198, 104, 'Power GND', '', 'end'),
    label(26, 77, 'BAT+ →'), label(26, 104, 'BAT− →'),
    label(26, 137, 'Phase screw block', 'small'), label(373, 137, 'Phase leads', 'small')
  ];
  ['MA', 'MB', 'MC'].forEach((pin, index) => {
    const y = 159 + 24 * index;
    content.push(wire(`M213 ${y}H361`, 'phase'), label(199, y+5, pin, '', 'end'),
      label(373, y+5, 'Matched phase ' + (index + 1)));
  });
  content.push(label(26, 238, 'White Hall header', 'small'), label(373, 238, 'Motor Hall harness', 'small'),
    box(650, 241, 130, 135, 'part'), label(715, 261, buffer + ' · LVC3G17', '', 'middle'));
  const inputs = [1, 3, 6];
  const outputs = [7, 5, 2];
  ['HA', 'HB', 'HC'].forEach((pin, index) => {
    const y = 278 + 27 * index;
    const tap = 244 + 32 * index;
    const route = 408 + 21 * index;
    content.push(wire(`M213 ${y}H361 M${tap} ${y}V${route}H${631+6*index}V${y}H650 M780 ${y}H812`, 'hall'),
      junction(tap, y, 'hall'), label(199, y+5, pin, '', 'end'), label(373, y+5, pin),
      label(659, y+5, inputs[index] + ' IN', 'small'), label(771, y+5, 'OUT ' + outputs[index], 'small', 'end'),
      label(825, y+5, 'GPIO' + hallPins[index] + ' / ' + pin));
  });
  content.push(
    wire('M213 250H361', 'power'), label(199,255,'Hall 5 V','','end'), label(373,255,'Hall 5 V'),
    wire('M213 365H361','ground'), label(199,370,'Hall GND','','end'), label(373,370,'Hall GND'),
    wire('M812 111H723V241','power'), label(733,105,'Pin 8 / 3V3','small'), label(825,116,'3V3 OUT'),
    wire('M812 200H751V223H770V241','ground'),
    wire('M723 164H751V176','power'), junction(723,164,'power'),
    wire('M739 176H763M739 188H763','ground'),
    wire('M751 188V200','ground'), junction(751,200,'ground'), label(659,184,'100 nF','small'), label(825,205,'GND'),
    label(774,230,'Pin 4','small'),
    box(12,480,201,224), label(26,505,'ZS-X11H controls','heading'),
    label(199,532,'PWM throttle','','end'), label(199,599,'Rotation','','end'),
    label(199,680,'Signal GND','','end'),
    wire('M213 527H812','control'), label(825,532,'GPIO'+pwm+' / PWM'),
    wire('M439 527V545M427 545H451V572H427V545M439 572V675','ground'),
    junction(439,527,'control'), junction(439,675,'ground'), label(456,562,'10 kΩ','small'),
    box(551,577,108,74,'part'), label(605,596,transistor+' · 2N3904','','middle'),
    label(560,613,'C','small'), label(647,613,'B','small','end'), label(605,638,'E','small','middle'),
    wire('M213 594H520V608H551','control'),
    wire('M812 608H772M707 608H659','control'), box(707,597,65,22,'part'),
    label(739,613,'4.7 kΩ','small','middle'), label(825,613,'GPIO'+direction+' / DIR'),
    wire('M683 608V622M673 622H693V649H673V622M683 649V675','ground'),
    junction(683,608,'control'), label(697,642,'100 kΩ','small'),
    wire('M605 651V675M213 675H996V562H988','ground'),
    junction(605,675,'ground'), junction(683,675,'ground'), label(976,567,'GND','','end'),
    label(12,732,'ESP32 GND, both driver signal grounds and both buffer grounds share the battery-negative reference.','small'),
    label(12,753,'Hall 5 V stays on the motor harness. The three tap dots represent splices or a parallel breakout on that harness.','small')
  );
  sheet(side.toLowerCase() + '-sheet', 766, side + ' motor terminal wiring with Hall taps, Schmitt buffer, PWM pull-down and NPN direction switch', content);
}

motorSheet('Left', 4, 6, [8, 10, 11], 'U1', 'Q1');
motorSheet('Right', 5, 7, [9, 12, 13], 'U2', 'Q2');

sheet('connector-sheet', 262, 'ZS-X11H V2 connector layout from the supplied photograph', [
  box(5,10,990,242), label(500,42,'ZS-X11H V2 · component side','heading','middle'),
  label(26,67,'Left screw block · top to bottom','small'),
  ...['MA', 'MB', 'MC', 'VCC', 'Power GND'].map((pin,i)=>label(26,94+24*i,`${i+1}  ${pin}`)),
  label(968,67,'Right screw block · top to bottom','small','end'),
  ...['5 V out · unused','Analog speed · unused','Signal GND','Rotation','Brake · unwired','Stop · unwired'].map((pin,i)=>label(968,94+24*i,`${i+1}  ${pin}`,'','end')),
  label(367,119,'Bottom-left white connector','heading','middle'),
  label(367,146,'GND | Speed Out | 5 V out | PWM | GND','','middle'),
  label(367,173,'Take PWM and GND. Leave Speed Out and 5 V out open.','small','middle'),
  label(674,192,'Bottom-right Hall connector','heading','middle'),
  label(674,218,'GND | HA | HB | HC | 5 V','','middle'),
  label(674,237,'Full motor harness plugs in here.','small','middle')
]);

const piContent = [
  box(350,18,270,313), label(368,46,'Raspberry Pi 4B','heading'),
  box(10,38,217,63), label(22,62,'Regulated supply','heading'),label(22,84,'Converter OUT+ and OUT−'),
  wire('M227 69H350','power'), label(362,75,'USB-C power · 5 V / 3 A'),
  box(760,43,230,56), label(774,75,'ESP32 USB-C'),
  box(760,103,230,56), label(774,135,'Lidar USB adapter'),
  box(760,163,230,56), label(774,195,'USB microphone'),
  box(760,223,230,82), label(774,246,'Powered speaker','heading'),
  label(774,269,'USB power'), label(774,292,'3.5 mm audio')
];
[['USB-A · data + 5 V',71],['USB-A · lidar',131],['USB-A · microphone',191],
  ['USB-A · speaker power',264],['3.5 mm audio out',287]].forEach(([name,y])=>{
  piContent.push(label(607,y+4,name,'','end'),wire(`M620 ${y}H760`,'control'));
});
piContent.push(box(350,360,270,142),box(760,360,230,142),label(370,384,'Pi 40-pin header','heading'),
  label(774,384,'BMP390 · JST-SH','heading'));
[['Physical pin 1 · 3.3 V','3.3 V','power'],['Physical pin 3 · GPIO2','SDA','control'],
 ['Physical pin 5 · GPIO3','SCL','control'],['Physical pin 6 · GND','GND','ground']].forEach(([pin, signal, kind],i)=>{
  const y=404+25*i;
  piContent.push(label(607,y+5,pin,'','end'),wire(`M620 ${y}H760`,kind),label(774,y+5,signal));
});
sheet('pi-sheet',520,'Pi 4B power, USB serial, lidar, audio and optional I2C altimeter wiring',piContent);
