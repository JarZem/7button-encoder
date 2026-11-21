# remotecontrol7andEncoder

Základní ESP-IDF projekt pro ESP32-C6-WROOM-1-N16:

- 7 samostatných tlačítek
- rotační encoder A/B
- tlačítko encoderu
- vstupy přes GPIO přerušení se softwarovým debounce
- fronta událostí
- Zigbee Home Assistant entity pro 6 vystupu, svetlo a RS232/OTA enable switche
- vlastní partition tabulka pro 16 MB flash a OTA

## Zapojení

Tlačítka i encoder spínají příslušný GPIO proti GND. Vstupy nepoužívají interní pull-up, počítá se s externím pull-up odporem v zapojení.

Doporučené zapojení tlačítek:

```text
3,3 V
  |
 10 kOhm
  |
  +---- 100 Ohm ---- GPIO
  |                  |
tlacitko           100 nF
  |                  |
 GND                GND
```

U encoderu se pocita se stejnym principem, jen kondenzator muze byt mensi, typicky 10 nF.

Výchozí piny jsou v `main/pins.h`.

Vstupy tlačítek a tlačítko encoderu jsou aktivní v úrovni L, spínají proti GND a jsou obsluhované přes GPIO přerušení na obě hrany. Po přerušení se stav potvrdí až po SW debounce okně, takže se neprovádí žádné pravidelné scanování L úrovně. Vstupy A/B rotačního encoderu jsou také obsluhované přes přerušení a směr se určuje z posloupnosti kvadraturních stavů.

## Debug rezim

Debug rezim se aktivuje automaticky, kdyz je pripojena programovaci USB Serial/JTAG linka. Nepouziva RS232 linku mezi moduly, takze pri OTA a beznem provozu zustavaji UART piny pro druhy ESP modul vyhrazene.

Po zapnuti nebo resetu vypise nacteny stav z NVS jako `STATE reason=boot_loaded ...`. Pri kazde lokalni zmene vypise aktualni stav jako `STATE reason=local ...`. Pri zmene z Home Assistantu vypise navic radek `HA_CHANGE ...` a potom kompletni `STATE reason=zigbee ...`.

## Status LED

Status LED na `PIN_STATUS_LED` / GPIO8 je rizena centralne v `main/status_led.c` pres `esp_timer`. Ostatni casti aplikace pouze nastavi stav nebo poslou kratky indikacni pozadavek; nikde se neceka v loopu ani pres blokujici sleep kvuli bliknuti.

Vychozi vyznam barev:

```text
zluta jednorazove po resetu        boot zacal
fialova blikani                    zarizeni neni v Zigbee siti / nekomunikuje
modra blikani                      probiha Zigbee parovani / network steering
zelena kratce 1x za cca 10 s       vse bezi a Zigbee je pripojene
zelena -> zluta kratce             odesila se stav/report do HA/Zigbee
zluta kratce                       prijat prikaz z HA
zelena kratce                      lokalni aktivita/tlacitko/encoder
OTA oranzova                       OTA pozadavek ceka ve fronte
OTA tyrkysova                      OTA pripojuje Wi-Fi
OTA bila                           OTA stahuje firmware
OTA modra                          OTA overuje firmware
OTA zelena                         OTA uspesne dokonceni
OTA cervena                        OTA chyba
cervena blikani                    opakovane selhani mimo OTA, napr. neuspesny Zigbee steering
rychle cervena blikani             fatalni chyba
```

Po pripojeni napajeni nebo resetu ma LED delat toto:

