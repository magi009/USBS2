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
#include "usb.h"
#include "saves.h"

// Escribe el paso en pantalla y en sdmc:/switch/USBS2.log antes de ejecutarlo,
// así si la consola se cae sabemos exactamente dónde.
static void step(const char *s) {
    log_write("%s", s);
    printf("%s\n", s);
    consoleUpdate(nullptr);
}

// Lee usb!usb30_force_enabled: 0/1, o -1 si no se puede.
static int read_usb3() {
    int out = -1;
    if (R_SUCCEEDED(setsysInitialize())) {
        u8 v = 0; u64 sz = 0;
        Result r = setsysGetSettingsItemValue("usb", "usb30_force_enabled", &v, sizeof(v), &sz);
        if (R_SUCCEEDED(r)) out = v ? 1 : 0;
        else log_write("    ajuste usb!usb30_force_enabled: no se puede leer (0x%X)", r);
        setsysExit();
    }
    return out;
}

int main(int, char **) {
    remove("sdmc:/USBS2.log");          // log viejo en la raíz (versiones anteriores)
    // Se conserva el log de la sesión anterior (por si la app se cerró o se cayó).
    remove("sdmc:/switch/USBS2-anterior.log");
    rename("sdmc:/switch/USBS2.log", "sdmc:/switch/USBS2-anterior.log");
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
    log_write("USBS2 1.8.0 | firmware %u.%u.%u | modo seguro: %s | registro detallado: %s",
              HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()), HOSVER_MICRO(hosversionGet()),
              safe ? "si" : "no", g_log_verbose ? "si" : "no");

    static std::atomic<bool> quit{false};
    if (!safe) {
        step("[3] servicios de partidas (ns, acc)");
        saves::init();
    } else {
        step("[3] MODO SEGURO: partidas desactivadas");
    }

    // Ajuste que deja usar USB 3 a las apps caseras (usb!usb30_force_enabled).
    int usb3 = read_usb3();
    if (usb3 >= 0) log_write("    ajuste usb!usb30_force_enabled = %d", usb3);
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
        printf("USBS2 1.8.0 - Transferencia USB (MTP)%s%s\x1b[K\n", safe ? "  [MODO SEGURO]" : "", g_log_verbose ? "  [REGISTRO DETALLADO]" : "");
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
            printf("Conexion: %s   Peticiones de control: %u\x1b[K\n", mtp::link_name(), usb::setup_count());
            if (g_log_verbose) printf("USB ahora: %s / control: %s\x1b[K\n", usb::where(), usb::where_ctrl());
            printf("Transferido: %.1f MB   Velocidad: %.1f MB/s\x1b[K\n\n", bytes / 1048576.0, mbps);
            // Barra de progreso del archivo en curso
            mtp::Progress px;
            mtp::progress(&px);
            if (px.dir) {
                double frac = px.total ? (double)px.done / px.total : 1.0;
                if (frac > 1) frac = 1;
                const int W = 50;
                int fill = (int)(frac * W + 0.5);
                char bar[W + 1];
                for (int i = 0; i < W; i++) bar[i] = i < fill ? '#' : '-';
                bar[W] = 0;
                printf("%s: %.60s\x1b[K\n", px.dir == 1 ? "PC -> consola" : "Consola -> PC", px.name);
                printf("[\x1b[32m%.*s\x1b[0m%s] %3d%%\x1b[K\n", fill, bar, bar + fill, (int)(frac * 100));
                double left_s = mbps > 0.1 ? (px.total - px.done) / 1048576.0 / mbps : 0;
                printf("%.1f de %.1f MB   quedan %.0f s   Archivos copiados: %u\x1b[K\n\n",
                       px.done / 1048576.0, px.total / 1048576.0, left_s, px.files);
            } else {
                printf("Sin copias en curso.   Archivos copiados: %u\x1b[K\n\x1b[K\n\x1b[K\n\n", px.files);
            }
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
    bool clean = R_FAILED(rc) || mtp::stop();
    if (clean) saves::exit();
    log_write("salida %s", clean ? "limpia" : "forzada");
    consoleExit(nullptr);
    return 0;
}
