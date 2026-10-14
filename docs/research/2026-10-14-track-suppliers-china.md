# Tank running-gear suppliers in China (research, 2026-10-14)

**Scope (owner update):** the chassis will be laser-cut acrylic, so we only need to source the **running gear**. That means tracks, sprockets, road wheels, swing arms, springs, caps, axles and TT motors, in the ELEGOO Conqueror / DFRobot Devastator style. Complete chassis kits are a fallback: buy one and keep only its running gear.

**Method and limits:** web search plus page fetches. Alibaba product pages came back empty, AliExpress refused the connection, Walmart blocked the fetch, and 1688 returned no listings. **Alibaba, AliExpress and 1688 still need a manual search.** Use these terms: 履带底盘 减震, 坦克底盘 悬挂 TT电机, 塑料履带 驱动轮 负重轮, 机器人坦克 摆臂 拉簧. Prices are USD as shown on 2026-10-14 unless stated otherwise. They are list or retail prices, not quotes.

## Key findings

- **No source sells an exact Conqueror-equivalent set**: a TT-shaft sprocket, 4 road wheels per side, swing arms and tension springs together. The closest is Sinoning, which sells track, wheel and suspension-wheel parts separately.
- **No OEM identified for the Conqueror or Devastator track style.**
  - ELEGOO lists no spare parts.
  - DFRobot sells the complete Devastator: ROB0112 at $69.90 and ROB0128 at $84.90 on dfrobot.com. It shows no parts catalogue and no published B2B programme. A 2017 forum request for replacement track links was pointed to support email. Volume enquiries go through the "DFRobot Distributors" page. TME lists ROB0112 as withdrawn.
- Most "shock-absorbing" kits use **25 mm, 33GB/520 or 37 mm motors, not TT**. The motor class is marked in the table.
- Sinoning's SNT2 listing and Smaring's Tindie track kit use **identical text**: 4.5 cm × 78 cm, about 100 links, 50 mm wheels, M3/M4. They probably come from one factory source (unverified).
- Several Sinoning parts are labelled "Original DOIT" or "DOIT …". This suggests a DOIT link, but **it is unverified**. Do not treat Sinoning as DOIT's official store. DOIT's legal name also differs between sources (Shenzhen Doctors of Intelligence & Technology on its wiki, Shenzhen Sibo Zhilian on LCSC).

## Brand check

| Brand | Tracked chassis? | Suspension? |
|---|---|---|
| Hiwonder | Yes | Yes. Its "Raptor" chassis has patented torsion-spring swing-arm suspension (used in MentorPi T1). Its cheaper tank kit gets damping only from the tracks. |
| XiaoR GEEK (Shenzhen) | Yes. Tindie lists an encoder-motor tank chassis at $99. | Claims "shock absorbing" but does not describe the design. |
| Smaring / SmaringWing (Shenzhen) | Yes: TS100, TS400 and TT02/TT04 kits, plus parts | TS-series has suspension. The Tindie store is paused ("taking a break", no shipping). |
| Yahboom | G1 tank only | Not confirmed for the tank. Its suspension products are wheeled. |
| Keyestudio, Adeept | Yes | No |
| Waveshare (UGV), SunFounder | Wheeled | Not relevant |

## Ranked candidates

