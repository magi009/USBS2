// USBS2 - Transferencia local por USB (MTP) para Switch 2 / Switch.
// Al abrirla, el PC ve la consola como dispositivo con 3 unidades:
//   Tarjeta SD (sdmc:/), Homebrew (sdmc:/switch) y Partidas guardadas.
// Mantén R al abrir para el MODO SEGURO (sin partidas guardadas).
#include <switch.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include "log.h"
#include "mtp.h"
#include "saves.h"

// Escribe el paso en pantalla y en sdmc:/switch/USBS2.log antes de ejecutarlo,
// así si la consola se cae sabemos exactamente dónde.
static void step(const char *s) {
    log_write("%s", s);
    printf("%s\n", s);
    consoleUpdate(nullptr);
}

int main(int, char **) {
    remove("sdmc:/USBS2.log");          // log viejo en la raíz (versiones anteriores)
    remove("sdmc:/switch/USBS2.log");
    consoleInit(nullptr);
    step("[1] consola iniciada");

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    // El control tarda unos fotogramas en reportar botones: se mira durante ~0,5 s.
    u64 held = 0;
    for (int i = 0; i < 10; i++) {
        padUpdate(&pad);
        held |= padGetButtons(&pad);
        svcSleepThread(50'000'000ULL);
    }
    bool safe = held & HidNpadButton_R;
    g_log_verbose = held & HidNpadButton_ZL;
    log_write("USBS2 1.4.2 | firmware %u.%u.%u | modo seguro: %s | registro detallado: %s",
              HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()), HOSVER_MICRO(hosversionGet()),
              safe ? "si" : "no", g_log_verbose ? "si" : "no");

    static std::atomic<bool> quit{false};
    if (!safe) {
        step("[3] servicios de partidas (ns, acc)");
        saves::init();
    } else {
        step("[3] MODO SEGURO: partidas desactivadas");
    }

    step("[4] iniciando USB (usb:ds)...");
    Result rc = mtp::start(&quit);
    log_write("    resultado USB: 0x%X", rc);
    step("[5] listo");
    consoleClear();

    char line[128];
    u64 bytes = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & (HidNpadButton_Plus | HidNpadButton_B)) break;
        if (down & HidNpadButton_Y) {
            g_log_verbose = !g_log_verbose;
            log_write("registro detallado: %s (boton Y)", g_log_verbose ? "ACTIVADO" : "desactivado");
        }

        printf("\x1b[1;1H");
        printf("USBS2 1.4.2 - Transferencia USB (MTP)%s%s\x1b[K\n", safe ? "  [MODO SEGURO]" : "", g_log_verbose ? "  [REGISTRO DETALLADO]" : "");
        printf("--------------------------------\x1b[K\n\n");
        if (R_FAILED(rc)) {
            printf("Error iniciando USB: 0x%X\x1b[K\n", rc);
            printf("Registro (manda foto de esta pantalla):\x1b[K\n");
            for (auto &l : log_tail(30)) printf("%.78s\x1b[K\n", l.c_str());
        } else {
            mtp::status(line, sizeof(line), &bytes);
            printf("Estado: %s\x1b[K\n\n", line);
            // velocidad media del último segundo
            static u64 last_bytes = 0, last_tick = 0;
            static double mbps = 0;
            u64 now = armTicksToNs(armGetSystemTick());
            if (now - last_tick >= 1'000'000'000ULL) {
                mbps = last_tick ? (bytes - last_bytes) / 1048576.0 / ((now - last_tick) / 1e9) : 0;
                last_bytes = bytes;
                last_tick = now;
            }
            printf("Conexion: %s\x1b[K\n", mtp::link_name());
            printf("Transferido: %.1f MB   Velocidad: %.1f MB/s\x1b[K\n\n", bytes / 1048576.0, mbps);
            printf("En el PC veras:\x1b[K\n");
            printf("  - Tarjeta SD           -> sdmc:/\x1b[K\n");
            printf("  - Homebrew             -> sdmc:/switch\x1b[K\n");
            printf("  - Partidas guardadas   -> una carpeta por juego/usuario\x1b[K\n\n");
            printf("Cierra los juegos antes de tocar sus partidas.\x1b[K\n");
        }
        printf("\nRegistro: sdmc:/switch/USBS2.log   Detallado: %s (Y = cambiar)\x1b[K\n", g_log_verbose ? "SI" : "no");
        printf("Pulsa + o B para salir.\x1b[K\n");
        consoleUpdate(nullptr);
        svcSleepThread(50'000'000ULL);
    }

    log_write("salida normal");
    quit = true;
    if (R_SUCCEEDED(rc)) mtp::stop();
    saves::exit();
    consoleExit(nullptr);
    return 0;
}
