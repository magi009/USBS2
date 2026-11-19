# USBS2 – Transferencia local por USB (MTP) para Switch 2

Al abrir `USBS2.nro` arranca el modo MTP de inmediato, sin menús. Conecta el cable USB‑C al PC y en el Explorador de Windows (o en Android File Transfer/OpenMTP en Mac, o gvfs/KDE en Linux) aparece **"Switch 2"** con 3 unidades:

| Unidad en el PC        | Qué es en la consola                                      |
|------------------------|-----------------------------------------------------------|
| Tarjeta SD             | `sdmc:/` completo                                          |
| Homebrew (sdmc-switch) | `sdmc:/switch`                                             |
| Partidas guardadas     | Una carpeta por juego y usuario, por ejemplo `Mario Kart World - Juan` |

Puedes copiar, pegar, borrar, renombrar, mover y crear carpetas. En las partidas, cada cambio se guarda (commit) en la partida apenas termina.

Probado en Switch 2 con firmware 23.0.1 y KaiHEN.

## Descargar
La última compilación está siempre en **Releases → latest** (`USBS2.nro`).

## Instalar y usar
1. Copia `USBS2.nro` a `sdmc:/switch/` y ábrela desde el Homebrew Launcher.
2. Conecta el cable USB‑C al PC.
3. Para salir pulsa **+** o **B**.

Botones al abrir la app (mantenlos mientras la abres):
- **R** – modo seguro: sin partidas guardadas.
- **Y** (con la app abierta) – activa o desactiva el registro `sdmc:/switch/USBS2.log`. Empieza desactivado: no se escribe nada hasta pulsar Y. Al activarlo se ven las últimas líneas en pantalla. Si el USB no arranca, el registro se activa solo.


El registro está en `sdmc:/switch/USBS2.log`. Si el USB no arranca, el error también sale en pantalla.

## Avisos
- **Cierra el juego** antes de tocar sus partidas. Con el juego abierto, la carpeta aparecerá vacía o dará error.
- **Haz copia de las partidas antes de modificarlas.**
- Solo verás partidas de juegos que tengan partida creada en la consola.

## Cómo se compila (Switch 2)
GitHub Actions (`.github/workflows/build.yml`) hace esto en la imagen `devkitpro/devkita64`:
1. Descarga [libounce](https://github.com/ouncebrew/libounce) en una versión fija.
2. Le aplica los parches de `patches/` (ver abajo).
3. Compila e instala libounce, y luego compila `USBS2.nro` y `usbprobe.nro`.

Para compilar en tu PC necesitas devkitPro con libounce compilado con el mismo parche, y luego `make`.

## Parche de USB para Switch 2 (`patches/0001-usbds-switch2.patch`)
El servicio `usb:ds` de la Switch 2 no es igual al de la Switch 1. libounce usaba los comandos de la Switch 1 y el registro de la interfaz fallaba con `0x4BB25` (2293‑0605). El parche solo se aplica al compilar para Switch 2 (`__SWITCH2__`):

| Llamada | Switch 1 (11.0+) | Switch 2 (23.0.1) |
|---|---|---|
| `IDsInterface` GetCtrlInCompletionEvent | cmd 5 | devuelve `0x4BB25` → no se pide |
| `IDsInterface` GetCtrlOutCompletionEvent | cmd 7 | cierra la sesión → no se pide |
| `IDsInterface` AppendConfigurationData | cmd 10 | **cmd 6** (misma entrada: u32 velocidad + buffer MapAlias) |

El resto de comandos que usa MTP funcionan igual: RegisterInterface, GetSetupEvent, RegisterEndpoint, los de endpoint, descriptores, BOS, Enable, GetState y GetSpeed. Con el parche, los eventos de control (`CtrlInCompletionEvent`/`CtrlOutCompletionEvent`) quedan sin inicializar, así que no se pueden usar transferencias de control propias; los endpoints bulk e interrupt funcionan.

`probe/` (`usbprobe.nro`) es la herramienta con la que se descubrió esto: prueba cada comando de `usb:ds` en una sesión nueva y anota el resultado en `sdmc:/switch/usbprobe.log`.

## Créditos
Fuente: [Inter](https://rsms.me/inter/) (SIL Open Font License), incluida como atlas en `data/font.bin` (se genera con `tools/make_font.py`).
