# USBS2 – Transferencia local por USB (MTP) para Switch 2

Al abrir `USBS2.nro` arranca el modo MTP de inmediato, sin menús. Conecta el cable USB‑C al PC y en el Explorador de Windows (o en Android File Transfer/OpenMTP en Mac, o gvfs/KDE en Linux) aparece **"Switch 2 (USBS2)"** con 3 unidades:

| Unidad en el PC       | Qué es en la consola                           |
|-----------------------|------------------------------------------------|
| Tarjeta SD            | `sdmc:/` completo                              |
| Homebrew (sdmc-switch)| `sdmc:/switch`                                 |
| Partidas guardadas    | Una carpeta por juego y usuario, por ejemplo `Mario Kart World - Juan` |

Puedes copiar, pegar, borrar, renombrar, mover y crear carpetas. En las partidas, cada cambio se guarda (commit) en la partida apenas termina.

## Compilar sin instalar nada (GitHub)
1. Crea un repositorio nuevo en GitHub y sube esta carpeta tal cual (incluida `.github/`).
2. Ve a **Actions → Compilar USBS2.nro** y espera 1–2 minutos.
3. Descarga el artefacto **USBS2** y saca de ahí `USBS2.nro`.

## Compilar en tu PC
Con devkitPro (paquete `switch-dev`) instalado: `make`

## Instalar
Copia `USBS2.nro` a `sdmc:/switch/` y ábrela desde el Homebrew Launcher.
Para salir pulsa **+** o **B**.

## Avisos
- **Cierra el juego** antes de tocar sus partidas. Con el juego abierto la carpeta aparecerá vacía o dará error.
- **Haz copia de las partidas antes de modificarlas.**
- Solo verás partidas de juegos que tengan partida creada en la consola.
- **Switch 2:** depende de que tu exploit/hbloader permita usar los servicios `usb:ds` y de partidas. Si al abrir la app sale "Error iniciando USB: 0x…", anota el código, porque significa que el entorno actual no deja usar el USB.
- No hay ningún archivo de configuración. Para cambiar las carpetas que se muestran, edita `base_path()` en `source/mtp.cpp`.