```text
1. kratce blikne zlute
   - firmware zacal bootovat a status LED driver uz bezi

2A. pokud je modul uz v Zigbee siti
   - po startu Zigbee zustane v provoznim stavu
   - pri uvodnim odeslani reportu do HA blikne zelene a hned potom zlute
   - potom v klidu kratce blikne zelene asi 1x za 10 s

2B. pokud modul neni v Zigbee siti / nema platne sparovani
   - zacne blikat modre pri network steering / parovani
   - po uspesnem sparovani prejde na provozni zeleny heartbeat
   - pokud parovani selze, prejde do cerveneho blikani a dalsi pokusy jsou logovane na COM

2C. pokud modul neni pripojeny nebo neumi komunikovat se Zigbee siti
   - blika fialove jako stav bez komunikace

3. pri rucnim znovuparovani
   - stisk BOOT na GPIO9 nebo soucasny stisk tlacitek 1 + 7 posle pozadavek na znovuparovani
   - LED zacne blikat modre
   - pokud bylo zarizeni uz v siti, smaze Zigbee pairing storage a restartuje se do parovani

4. pri odeslani stavu do HA
   - vzdy kratce blikne zelene a potom zlute

5. pri OTA
   - oranzova: OTA pozadavek ceka ve fronte
   - tyrkysova: pripojuje se Wi-Fi
   - bila: stahuje firmware
   - modra: overuje SHA256 firmware
   - zelena: OTA uspesne dokoncena
   - cervena: OTA chyba, firmware se nesmi nastavit jako bootovaci
```

## Usporny provoz

Projekt ma zapnuty ESP-IDF power management, tickless idle a automaticky light sleep. Zigbee cast je nakonfigurovana jako end-device, aby se firmware stabilne paroval se siti. Behem hledani a parovani si modul drzi napajeci zamek; po pripojeni muze zbytek aplikace mimo aktivitu prechazet do lehkeho spanku a vstupy GPIO zustavaji zdrojem probuzeni.

## Zigbee2MQTT / Home Assistant

Aktualni firmware se hlasi jako:

```text
manufacturerName: JaroslavZ
modelID: ESP32-C6-ENC
```

Externi converter uci Zigbee2MQTT identitu `ESP32-C6-ENC` a ponechava kompatibilitu se starsim parovanim `Jaros / RemoteControl7Encoder`. Vystavuje vystupy, sdilenou hodnotu svetla a RS232/OTA enable switche.

OTA/provisioning transport neni Home Assistant GUI entita. Python OTA server posila pres MQTT interní Zigbee2MQTT `set` vlastnost `ota_command`; converter ji prevede na custom Zigbee zapis. Tato vlastnost zustava v `toZigbee`, ale neni v `exposes`, aby se v HA nevytvarel rucni textovy ovladac s citlivymi daty.

OTA prikaz se posila jako jeden string:

```text
SSID|PASSWORD|HOST|CODE|TOKEN
```

Converter ho zapise na endpoint 1 do custom clusteru `0xFC00`, atribut `0x0001`. Hodnota je write-only a necte se zpatky, aby se heslo a token nedrzely ve stavu Home Assistantu.

```text
zigbee2mqtt/remotecontrol7andEncoder.mjs
```

Postup:

1. V Zigbee2MQTT povol externi JS konvertory, pokud je instalace ma vypnute:

```yaml
advanced:
  enable_external_js: true
```

2. Nahraj obsah `zigbee2mqtt/remotecontrol7andEncoder.mjs` pres Zigbee2MQTT frontend: Settings -> Dev console -> External converters, nebo soubor uloz do slozky:

```text
/config/zigbee2mqtt/external_converters/remotecontrol7andEncoder.mjs
```

V Home Assistant OS / Supervised je to typicky tento postup:

```text
Home Assistant -> Zigbee2MQTT -> Nastaveni -> External converters
```

Tam vytvor/otevri converter `remotecontrol7andEncoder.mjs` a vloz do nej obsah souboru z tohoto projektu:

```text
D:\Espressif\project\remotecontrol7andEncoder\zigbee2mqtt\remotecontrol7andEncoder.mjs
```

Pokud nepouzivas editor ve frontend UI, stejny soubor uloz pres File editor / Samba / SSH do Zigbee2MQTT konfigurace:

```text
/config/zigbee2mqtt/external_converters/remotecontrol7andEncoder.mjs
```

Ve starsich instalacich Zigbee2MQTT muze byt misto toho potreba pridat soubor do `configuration.yaml`:

```yaml
external_converters:
  - external_converters/remotecontrol7andEncoder.mjs
```

