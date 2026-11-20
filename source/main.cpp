// USBS2 (MTP2) - Transferencia local por USB (MTP) para Switch 2.
// Al abrirla, el PC ve la consola como dispositivo con 3 unidades:
//   Tarjeta SD (sdmc:/), Homebrew (sdmc:/switch) y Partidas guardadas.
// Mantén R al abrir para el MODO SEGURO (sin partidas guardadas).
// Y activa/desactiva el registro (desactivado al empezar).
#include <switch.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include "gfx.h"
#include "log.h"
#include "mtp.h"
#include "usb.h"
#include "saves.h"

#define APP_VER "1.9.1"

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

// ---------------- colores ----------------
static constexpr u32 C_WHITE = 0xFFFFFF;
static constexpr u32 C_TEXT2 = 0xB8CCF0;    // texto secundario
static constexpr u32 C_TEXT3 = 0x7F99C9;    // texto tenue
static constexpr u32 C_CARD = 0x0F2F6E;     // tarjetas
static constexpr u32 C_CARD_EDGE = 0x2A56A8;
static constexpr u32 C_TRACK = 0x091D47;    // fondo de la barra
static constexpr u32 C_YEL1 = 0xFFD84A;     // barra (arriba)
static constexpr u32 C_YEL2 = 0xF2A900;     // barra (abajo)
static constexpr u32 C_GREEN = 0x3DDC84;
static constexpr u32 C_RED = 0xFF6B6B;

static void card(int x, int y, int w, int h) {
    gfx::round_rect(x - 1, y - 1, w + 2, h + 2, 18, C_CARD_EDGE);
    gfx::round_rect(x, y, w, h, 17, C_CARD);
}

static void button_hint(int &x, int y, const char *btn, const char *label) {
    int bw = gfx::text_w(gfx::Small, btn) + 18;
    gfx::round_rect(x, y, bw < 30 ? 30 : bw, 30, 15, C_WHITE, 235);
    int tw = gfx::text_w(gfx::Small, btn);
    gfx::text(gfx::Small, x + ((bw < 30 ? 30 : bw) - tw) / 2, y + 4, 0x0D2A63, btn);
    x += (bw < 30 ? 30 : bw) + 10;
    x += gfx::text(gfx::Small, x, y + 4, C_TEXT2, label) + 34;
}

static void fmt_size(char *out, size_t n, u64 b) {
    if (b >= 1024ULL * 1024 * 1024) snprintf(out, n, "%.2f GB", b / 1073741824.0);
    else if (b >= 1024 * 1024) snprintf(out, n, "%.1f MB", b / 1048576.0);
    else snprintf(out, n, "%.0f KB", b / 1024.0);
}

static void fmt_time(char *out, size_t n, double s) {
    if (s < 1) snprintf(out, n, "menos de 1 s");
    else if (s < 60) snprintf(out, n, "%.0f s", s);
    else if (s < 3600) snprintf(out, n, "%d min %02d s", (int)s / 60, (int)s % 60);
    else snprintf(out, n, "%d h %02d min", (int)s / 3600, ((int)s % 3600) / 60);
}

