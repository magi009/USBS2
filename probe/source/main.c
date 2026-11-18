// usbprobe - Sonda del servicio usb:ds en Switch 2 (KaiHEN / libounce).
// Prueba combinaciones de "complex id", números de interfaz y comandos para
// descubrir cómo hay que registrar una interfaz USB-dispositivo en Switch 2.
// Resultado en pantalla y en sdmc:/switch/usbprobe.log
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

static FILE *g_log;

static void out(const char *fmt, ...) {
    char line[256];
    va_list va;
    va_start(va, fmt);
    vsnprintf(line, sizeof(line), fmt, va);
    va_end(va);
    printf("%s\n", line);
    consoleUpdate(NULL);
    if (g_log) { fprintf(g_log, "%s\n", line); fflush(g_log); }
}

static const char *fmt_rc(Result rc) {
    static char b[4][40];
    static int i;
    i = (i + 1) & 3;
    if (rc == 0) snprintf(b[i], sizeof(b[i]), "OK");
    else snprintf(b[i], sizeof(b[i]), "0x%X (%04u-%04u)", rc, 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
    return b[i];
}

typedef struct { Service root, ds; bool open; } Ds;

static Result ds_open(Ds *d) {
    memset(d, 0, sizeof(*d));
    Result rc = smGetService(&d->root, "usb:ds");
    if (R_FAILED(rc)) return rc;
    rc = serviceConvertToDomain(&d->root);
    if (R_FAILED(rc)) { serviceClose(&d->root); return rc; }
    serviceAssumeDomain(&d->root);
    rc = serviceDispatch(&d->root, 0, .out_num_objects = 1, .out_objects = &d->ds);
    if (R_FAILED(rc)) { serviceClose(&d->root); return rc; }
    d->open = true;
    return 0;
}

static void ds_close(Ds *d) {
    if (!d->open) return;
    serviceAssumeDomain(&d->ds);
    serviceClose(&d->ds);
    serviceClose(&d->root);
    d->open = false;
}

static Result ds_bind(Ds *d, u32 complex_id) {
    serviceAssumeDomain(&d->ds);
    return serviceDispatchIn(&d->ds, 0, complex_id,
        .in_num_handles = 1,
        .in_handles = { CUR_PROCESS_HANDLE },
    );
}

static Result ds_register_u8(Ds *d, u8 intf, Service *out_intf) {
    serviceAssumeDomain(&d->ds);
    return serviceDispatchIn(&d->ds, 1, intf, .out_num_objects = 1, .out_objects = out_intf);
}

static Result ds_register_u32(Ds *d, u32 intf, Service *out_intf) {
    serviceAssumeDomain(&d->ds);
    return serviceDispatchIn(&d->ds, 1, intf, .out_num_objects = 1, .out_objects = out_intf);
}

static void close_intf(Service *s) {
    serviceAssumeDomain(s);
    serviceClose(s);
}

int main(int argc, char **argv) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    g_log = fopen("sdmc:/switch/usbprobe.log", "w");
    out("usbprobe 1 - sonda usb:ds");
    out("firmware (hosversion): %u.%u.%u", HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()), HOSVER_MICRO(hosversionGet()));

    // --- Prueba A: complex id 0..7, registrar interfaces 0..7 ---
    out("");
    out("== A: complex id x interfaz ==");
    for (u32 cid = 0; cid < 8; cid++) {
        Ds d;
        Result rc = ds_open(&d);
        if (R_FAILED(rc)) { out("cid %u: abrir usb:ds -> %s", cid, fmt_rc(rc)); continue; }
        rc = ds_bind(&d, cid);
        if (R_FAILED(rc)) { out("cid %u: bind -> %s", cid, fmt_rc(rc)); ds_close(&d); continue; }

        char line[200];
        int n = snprintf(line, sizeof(line), "cid %u: bind OK | reg:", cid);
        for (u8 i = 0; i < 8; i++) {
            Service s;
            Result r = ds_register_u8(&d, i, &s);
            if (R_SUCCEEDED(r)) { n += snprintf(line + n, sizeof(line) - n, " %u=OK", i); close_intf(&s); }
            else n += snprintf(line + n, sizeof(line) - n, " %u=%X", i, r);
        }
        out("%s", line);
        ds_close(&d);
    }

    // --- Prueba B: cid 2, interfaces 8..31 y entrada u32 ---
    out("");
    out("== B: cid 2, interfaces 8..31 ==");
    {
        Ds d;
        Result rc = ds_open(&d);
        if (R_SUCCEEDED(rc)) rc = ds_bind(&d, 2);
        if (R_FAILED(rc)) out("abrir/bind -> %s", fmt_rc(rc));
        else {
            char line[300];
            int n = 0;
            for (u8 i = 8; i < 32; i++) {
                Service s;
                Result r = ds_register_u8(&d, i, &s);
                if (R_SUCCEEDED(r)) { n += snprintf(line + n, sizeof(line) - n, " %u=OK", i); close_intf(&s); }
                else n += snprintf(line + n, sizeof(line) - n, " %u=%X", i, r);
                if (n > 200) { out("%s", line); n = 0; }
            }
            if (n) out("%s", line);
            Service s;
            Result r = ds_register_u32(&d, 0, &s);
            out("reg(u32 0) -> %s", fmt_rc(r));
            if (R_SUCCEEDED(r)) close_intf(&s);
        }
        ds_close(&d);
    }

    // --- Prueba C: qué comandos existen en IDsService (sin argumentos) ---
    out("");
    out("== C: comandos IDsService (sin args) ==");
    {
        Ds d;
        Result rc = ds_open(&d);
        if (R_SUCCEEDED(rc)) rc = ds_bind(&d, 2);
        if (R_FAILED(rc)) out("abrir/bind -> %s", fmt_rc(rc));
        else {
            char line[300];
            int n = 0;
            for (u32 cmd = 0; cmd < 24; cmd++) {
                if (cmd == 0 || cmd == 9) continue; // 0 = Bind, 9 = Enable: no se tocan
                serviceAssumeDomain(&d.ds);
                Result r = serviceDispatch(&d.ds, cmd);
                n += snprintf(line + n, sizeof(line) - n, " c%u=%X", cmd, r);
                if (n > 200) { out("%s", line); n = 0; }
            }
            if (n) out("%s", line);
        }
        ds_close(&d);
    }

    // --- Prueba D: comandos de la sesion raiz (IDsRootSession) ---
    out("");
    out("== D: comandos IDsRootSession ==");
    {
        Service root;
        Result rc = smGetService(&root, "usb:ds");
        if (R_SUCCEEDED(rc)) rc = serviceConvertToDomain(&root);
        if (R_FAILED(rc)) out("abrir -> %s", fmt_rc(rc));
        else {
            char line[200];
            int n = 0;
            for (u32 cmd = 1; cmd < 8; cmd++) {
                Service o;
                serviceAssumeDomain(&root);
                Result r = serviceDispatch(&root, cmd, .out_num_objects = 1, .out_objects = &o);
                if (R_SUCCEEDED(r)) { serviceAssumeDomain(&o); serviceClose(&o); }
                n += snprintf(line + n, sizeof(line) - n, " r%u=%X", cmd, r);
            }
            out("%s", line);
            serviceClose(&root);
        }
    }

    out("");
    out("Listo. Log: sdmc:/switch/usbprobe.log");
    out("Pulsa + para salir.");
    if (g_log) fclose(g_log);

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
        svcSleepThread(50000000ULL);
    }
    consoleExit(NULL);
    return 0;
}