3. Restartuj Zigbee2MQTT uplnym Stop -> Start add-onu.
4. U jiz sparovaneho zarizeni obvykle staci v Zigbee2MQTT spustit re-interview/reconfigure nebo restartovat Zigbee2MQTT. Po nacteni noveho converteru se ma objevit entita `ota_command`. Mazani flash a permit join nech az na situace, kdy se meni Zigbee descriptor nebo je v Z2M ulozena stara interview cache.

Pokud je nutne udelat uplne ciste sparovani, smaz Zigbee storage oblasti. Pro COM3 a aktualni partition tabulku:

```powershell
python -m esptool --chip esp32c6 -p COM3 erase_region 0xE60000 0x5000
```

Converter pri configure cte aktualni stav znamych endpointu. OTA atribut zamerne necte, protoze payload obsahuje citliva data.

### Zigbee kanal pri parovani

Zigbee parovani nepouziva slepe vzdy celou sit. Firmware ma dvoustupnovy postup:

```text
1. nacti posledni uspesny Zigbee kanal z NVS klice remote/zb_ch
2. pokud neni ulozeny, pouzij build konfiguraci CONFIG_APP_ZIGBEE_DEFAULT_CHANNEL
3. zkus network steering jen na tomto rychlem kanalu
4. po CONFIG_APP_ZIGBEE_FAST_STEERING_ATTEMPTS neuspesnych pokusech prepni na CONFIG_APP_ZIGBEE_SCAN_CHANNEL_MASK
5. po uspesnem sparovani nebo po bootu uz sparovaneho zarizeni uloz skutecny kanal z esp_zb_get_current_channel() zpet do NVS
```

Vychozi hodnoty v `main/Kconfig.projbuild`:

```text
CONFIG_APP_ZIGBEE_DEFAULT_CHANNEL=11
CONFIG_APP_ZIGBEE_FAST_STEERING_ATTEMPTS=3
CONFIG_APP_ZIGBEE_SCAN_CHANNEL_MASK=0x07FFF800
```

Maska `0x07FFF800` pokryva Zigbee kanaly 11..26. Pokud ma byt fallback omezeny, uprav tuto masku v build konfiguraci. Napriklad jen kanal 11 je `0x00000800`.

V COM logu hledej:

```text
Zigbee last successful channel loaded from NVS: 11
Zigbee fast-pair channel from NVS: 11
Zigbee steering channel mask=initial fast 0x00000800 fast_channel=11 fallback_mask=0x07fff800 attempts=3
Zigbee last successful channel saved to NVS: 11
```

## Vytvoření a sestavení ve Windows

Spusť **ESP-IDF PowerShell**:

```powershell
cd D:\Espressif\project\remotecontrol7andEncoder
idf.py set-target esp32c6
idf.py fullclean
idf.py build
idf.py -p COM6 flash monitor
```

COM port uprav podle skutečnosti.

Pro pohodlné otevření editoru z kořene projektu můžeš použít:

```bat
open-vscode.bat
```

Skript se přepne do složky projektu, aktivuje ESP-IDF prostředí, spustí `idf.py reconfigure` kvůli `build/compile_commands.json` a potom otevře VS Code příkazem `code .`.

## Překopírování projektu

Obsah ZIP archivu rozbal do:

```text
D:\Espressif\project\remotecontrol7andEncoder
```

## OTA

Partition tabulka obsahuje `ota_0`, `ota_1`, `otadata` a Zigbee partitions `zb_storage`/`zb_fct`. OTA se spousti v Python OTA serveru. Server vybere firmware, najde cilove ESP32-C6 zarizeni v HA/Zigbee2MQTT a posle kratky interní OTA payload do custom Zigbee atributu.

Projektova metadata firmware nejsou globalni nastaveni HA add-onu. Tento projekt je popsan v:

```text
device.project.json
```

Pred nahranim nove binarky do OTA serveru vytvor release metadata:

```powershell
python tools\generate_release_metadata.py --bin build\remotecontrol7andEncoder.bin
```

Vedle binarky vznikne `build\remotecontrol7andEncoder.release.json`. Do OTA serveru se nahrava `.bin` i odpovidajici `.release.json`; server z nej bere `ota_ecosystem`, model, roli, hardware, produkt, kanal a secure version.

