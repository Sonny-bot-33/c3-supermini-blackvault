# c3-supermini-blackvault

Firmware para ESP32-C3 SuperMini con:

- BlackVault como función principal
- intento de conexión a dos redes Wi‑Fi conocidas
- comprobación automática de actualización OTA al arrancar
- actualización desde GitHub Releases si hay versión nueva
- fallback automático a BlackVault si no hay update
- buzzer de arranque, entrada a BlackVault e inactividad

## OTA

Al arrancar, el firmware consulta:

- `https://raw.githubusercontent.com/Sonny-bot-33/c3-supermini-blackvault/main/manifest.json`

Si `version` es distinta de la del firmware actual, descarga el `firmware.bin` indicado en `firmwareUrl` y actualiza.

## Publicar nueva versión

### Opción rápida

```bash
~/.platformio/penv/bin/pio run -e esp32-c3-devkitm-1
./publish_ota.sh 0.1.2
```

### Qué hace el script

1. toma `.pio/build/esp32-c3-devkitm-1/firmware.bin`
2. copia `firmware.bin` al root del repo
3. actualiza `manifest.json`
4. hace commit y push
5. crea o actualiza el release `vX.Y.Z`