| # | Supplier / store | Product(s) and link | What's included | Motor | Price / MOQ (seen 2026-10-14) | Parts sold separately? | OEM? |
|---|---|---|---|---|---|---|---|
| 1 | **Sinoning** (Longhua, Shenzhen), own web store | SNT2 track set: sinoning.com/product/1set-plastic-wheel-track-with-bearing-wheel-driving-wheel-track-for-tank-chassis-tracked-vehicle-motor/ · SNT15 "XDC75" wheel set: …/xdc75-metal-track-wheel-set-compatible-with-tt-motor-robot-tank-chassis-accessories-diy-aluminum-snt15/ · "Original DOIT" suspension road wheel: …/original-doit-shock-absorption-metal-bearing-wheel-for-robot-tracked-tank-car-chassis-transmission-wheel-with-suspension/ | SNT2: plastic track 4.5 cm × 78 cm (about 100 links), grey plastic drive wheel 50×33, road wheel 50×35. SNT15: 46-link track 4.5 cm wide, 55 mm drive wheel, 48 mm idler, couplings "suitable for TT motors". Suspension wheel: 38 mm aluminium wheel, spring, bearing, suspension part, M4 hardware, left/right versions (made for the TS300) | TT (SNT15). Others not stated. | $4.99 (SNT2), $6.20 (SNT15), $8.76 (suspension wheel). Retail, no MOQ stated. | **Yes**: separate "Tank Spare parts" and track categories | Wholesale page invites quotes for "larger or customized" orders. OEM not stated. |
| 2 | **Shenzhen Ke Zhi You Technology (KZY)**, Shenzhen, Made-in-China | T200 / FUT5061 tank chassis: kzyeducabot.en.made-in-china.com/product/mChQskBdpZWN/ | 300×220×55 mm, aluminium body, metal wheels, engineering-plastic track, PS2 controller | 25 mm 9 V gear motor with encoder | $83.80 (5–9 pcs), $83 (10–99), $82 (100–499), $81 (500+). Sample $84. | Not listed. Needs RFQ. | "Customization: Available". **CTI-audited, Gold member since 2018.** |
| 3 | **Shenzhen Tianhehuijin Tech (Skyriver)**, Longgang, Shenzhen, Made-in-China | TS100 shock-absorption chassis: skyriver.en.made-in-china.com/product/lQBRbyGHCsro/ | 275×190×95 mm, 1.1 kg, aluminium body, plastic track, 2 metal drive wheels, 10 road wheels, "suspension system" (no details) | 25 mm 12 V or 33-series | Negotiable. MOQ 20 pcs. | Not listed. Needs RFQ. | "Customized" option shown but unclear. ISO certificates listed. Trading company, no audit badge. |
| 4 | **Smaring** (Nanshan, Shenzhen), Tindie | Track + drive + road wheel kit: tindie.com/products/Smaring/robot-tank-car-diy-kit-with-track-wheel/ · wheel/track coupling kit · 3818 track · TS100/TS400 chassis | Same 4.5 × 78 cm plastic track. Aluminium road wheels 48.3 mm. Full TS100 chassis: 10 road wheels, 25 mm motors, $92.86. | 25 mm (TS100) | $15.38 (track kit). **Unavailable: seller paused.** | Yes | Not stated. Try contacting them directly. |
| 5 | **Shenzhen Tianzhongtian Trading**, Alibaba / GoldSupplier | TS100 shock-absorber tank (plus arm/WiFi option): alibaba.com/product-detail/TS100-12V-Motor-7-DOF-Robot_60824922246.html | 275×255×90 mm, **plastic track 41 mm wide**, about 7 kg load | #37 12 V | Price not shown. MOQ 1 set. | Not listed | Logo printing after sample approval. Trading company. Alibaba page did not load, so badges are **unverified**. |

**Also worth a look:**

- **Heng Long (1:16 RC tank maker):** a strong lead for volume running gear. Sinoning's SN2300 ($69) is built on a Heng Long 3838 base with independent suspension. Heng Long tracks, road wheels, swing arms and springs are sold widely as RC spares. Scale and TT fit are unverified.
- **Guangdong Baijia Baiter** (Zhongshan): a factory that makes Rover 5-style ABS tank chassis. MOQ 12, price negotiable. It shows no suspension, so it is a fallback only.
- **Sinoning SN2700** ($85.90, spring "damping balance" chassis): uses 33GB-520 motors.
- **Amazon SmaringWing TT02/TT04 kits:** TT-motor tracked kits, but with **no suspension**.
- **Hiwonder:** to get Raptor-chassis parts, contact Hiwonder B2B. No parts listing was found.

## RFQ message (English)

