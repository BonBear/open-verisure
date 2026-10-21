# Protokoll, RF & Kryptografi: Verisure Sub-1GHz AES-128

Detta dokument beskriver den fullständiga specifikationen för radiosändningen på 868.950 MHz, den kryptografiska nyckelhärledningen (KDF), paketstrukturen och replay-attack-skyddet i gatewayen.

---

## 🔑 Nyckelhärledning (Key Derivation Function - KDF)

Verisure-systemet använder en hierarkisk nyckelstruktur där varje sensor har en unik **Device AES Key**, härledd ur en central **Master Key** och sensorns 8-tecken Device ID (t.ex. `2JHL2MVN`).

### Matematiskt Flöde
$$\text{DeviceKey} = \text{AES-128-ECB}_{\text{MasterKey}}(\text{DeviceID}_{\text{ASCII}} \,\|\, \text{Padding})$$

1. **Master Key (16 bytes):** Genereras eller hämtas från centralenheten/gatewayen (t.ex. `01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10`).
2. **Input Block (16 bytes):** De 8 ASCII-tecknen från sensorns ID noll-paddade till 16 bytes:
   ```text
   ['2', 'J', 'H', 'L', '2', 'M', 'V', 'N', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
   ```
3. **Kryptering:** Blocket krypteras en gång med AES-128 i ECB-läge med Master-nyckeln. Resultatet blir sensorns permanenta 16-bytes AES-128 enhetsnyckel.
4. **Verktyg:** Använd [`derive_key.py`](file:///c:/repo/Antigravity/derive_key.py) för att automatiskt härleda nyckeln och uppdatera sensorns `config.h`.

---

## 📦 Paketstruktur (24 Bytes + 2 Bytes Hårdvaru-CRC)

Radion sänder ett paket med fast längd på **24 bytes**, följt av 2 bytes hårdvarugenererad CC1101/CC1110 CRC16.

```
+--------------------------------+-------------------------------------------------+
|   Bytes 0..7: Plaintext Header  |          Bytes 8..23: AES-128 Ciphertext         |
|   (8-tecken Device ID)         |          (Krypterad med Device Key)             |
+--------------------------------+-------------------------------------------------+
```

### 1. Plaintext Header (Bytes 0..7)
Innehåller sensorns 8-tecken alfanumeriska ID (t.ex. `2JHL2MVN`). Gatewayen läser detta i klartext för att identifiera vilken enhet som sänder och hämta motsvarande härledda enhetsnyckel ur sessionstabellen.

### 2. Krypterad Payload (16 Bytes AES-128 Block)
Avkodas av gatewayen till följande 16 byte telemetri:

| Byte Index | Fält | Datatyp | Beskrivning |
| :---: | :--- | :---: | :--- |
| **`0..1`** | **Sekvensnummer** | `uint16_t` (Big-Endian) | Rullande räknare (`0` till `65535`), ökar för varje sänt paket. |
| **`2`** | **Sensorflaggor** | `uint8_t` (Bitmask) | Sensorstatusbitar (se tabell nedan). |
| **`3`** | **Batteriprocent** | `uint8_t` | Uppskattad laddning `0`–`100 %`. |
| **`4..5`** | **Batterispänning**| `uint16_t` (Big-Endian) | Uppmätt spänning i millivolt (t.ex. `3370 mV` för 3.37 V). |
| **`6..9`** | **Device ID Echo** | `char[4]` | De 4 första tecknen i Device ID (integritetskontroll mot nyckelfel). |
| **`10..11`**| **Magic Bytes** | `char[2]` | Fast signatur: `'V'`, `'S'` (Verisure Sensor). |
| **`12`** | **Port 0 Diagnostik**| `uint8_t` | Råvärde från CC1110 `P0` (P0_1 Tamper, P0_2/P0_4 LED). |
| **`13`** | **Port 1 Diagnostik**| `uint8_t` | Råvärde från CC1110 `P1` (P1_7 Reed, P1_2 Shock). |
| **`14`** | **Port 2 Diagnostik**| `uint8_t` | Råvärde från CC1110 `P2` (P2_0, P2_1 DD, P2_2 DC). |
| **`15`** | **Reset Cause** | `uint8_t` | `SLEEP.RST[1:0]` (0=Power-On, 1=Extern Reset). |

### Flaggbitar (Byte 2)
```text
Bit 0 (0x01): Magnet Reed Kontakt (1 = Öppen / Larm, 0 = Stängd)
Bit 1 (0x02): Skak / Vibrationssensor (1 = Stöt detekterad, 0 = Lugnt)
Bit 2 (0x04): Sabotage / Tamper (1 = Locket av / Larm, 0 = Locket på / Säkrad)
Bit 3 (0x08): Power-On Reset (1 = Nytt batteri isatt, aktivt de första 3 paketen)
Bit 4 (0x10): Låg batterivarning (1 = Spänning under 2400 mV)
Bit 5..7:     Reserverade (0)
```

---

## 🛡️ Säkerhet & Replay-Attack-Skydd

Gatewayen implementerar ett strikt 4-stegs verifieringsfilter:
1. **Hårdvaru-CRC16:** Paketet accepteras endast om CC1101:s inbyggda CRC-kontroll passerar (Bit 7 i LQI-statusbyten = 1).
2. **Kryptografisk Integritet:** Payloaden avkodas med sensorns enhetsnyckel. Om magic bytes inte är exakt `'V'`, `'S'`, eller om Device ID Echo inte matchar headern, avvisas paketet omedelbart som manipulerat eller krypterat med fel nyckel.
3. **Replay-Skydd via Sekvensnummer:**
   * Gatewayen sparar det senast sedda sekvensnumret för varje sensor.
   * Ett nytt paket måste ha `seq > last_seq` (med säker 16-bitars wraparound-hantering).
   * Om en angripare spelar in ett gammalt paket och sänder ut det igen, blockeras det direkt i mjukvaran.
   * **Undantag:** Om sekvensnumret nollställs i samband med batteribyte (`isPowerOn`) eller hårdvarureset accepteras omslaget säkert.
