// USBS2 - Transferencia local por USB (MTP) para Switch 2 / Switch.
// Al abrirla, el PC ve la consola como dispositivo con 3 unidades:
//   Tarjeta SD (sdmc:/), Homebrew (sdmc:/switch) y Partidas guardadas.
#include <switch.h>
#include <atomic>
#include <cstdio>
#include "mtp.h"
#include "saves.h"

int main(int, char **) {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    appletSetMediaPlaybackState(true); // evita que la pantalla se apague en copias largas

    static std::atomic<bool> quit{false};
    saves::init();
    Result rc = mtp::start(&quit);

    char line[128];
    u64 bytes = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & (HidNpadButton_Plus | HidNpadButton_B)) break;

        printf("\x1b[1;1H");
        printf("USBS2 - Transferencia USB (MTP)\x1b[K\n");
        printf("--------------------------------\x1b[K\n\n");
        if (R_FAILED(rc)) {
            printf("Error iniciando USB: 0x%X\x1b[K\n", rc);
            printf("Tu exploit/firmware puede no permitir usb:ds.\x1b[K\n");
        } else {
            mtp::status(line, sizeof(line), &bytes);
            printf("Estado: %s\x1b[K\n\n", line);
            printf("Transferido: %.1f MB\x1b[K\n\n", bytes / 1048576.0);
            printf("En el PC veras:\x1b[K\n");
            printf("  - Tarjeta SD           -> sdmc:/\x1b[K\n");
            printf("  - Homebrew             -> sdmc:/switch\x1b[K\n");
            printf("  - Partidas guardadas   -> una carpeta por juego/usuario\x1b[K\n\n");
            printf("Cierra los juegos antes de tocar sus partidas.\x1b[K\n");
        }
        printf("\nPulsa + o B para salir.\x1b[K\n");
        consoleUpdate(nullptr);
        svcSleepThread(50'000'000ULL);
    }

    quit = true;
    if (R_SUCCEEDED(rc)) mtp::stop();
    saves::exit();
    appletSetMediaPlaybackState(false);
    consoleExit(nullptr);
    return 0;
}
