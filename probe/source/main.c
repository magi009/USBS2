// usbprobe 6 - Busca AppendConfigurationData. (antes: usbprobe 5 - Mapa de comandos de IDsInterface en Switch 2, con una conexión
// NUEVA para cada prueba (en la sonda 4 la primera llamada cerró la sesión y
// contaminó todo lo demás con 0xF601).
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
    printf("%.79s\n", line);
    consoleUpdate(NULL);
    if (g_log) { fprintf(g_log, "%s\n", line); fflush(g_log); }
}

typedef struct { Service root, ds, intf; } Ctx;

// Abre usb:ds, Bind(2) y RegisterInterface(0) en una sesión nueva.
static Result fresh(Ctx *c) {
    memset(c, 0, sizeof(*c));
    Result rc = smGetService(&c->root, "usb:ds");
    if (R_FAILED(rc)) return rc;
    rc = serviceConvertToDomain(&c->root);
    if (R_FAILED(rc)) return rc;
    serviceAssumeDomain(&c->root);
    rc = serviceDispatch(&c->root, 0, .out_num_objects = 1, .out_objects = &c->ds);
    if (R_FAILED(rc)) return rc;
    const u32 cid = 2;
    serviceAssumeDomain(&c->ds);
    rc = serviceDispatchIn(&c->ds, 0, cid, .in_num_handles = 1, .in_handles = { CUR_PROCESS_HANDLE });
    if (R_FAILED(rc)) return rc;
    const u8 n = 0;
    serviceAssumeDomain(&c->ds);
    return serviceDispatchIn(&c->ds, 1, n, .out_num_objects = 1, .out_objects = &c->intf);
}

static void done(Ctx *c) {
    if (c->intf.session) { serviceAssumeDomain(&c->intf); serviceClose(&c->intf); }
    if (c->ds.session) { serviceAssumeDomain(&c->ds); serviceClose(&c->ds); }
    if (c->root.session) serviceClose(&c->root);
}

static const char *tag(Result rc) {
    if (rc == 0) return "OK";
    if (rc == 0xF601) return "SESION CERRADA";
    if (rc == 0x10601 || rc == 0x1A80 || rc == 0x11A0B) return "cmd desconocido?";
    return "";
}

// Prueba un comando "Get*Event" (sin entrada, devuelve un handle).
static void test_event(u32 cmd) {
    Ctx c;
    Result rc = fresh(&c);
    if (R_FAILED(rc)) { out("  ev cmd %2u: preparar -> 0x%X", cmd, rc); done(&c); return; }
    Handle h = INVALID_HANDLE;
    serviceAssumeDomain(&c.intf);
    rc = serviceDispatch(&c.intf, cmd, .out_handle_attrs = { SfOutHandleAttr_HipcCopy }, .out_handles = &h);
    bool got = R_SUCCEEDED(rc) && h != INVALID_HANDLE && h != 0;
    out("  ev cmd %2u: 0x%X %s%s", cmd, rc, tag(rc), got ? "  <-- DEVUELVE HANDLE" : "");
    if (got) svcCloseHandle(h);
    // ¿la sesión sigue viva después?
    Handle h2 = INVALID_HANDLE;
    serviceAssumeDomain(&c.ds);
    Result alive = serviceDispatch(&c.ds, 2, .out_handle_attrs = { SfOutHandleAttr_HipcCopy }, .out_handles = &h2);
    if (R_SUCCEEDED(alive) && h2 != INVALID_HANDLE) svcCloseHandle(h2);
    if (alive == 0xF601) out("             (despues de esto la sesion quedo cerrada)");
    done(&c);
}

static const u8 g_intf_desc[USB_DT_INTERFACE_SIZE] = {9, 4, 0, 0, 2, 0xFF, 0xFF, 0xFF, 0};

// Prueba "AppendConfigurationData" con un número de comando y un tipo de buffer.
static void test_append(u32 cmd, u32 attr, const char *attr_name, bool with_intf_num) {
    Ctx c;
    Result rc = fresh(&c);
    if (R_FAILED(rc)) { out("  append cmd %u %s: preparar -> 0x%X", cmd, attr_name, rc); done(&c); return; }
    serviceAssumeDomain(&c.intf);
    if (with_intf_num) {
        const struct { u8 intf; u8 pad[3]; u32 speed; } in = { 0, {0}, UsbDeviceSpeed_High };
        rc = serviceDispatchIn(&c.intf, cmd, in, .buffer_attrs = { attr }, .buffers = { { g_intf_desc, sizeof(g_intf_desc) } });
    } else {
        const u32 speed = UsbDeviceSpeed_High;
        rc = serviceDispatchIn(&c.intf, cmd, speed, .buffer_attrs = { attr }, .buffers = { { g_intf_desc, sizeof(g_intf_desc) } });
    }
    out("  append cmd %2u %-10s %s: 0x%X %s", cmd, attr_name, with_intf_num ? "(n,vel)" : "(vel)  ", rc, tag(rc));
    done(&c);
}

static void test_register_endpoint(u32 cmd) {
    Ctx c;
    Result rc = fresh(&c);
    if (R_FAILED(rc)) { out("  ep cmd %u: preparar -> 0x%X", cmd, rc); done(&c); return; }
    Service ep = {0};
    const u8 addr = USB_ENDPOINT_IN + 1;
    serviceAssumeDomain(&c.intf);
    rc = serviceDispatchIn(&c.intf, cmd, addr, .out_num_objects = 1, .out_objects = &ep);
    out("  RegisterEndpoint cmd %2u (0x81): 0x%X %s", cmd, rc, tag(rc));
    if (R_SUCCEEDED(rc)) {
        Handle h = INVALID_HANDLE;
        serviceAssumeDomain(&ep);
        Result r2 = serviceDispatch(&ep, 2, .out_handle_attrs = { SfOutHandleAttr_HipcCopy }, .out_handles = &h);
        out("    endpoint GetCompletionEvent (cmd 2): 0x%X %s", r2, tag(r2));
        if (R_SUCCEEDED(r2) && h != INVALID_HANDLE) svcCloseHandle(h);
        serviceAssumeDomain(&ep);
        serviceClose(&ep);
    }
    done(&c);
}

int main(int argc, char **argv) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    g_log = fopen("sdmc:/switch/usbprobe.log", "w");
    out("usbprobe 6 - buscar AppendConfigurationData");

    out("== Control: comando inexistente (100) ==");
    test_event(100);

    out("== Append (u32 velocidad + buffer MapAlias) cmd 2..30 ==");
    for (u32 cmd = 2; cmd <= 30; cmd++)
        test_append(cmd, SfBufferAttr_HipcMapAlias | SfBufferAttr_In, "MapAlias", false);

    out("== Append (u8 interfaz, u32 velocidad + MapAlias) cmd 2..30 ==");
    for (u32 cmd = 2; cmd <= 30; cmd++)
        test_append(cmd, SfBufferAttr_HipcMapAlias | SfBufferAttr_In, "MapAlias", true);

    out("");
    out("Listo. Log: sdmc:/switch/usbprobe.log");
    out("Pulsa + para salir.");
    if (g_log) { fclose(g_log); g_log = NULL; }

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
        svcSleepThread(50000000ULL);
    }
    consoleExit(NULL);
    return 0;
}