## Secure Provisioning Algoritmus

Tahle sekce popisuje cilovy secure provisioning model pro novy OTA transport. Detailni audit a navrh jsou v:

```text
docs/ZIGBEE_SECURE_PROVISION_ANALYSIS.md
docs/ZIGBEE_SECURE_PROVISION_DESIGN.md
```

### Ucastnici

```text
Python OTA server
  - zna SERVER_SIGN_PRIVATE_KEY
  - zna DEVICE_ENC_PUBLIC_KEY pro kazde registrovane ESP
  - nezna DEVICE_ENC_PRIVATE_KEY

Zigbee2MQTT
  - pouze prevadi MQTT zpravu na Zigbee/ZCL command
  - nikdy nesmi videt SSID/PASSWORD/HOST plaintext
  - nedela kryptograficka rozhodnuti

ESP32-C6
  - zna SERVER_SIGN_PUBLIC_KEY
  - zna vlastni DEVICE_ENC_PRIVATE_KEY
  - uklada provisioning do NVS jako encrypted blob
```

Pouzite klice jsou oddelene podle ucelu:

```text
SERVER_SIGN_PRIVATE_KEY  -> podpis prikazu na Python serveru
SERVER_SIGN_PUBLIC_KEY   -> overeni podpisu v ESP

DEVICE_ENC_PUBLIC_KEY    -> server podle nej zasifruje provisioning pro konkretni ESP
DEVICE_ENC_PRIVATE_KEY   -> konkretni ESP podle nej provisioning desifruje

TLS_SERVER_PRIVATE_KEY   -> jen HTTPS server
FIRMWARE_SIGNING_PRIVATE_KEY -> az pro budouci podpis firmware
```

`SERVER_SIGN_PRIVATE_KEY` neni nahrada za device encryption key. Verejny serverovy podpisovy klic v BIN neni tajny. Device private encryption key nesmi byt spolecny pro vice ESP.

### Identita a registrace zarizeni

Kazde ESP ma stabilni `device_id`, dnes odvozene ze Zigbee IEEE adresy. Pri vyrobe nebo prvnim bezpecnem flash postupu se vytvori device encryption identita:

```text
1. ESP vygeneruje DEVICE_ENC_PRIVATE_KEY.
2. ESP z nej odvodí DEVICE_ENC_PUBLIC_KEY.
3. ESP ulozi private key lokalne.
4. Do Python server DB se zapise jen:
   - device_id
   - DEVICE_ENC_PUBLIC_KEY
   - product_role
   - hardware_revision
   - device_key_id
```

Python server nikdy nepotrebuje `DEVICE_ENC_PRIVATE_KEY`. Pokud se ztrati serverova DB, public key neni tajny, ale bez zalohy nejde pro dane ESP vytvorit novy encrypted provisioning envelope.

### PROVISION_CONFIG - co se sifruje a podepisuje

`PROVISION_CONFIG` slouzi pouze k ulozeni nebo zmene konfigurace, kterou ESP pozdeji pouzije pro OTA Wi-Fi a HTTPS server. Nepouziva se jako okamzity OTA trigger.

Plaintext vznikne pouze na Python serveru pred sifrovanim a potom kratce v ESP RAM pri validaci nebo pri realnem pouziti Wi-Fi:

```text
config_schema_version
wifi.ssid
wifi.password
wifi.security_type
wifi.channel
ota.host
ota.port
```

Provisioning se sifruje hybridne:

```text
Python:
  DEVICE_ENC_PUBLIC_KEY konkretniho ESP
  + ephemeral ECDH private key
      -> ECDH shared secret
      -> HKDF-SHA256
      -> AES-256-GCM key
      -> ciphertext + tag

ESP:
  DEVICE_ENC_PRIVATE_KEY konkretniho ESP
  + server ephemeral public key ze zpravy
      -> stejny ECDH shared secret
      -> HKDF-SHA256
      -> AES-256-GCM key
      -> decrypt + tag verify
```

