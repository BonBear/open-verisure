# Mätprotokoll: blixtkortet (LED BOARD P/N3302042 C)

Mål: ta reda på vad de 8 stiften gör innan något drivs. Alla mätningar här är passiva (ohm och volt), så inget drivs.

## Det vi redan vet

- Kortet sitter i huvudkortets **J2** och **J3** (4 stift vardera). På själva blixtkortet heter kontakterna **J1** och **J2**.
- CC1110 **P0_7** (pin 13) går till huvudkortets **J2-4** via ~6 kΩ. P0_7 = 1 kallas i dag "aux rail" i firmware och slås på tillsammans med Blackfin.
- På baksidan finns **U2 MCP6042** (två op-förstärkare), **Q3/Q6** och två **1R0**-motstånd. Det är två konstantströmskanaler med 2 LED vardera. Strömmen per kanal blir ungefär *styrspänning / 1 Ω*: 0,5 V på styringången ger ~0,5 A.
  **Därför:** ett styrstift får aldrig drivas direkt till 3,3 V innan vi vet vad som sitter framför op-förstärkarens ingång (det kan bli flera ampere).
- På fliken ovanför linsen sitter ljussensorn (6 ben). Blackfins TWI-buss ser en enhet på **0x5C**, som passar en BH1750 med ADDR hög. Benordning för BH1750 (WSOF6): 1 VCC, 2 ADDR, 3 GND, 4 SDA, 5 DVI, 6 SCL. Kontrollera mot det som står på kretsen.
- **C3** (47 µF tantal) ligger vid kontakten J1 på blixtkortet, troligen på LED-matningen.

## Del A: blixtkortet urtaget, strömlöst

### A1. Huvudkortets J2/J3 mot kända nät (ohm, nätaggregatet AV)

Pin 1 = fyrkantig pad / märkt 1.

| Stift | mot GND | mot VBAT-pad | mot 3,3 V (J5-1) | mot CC1110-pinne (P0_7 pin 13, P0_3 pin 9, P0_1 pin 6, P0_6 pin 12, P2_0 pin 14) | Anteckning |
| :--- | :--- | :--- | :--- | :--- | :--- |
| J2-1 | | | | | |
| J2-2 | | | | | |
| J2-3 | | | | | |
| J2-4 | | | | P0_7 ~6 kΩ (känt) | |
| J3-1 | | | | | |
| J3-2 | | | | | |
| J3-3 | | | | | |
| J3-4 | | | | | |

P0_3 är intressant: den är ansluten någonstans (strömmen steg 74→92 mA när den drevs hög) men vi vet inte var.

### A2. Blixtkortets J1/J2 mot komponenterna (ohm)

| Stift på blixtkortet | Ljussensorns ben (1–6) | U2-ben (3 och 5 = styringångar, 4 = GND, 8 = VCC) | LED-anoder / C3 | Anteckning |
| :--- | :--- | :--- | :--- | :--- |
| J1-1 | | | | |
| J1-2 | | | | |
| J1-3 | | | | |
| J1-4 | | | | |
| J2-1 | | | | |
| J2-2 | | | | |
| J2-3 | | | | |
| J2-4 | | | | |

Skriv också vilken av blixtkortets kontakter som sitter i huvudkortets J2 respektive J3.

## Del B: blixtkortet isatt, spänning (volt mot GND)

Mät på huvudkortets sida av kontakterna (eller på blixtkortets stift ovanifrån).

| Stift | Vila (rails av, kortet drar ~3 mA) | Kameran igång (efter rörelse framför PIR, ~0,17 A) | Anteckning |
| :--- | :--- | :--- | :--- |
| J2-1 | | | |
| J2-2 | | | |
| J2-3 | | | |
| J2-4 | | | |
| J3-1 | | | |
| J3-2 | | | |
| J3-3 | | | |
| J3-4 | | | |

## Efter mätningen

När vi vet vilket stift som styr strömmen (och vad som sitter framför op-förstärkaren) väljer vi styrningen:
- från CC1110 (om styrstiftet går dit), eller
- från Blackfin (om det går via TWI/GPIO, då styrs blixten precis vid exponeringen).
Första provet görs med nätaggregatets strömgräns låg och kort puls.
