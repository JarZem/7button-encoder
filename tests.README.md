# Testy remotecontrol7andEncoder

(Projekt se přejmenuje na `ESP32_C6_RemoteEncoder`, až se uvolní složka.)

| Test | Soubor | Co dělá | Proč | Kdy spustit | Výsledek 2026-11-21 |
|---|---|---|---|---|---|
| Enrollment (vektory a výzvy) | `tools/test_device_enrollment.py` (6 testů) | Pevný vektor kanonické zprávy, odmítnutí změněného payloadu, různé tajemství pro různé identity, jednorázová výzva, expirace, normalizace ID | Zabezpečený OTA provisioning musí být deterministický a bezpečný | Po změně `tools/device_enrollment.py` nebo po aktualizaci OTA modulu | **6 z 6 OK** (spuštěno z `tools`) |
| Generátor vektorů ESP | `tools/test_esp_secure_target.py` | Není to test: vytváří vektory OTA -> ESP pro obejití serveru | Ladění provisioningu bez serveru | Ručně | Nespuštěno |
| OTA integrační testy | `tests/run_all.py` | Reálná HTTPS komunikace proti běžícímu OTA serveru, bez mockování, jen produkční API. Je určen ke spuštění uvnitř HA add-onu, kde je `/data/ota_token_secret.bin` | Ověření stahování firmwaru přes API | Po změně OTA serveru nebo tokenové logiky, uvnitř add-onu. Podrobnosti v `tests/README.md` | Nespuštěno (vyžaduje add-on) |

## Spuštění

```
cd tools
python -m unittest test_device_enrollment
```

Testy v `tools/` se pouští z adresáře `tools`.

Poznámka: `tests/run_all.py` odkazuje na `/share/esp_ota/firmware/remotecontrol7andEncoder.bin`. Po přejmenování
projektu se musí cesta a metadata `.release.json` změnit spolu s názvem firmwaru.
