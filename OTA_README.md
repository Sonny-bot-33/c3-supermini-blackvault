# OTA para ESP32-C3 SuperMini BlackVault

## Manifest esperado

El firmware consulta este archivo al arrancar:

- `https://raw.githubusercontent.com/Sonny-bot-33/c3-supermini-blackvault/main/manifest.json`

Formato:

```json
{
  "version": "0.1.0",
  "firmwareUrl": "https://github.com/Sonny-bot-33/c3-supermini-blackvault/releases/download/v0.1.0/firmware.bin"
}
```

## Flujo de publicación

1. Compilar `firmware.bin`
2. Subir `firmware.bin` a un release de GitHub
3. Actualizar `manifest.json` con la nueva versión y URL
4. Hacer commit/push de `manifest.json`

## Nota

Si la `version` remota coincide con `FW_VERSION`, el equipo sigue con BlackVault sin actualizar.
