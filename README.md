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

1. Compilar `firmware.bin`
2. Crear release `vX.Y.Z` en GitHub
3. Adjuntar `firmware.bin`
4. Actualizar `manifest.json`
5. Hacer push del manifest