> Hello, we build small educational robot tanks (India, selling worldwide). We need **running-gear sets only (no chassis plate, no electronics)**. Per tank:
> - 2 × pinned-link plastic track belts with tread pads
> - 2 × toothed drive sprockets for the TT gear-motor shaft
> - 8 × road wheels (4 per side)
> - ~8 × suspension swing arms
> - ~6 × tension springs
> - wheel caps
> - shoulder screws / axles
> - 2 × TT gear motors (encoder TT motors preferred)
>
> Please quote **with and without motors**, and tell us:
> - material
> - track width, pitch and link count
> - sprocket teeth and diameter
> - road-wheel diameter
> - sample price
> - unit price at 100 / 500 / 1000 sets
> - MOQ
> - lead time
> - shipping cost to India (and DDP worldwide options)
> - whether you offer OEM (colour, logo, packaging, custom parts)
>
> Drawings and photos attached. Do you also sell a complete tracked chassis with this suspension? Thank you.

(Count the "~" quantities from the kit and fix them before sending. Suppliers quote on exact counts.)

## RFQ message (简体中文)

> 您好,我们在印度生产小型教育机器人坦克(销往全球)。只需要**行走系统套件(不要底板,不要电子件)**,每台:
> - 履带(销连接,带胶纹)2条
> - 驱动轮(适配TT电机轴)2个
> - 负重轮 8个(每边4个)
> - 摆臂 约8个
> - 拉簧 约6个
> - 轮盖
> - 台阶螺丝/轮轴
> - TT减速电机 2个(优先带编码器)
>
> 请分别报价**含电机和不含电机**,并告知:
> - 材质
> - 履带宽度、节距、节数
> - 驱动轮齿数和直径
> - 负重轮直径
> - 样品价
> - 100 / 500 / 1000 套单价
> - 最小起订量
> - 交期
> - 到印度运费(及全球DDP)
> - 是否可OEM(颜色、Logo、包装、定制件)
>
> 附图纸和照片。如有同款减震履带底盘整机也请报价。谢谢!

## Measurement checklist (take from the ELEGOO tank before sending)

1. **Track shoe:** width, pitch (centre-to-centre of pins; measure across 10 links and divide by 10), link count per side, pin diameter and material, tread pad yes/no.
2. **Drive sprocket:** tooth count, outside diameter, bore shape (measure the TT shaft flats; do not assume), hub length.
3. **Road wheels:** diameter, width, bore and axle diameter, bearing or plain bore, count per side.
4. **Idler** (if separate from road wheels): diameter, bore, tension adjustment method.
5. **Swing arms:** pivot-to-axle length, pivot spacing along the hull, arm thickness.
6. **Springs:** free length, wire and coil diameter, hook style, how many per side.
7. **Layout:** axle spacing (sprocket to idler, wheel to wheel), ground clearance, suspension travel (how far a wheel rises before the arm stops).
8. **Motor:** TT type (straight or 90°), gear ratio and voltage, shaft length, encoder type.
9. **Hardware:** caps, shoulder-screw diameter and length.
10. **Photos:** a photo of every part next to a ruler, plus the assembled side view.

## Vetting tips

- On Alibaba, pay through **Trade Assurance** only. Prefer **Verified Supplier** (Alibaba) or **audited / Gold** (Made-in-China) badges, and check "manufacturer" against "trading company". Most hits here were trading companies.
- **Order samples from 2–3 suppliers first.** Test fit on the acrylic chassis and run a wear test on the track pins before any volume order.
- Ask for the factory's own photos and videos of the moulds. Ask whether they own the mould, which matters for future supply and for custom changes.
- For 1688 and Taobao (often 30–50% cheaper, Chinese only), use a **sourcing agent** or forwarder (Superbuy, CSSBuy, or a Yiwu/Shenzhen agent). The agent can combine parts from several shops into one shipment to India.
- Confirm the **HS code and the import duty in India**, and ask for DDP quotes for worldwide customers.