Podepisuje se cely envelope bez pole `signature`, ne samotny plaintext:

```text
domain = "ESP-PROVISION-CONFIG-v1"
signed_data =
  domain
  || protocol_version
  || command_type = PROVISION_CONFIG
  || target_device_id
  || provision_counter
  || key_id
  || device_key_id
  || ephemeral_public_key
  || nonce
  || ciphertext
  || AEAD tag
```

Podpis dela Python pomoci `SERVER_SIGN_PRIVATE_KEY`. ESP ho overi pomoci `SERVER_SIGN_PUBLIC_KEY` jeste pred prijetim nove konfigurace.

### PROVISION_CONFIG - cislovani zprav

Provisioning ma vlastni persistentni monotónní counter:

```text
last_provision_counter
```

Pravidlo prijeti:

```text
incoming_counter > last_provision_counter  -> prijmout
incoming_counter <= last_provision_counter -> odmitnout
```

Counter je viditelny a neni tajny. Musi ale byt soucasti podepisovanych dat, aby utocnik nemohl zmenit jeho hodnotu.

Poradi zpracovani v ESP:

```text
1. prijmi PROVISION_CONFIG fragmenty
2. sloz envelope
3. zkontroluj velikost a format
4. zkontroluj target_device_id
5. over SERVER signature
6. zkontroluj provision_counter
7. zkus ECDH + HKDF + AES-GCM decrypt
8. validuj plaintext konfiguraci
9. uloz ORIGINAL encrypted envelope do candidate NVS slotu
10. over readback candidate slotu
11. prepni active slot
12. uloz last_provision_counter
13. smaz citlive RAM buffery
```

Pred prepsanim aktivni konfigurace musi byt novy blob desifrovatelny a validni. Pokud se cokoliv nepovede, zustava aktivni stary provisioning blob.

### Ulozeni v NVS

ESP neuklada plaintext:

```text
NEUKLADAT:
  SSID
  PASSWORD
  HOST
```

ESP uklada encrypted envelope:

```text
ota_prov/active_slot
ota_prov/blob0
ota_prov/blob1
ota_prov/last_prov_ctr
ota_prov/last_ota_ctr_dev
ota_prov/last_ota_ctr_role
```

`blob0` a `blob1` obsahuji puvodni sifrovany envelope. Aktivni slot se prepne az po uspesnem zapisu a readbacku candidate slotu.

ESP-IDF NVS Encryption je dalsi obranna vrstva, ne nahrada application-level sifrovani. Provisioning musi byt zasifrovany uz pri prenosu pres MQTT/Zigbee.

### Fragmentace PROVISION_CONFIG

Provisioning envelope se pravdepodobne nevejde do jednoho Zigbee packetu. Fragmentace patri do transportni vrstvy, ne do kryptografie.

Fragment obsahuje:

```text
message_id
fragment_index
fragment_count
total_length
fragment_data
```

ESP sestavi zpravu jen pokud:

```text
fragment_count <= max_fragment_count
total_length <= max_envelope_size
vsechny fragmenty maji stejny message_id
duplicitni fragment ma stejny obsah
vse prijde pred timeoutem
```

Kryptografie se overuje az nad slozenym envelope. Neplatna nebo neuplna fragmentace nesmi prepsat konfiguraci.

### OTA_CHECK - co obsahuje a co neobsahuje

`OTA_CHECK` je kratky trigger:

```text
zkontroluj server, ktery uz mas bezpecne ulozeny v provisioning blobu
```

`OTA_CHECK` nikdy neposila:

```text
SSID
PASSWORD
OTA_HOST
OTA_PORT
firmware download token
```

Obsahuje pouze netajna podepsana data:

```text
protocol_version
command_type = OTA_CHECK
key_id
target_device_id nebo product_role/scope
ota_check_counter
nonce
flags
not_before / expires
signature
```

`OTA_CHECK` se nesifruje, protoze neobsahuje tajemstvi. Musi byt ale podepsany a chraneny proti replay.

### OTA_CHECK - cislovani a replay ochrana

OTA trigger ma vlastni counter oddeleny od provisioningu:

```text
last_ota_check_counter
```