int main(int, char **) {
    remove("sdmc:/USBS2.log");   // log viejo en la raíz (versiones antiguas)
    log_write("[1] inicio");
    bool gfx_ok = gfx::init();
    if (!gfx_ok) {
        // Sin gráficos: al menos que quede el registro para saber por qué.
        log_enable(true);
        log_write("    no se pudo iniciar la pantalla grafica");
    }

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
    log_write("USBS2 " APP_VER " | firmware %u.%u.%u | modo seguro: %s",
              HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()), HOSVER_MICRO(hosversionGet()), safe ? "si" : "no");

    if (gfx_ok) {
        gfx::begin();
        gfx::text(gfx::Title, 60, 36, C_WHITE, "MTP2");
        gfx::text(gfx::Body, 60, 120, C_TEXT2, "Iniciando…");
        gfx::end();
    }

    static std::atomic<bool> quit{false};
    if (!safe) {
        log_write("[3] servicios de partidas (ns, acc)");
        saves::init();
    } else {
        log_write("[3] MODO SEGURO: partidas desactivadas");
    }
    int usb3 = read_usb3();
    if (usb3 >= 0) log_write("    ajuste usb!usb30_force_enabled = %d", usb3);
    log_write("[4] iniciando USB (usb:ds)...");
    Result rc = mtp::start(&quit);
    log_write("    resultado USB: 0x%X", rc);
    if (R_FAILED(rc) && !g_log_verbose) log_enable(true);   // error: guardar el registro

    char line[160], a[48], b[48], c[48];
    u64 bytes = 0, last_bytes = 0, last_tick = 0;
    double mbps = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & (HidNpadButton_Plus | HidNpadButton_B)) break;
        if (down & HidNpadButton_Y) {
            log_enable(!g_log_verbose);
            log_write("registro: %s (boton Y)", g_log_verbose ? "ACTIVADO" : "desactivado");
        }

        if (R_SUCCEEDED(rc)) mtp::status(line, sizeof(line), &bytes);
        u64 now = armTicksToNs(armGetSystemTick());
        if (now - last_tick >= 1'000'000'000ULL) {
            mbps = last_tick ? (bytes - last_bytes) / 1048576.0 / ((now - last_tick) / 1e9) : 0;
            last_bytes = bytes;
            last_tick = now;
        }

        if (!gfx_ok) { svcSleepThread(50'000'000ULL); continue; }
        gfx::begin();

        // ---- cabecera ----
        gfx::text(gfx::Title, 60, 28, C_WHITE, "MTP2");
        int tx = 60 + gfx::text_w(gfx::Title, "MTP2") + 22;
        gfx::text(gfx::Body, tx, 44, C_TEXT2, "Transferencia USB para Switch 2");
        gfx::text(gfx::Small, tx, 74, C_TEXT3, "USBS2 " APP_VER);
        if (safe) {
            int w = gfx::text_w(gfx::Small, "MODO SEGURO") + 24;
            gfx::round_rect(1220 - w, 46, w, 30, 15, 0xF2A900);
            gfx::text(gfx::Small, 1220 - w + 12, 50, 0x1A1200, "MODO SEGURO");
        }
        gfx::rect(60, 112, 1160, 2, C_CARD_EDGE, 160);

        if (R_FAILED(rc)) {
            card(60, 136, 1160, 520);
            snprintf(line, sizeof(line), "No se pudo iniciar el USB (error 0x%X)", rc);
            gfx::text(gfx::Big, 90, 160, C_RED, line);
            gfx::text(gfx::Small, 90, 206, C_TEXT2, "Registro guardado en sdmc:/switch/USBS2.log");
            int y = 244;
            for (auto &l : log_tail(16)) { gfx::text(gfx::Small, 90, y, C_TEXT2, l.c_str(), 1100); y += 24; }
        } else {
            // ---- estado ----
            bool connected = strncmp(line, "Esperando", 9) != 0;
            card(60, 136, 1160, 150);
            gfx::round_rect(90, 166, 14, 14, 7, connected ? C_GREEN : 0xF2A900);
            gfx::text(gfx::Small, 116, 160, C_TEXT3, connected ? "CONECTADO" : "ESPERANDO");
            gfx::text(gfx::Big, 90, 188, C_WHITE, connected ? (strncmp(line, "Cable", 5) == 0 ? "Cable conectado, esperando al PC" : "Conectado al PC") : "Conecta el cable USB al PC", 760);
            snprintf(a, sizeof(a), "%s", mtp::link_name());
            gfx::text(gfx::Body, 90, 238, C_TEXT2, connected ? a : "La consola aparecerá como \"Switch 2 (USBS2)\"");
            fmt_size(b, sizeof(b), bytes);
            mtp::Progress px;
            mtp::progress(&px);
            snprintf(c, sizeof(c), "%u archivos", px.files);
            gfx::text(gfx::Small, 900, 172, C_TEXT3, "TRANSFERIDO");
            gfx::text(gfx::Big, 900, 194, C_WHITE, b);
            gfx::text(gfx::Small, 900, 240, C_TEXT2, c);

            // ---- progreso ----
            card(60, 306, 1160, 220);
            if (px.dir) {
                double frac = px.total ? (double)px.done / px.total : 1.0;
                if (frac > 1) frac = 1;
                const char *dir = px.dir == 1 ? "PC  →  Consola" : "Consola  →  PC";
                int dw = gfx::text_w(gfx::Small, dir) + 26;
                gfx::round_rect(90, 330, dw, 30, 15, 0x1E4C9E);
                gfx::text(gfx::Small, 103, 334, C_YEL1, dir);
                snprintf(a, sizeof(a), "%d%%", (int)(frac * 100));
                int pw = gfx::text_w(gfx::Title, a);
                gfx::text(gfx::Title, 1190 - pw, 318, C_WHITE, a);
                gfx::text(gfx::Big, 90, 372, C_WHITE, px.name, 1190 - pw - 120);
                // barra amarilla
                const int bx = 90, by = 430, bw = 1100, bh = 26;
                gfx::round_rect(bx, by, bw, bh, 13, C_TRACK);
                int fw = (int)(bw * frac);
                if (fw > 0) {
                    if (fw < bh) fw = bh;
                    gfx::round_rect_v(bx, by, fw, bh, 13, C_YEL1, C_YEL2);
                    gfx::round_rect(bx + 8, by + 4, fw - 16 > 0 ? fw - 16 : 0, 5, 3, 0xFFFFFF, 70);  // brillo
                }
                fmt_size(a, sizeof(a), px.done);
                fmt_size(b, sizeof(b), px.total);
                snprintf(line, sizeof(line), "%s de %s", a, b);
                gfx::text(gfx::Body, 90, 474, C_TEXT2, line);
                double left_s = mbps > 0.1 ? (px.total - px.done) / 1048576.0 / mbps : 0;
                fmt_time(c, sizeof(c), left_s);
                snprintf(line, sizeof(line), "%.1f MB/s   ·   quedan %s", mbps, mbps > 0.1 ? c : "…");
                int lw = gfx::text_w(gfx::Body, line);
                gfx::text(gfx::Body, 1190 - lw, 474, C_TEXT2, line);
            } else if (mtp::get_notice(line, sizeof(line))) {
                gfx::text(gfx::Small, 90, 334, C_RED, "AVISO");
                gfx::text(gfx::Big, 90, 362, C_WHITE, line, 1100);
                gfx::text(gfx::Body, 90, 410, C_TEXT2, "Activa el registro con Y para ver los detalles.");
                gfx::round_rect(90, 450, 1100, 26, 13, C_TRACK);
            } else {
                gfx::text(gfx::Big, 90, 360, C_WHITE, "Sin copias en curso");
                gfx::text(gfx::Body, 90, 404, C_TEXT2, "Copia o arrastra archivos desde el Explorador de Windows.");
                gfx::round_rect(90, 450, 1100, 26, 13, C_TRACK);
            }

            // ---- registro o ayuda ----
            if (g_log_verbose) {
                card(60, 546, 1160, 118);
                gfx::text(gfx::Small, 90, 556, C_YEL1, "REGISTRO ACTIVADO  ·  sdmc:/switch/USBS2.log");
                snprintf(a, sizeof(a), "USB: %s", usb::where());
                int aw = gfx::text_w(gfx::Small, a);
                gfx::text(gfx::Small, 1190 - aw, 556, C_TEXT3, a);
                int y = 582;
                for (auto &l : log_tail(3)) { gfx::text(gfx::Small, 90, y, C_TEXT2, l.c_str(), 1100); y += 25; }
            } else {
                gfx::text(gfx::Small, 60, 552, C_TEXT3, "En el PC verás:  Tarjeta SD  ·  Homebrew (sdmc:/switch)  ·  Partidas guardadas");
                gfx::text(gfx::Small, 60, 580, C_TEXT3, "Cierra el juego antes de tocar sus partidas.");
            }
        }

        // ---- pie ----
        gfx::rect(0, 672, 1280, 48, 0x051433, 200);
        int fx = 60;
        button_hint(fx, 681, "+", "Salir");
        button_hint(fx, 681, "Y", g_log_verbose ? "Registro: activado" : "Registro: desactivado");

        gfx::end();
        svcSleepThread(30'000'000ULL);   // ~20 fotogramas/s: deja la CPU al USB
    }

    log_write("salida normal");
    quit = true;
    bool clean = R_FAILED(rc) || mtp::stop();
    if (clean) saves::exit();
    log_write("salida %s", clean ? "limpia" : "forzada");
    gfx::exit();
    return 0;
}