Pravidlo prijeti:

```text
incoming_counter > last_ota_check_counter  -> kandidat k prijeti
incoming_counter <= last_ota_check_counter -> replay/old packet, odmitnout
```

Counter je soucasti podpisu. Pokud se posila OTA_CHECK pro product/group scope, ESP drzi counter pro tento scope oddelene od device-target counteru.

Poradi zpracovani v ESP:

```text
1. prijmi OTA_CHECK
2. zkontroluj format a command_type
3. zkontroluj target/scope
4. over SERVER signature
5. zkontroluj ota_check_counter
6. zkontroluj not_before / expires
7. zkontroluj rate limit
8. az ted prepnout Zigbee -> Wi-Fi
9. nacist encrypted provisioning blob z NVS
10. desifrovat SSID/PASSWORD/HOST/PORT pouze do RAM
11. pripojit Wi-Fi
12. HTTPS OTA check/download
13. smazat citlive RAM buffery
```

Pokud podpis, counter, scope, expirace nebo format nesedi, ESP zustava na Zigbee a Wi-Fi se nespousti.

### OTA rate limit

Vedle counteru ma ESP hlidat minimalni interval mezi OTA pokusy:

```text
MIN_OTA_CHECK_INTERVAL_SECONDS
```

Podepsany `force` flag muze rate limit obejit jen tehdy, pokud je soucasti podepisovanych dat. Nepodepsany `force` neexistuje.

### Strucny tok celeho systemu

Provisioning:

```text
Python vybere device_id
Python nacte DEVICE_ENC_PUBLIC_KEY
Python zvysi provision_counter
Python zasifruje config pres ECDH/HKDF/AES-GCM
Python podepise envelope pres SERVER_SIGN_PRIVATE_KEY
Python posle MQTT -> Zigbee2MQTT -> OTA_CONTROL/PROVISION_CONFIG
ESP overi podpis
ESP overi counter
ESP desifruje a validuje config
ESP ulozi encrypted envelope do NVS
```

OTA check:

```text
Python zvysi ota_check_counter
Python podepise OTA_CHECK pres SERVER_SIGN_PRIVATE_KEY
Python posle MQTT -> Zigbee2MQTT -> OTA_CONTROL/OTA_CHECK
ESP overi podpis
ESP overi counter/scope/rate limit
ESP desifruje ulozeny provisioning blob do RAM
ESP prejde na Wi-Fi
ESP stahuje firmware pres HTTPS
```

Hlavni pravidlo:

```text
PROVISION_CONFIG meni ulozenou konfiguraci.
OTA_CHECK pouze spousti kontrolu aktualizace podle uz ulozene konfigurace.
```

## Sériová komunikace

RS232/UART linka pro druhý ESP modul používá pevný binární paket s potvrzováním ACK, retry a požadavkem na okamžité poslání stavu. Přenos na tuto linku je ve výchozím stavu vypnutý. Zigbee entita `RS232 Enabled` byla odstraněna, linku lze zapnout jen při překladu (`-DUART_TRANSPORT_ENABLED=1`). Pro UART/RS-485 jsou rezervované GPIO16/GPIO17.

## Home Assistant OTA Deployment

Nasazení OTA serveru do Home Assistant OS, jeho runtime options, adresáře,
init/update skript a oddělení Zigbee2MQTT GUI vrstvy od provisioning/OTA
transportu jsou popsané v [docs/HA_OTA_DEPLOYMENT.md](docs/HA_OTA_DEPLOYMENT.md).
# Secure OTA Device Enrollment

The OTA enrollment/authentication algorithm is documented in
[`docs/DEVICE_ENROLLMENT_IMPLEMENTATION.md`](docs/DEVICE_ENROLLMENT_IMPLEMENTATION.md).

That section defines the device identity key, server-derived per-device HMAC
secret, challenge numbering, canonical byte layout, enrollment counter, registry
states, and provisioning gate. Legacy plaintext provisioning is development-only
and defaults to disabled.

## Testy

Přehled testů, k čemu jsou a kdy se mají spustit: [tests.README.md](tests.README.md).
