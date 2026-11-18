// usbprobe 4 - Mapa de comandos de IDsInterface / IDsEndpoint en Switch 2.
// libounce registra la interfaz bien, pero luego pide sus eventos con los
// números de comando de la Switch 1 y uno falla con 0x4BB25. Aquí se prueba
// cada número de comando y se anota cuáles devuelven un evento (handle).
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

// Llama a un comando esperando un handle de vuelta (como Get*Event).
static Result get_handle(Service *s, u32 cmd, Handle *h) {
    *h = INVALID_HANDLE;
    serviceAssumeDomain(s);
    return serviceDispatch(s, cmd, .out_handle_attrs = { SfOutHandleAttr_HipcCopy }, .out_handles = h);
}

static void scan_handles(const char *what, Service *s, u32 from, u32 to, const u32 *skip, int nskip) {
    for (u32 cmd = from; cmd <= to; cmd++) {
        bool sk = false;
        for (int i = 0; i < nskip; i++) if (skip[i] == cmd) sk = true;
        if (sk) { out("  %s cmd %2u: (omitido)", what, cmd); continue; }
        out("  %s cmd %2u: probando...", what, cmd);
        Handle h;
        Result rc = get_handle(s, cmd, &h);
        bool got = R_SUCCEEDED(rc) && h != INVALID_HANDLE && h != 0;
        out("  %s cmd %2u: 0x%X%s", what, cmd, rc, got ? "  <-- DEVUELVE HANDLE" : "");
        if (got) svcCloseHandle(h);
    }
}

int main(int argc, char **argv) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    g_log = fopen("sdmc:/switch/usbprobe.log", "w");
    out("usbprobe 4 - comandos de IDsInterface/IDsEndpoint");

    Service root = {0}, ds = {0}, intf = {0}, ep = {0};
    Result rc = smGetService(&root, "usb:ds");
    out("usb:ds -> 0x%X", rc);
    if (R_FAILED(rc)) goto end;
    rc = serviceConvertToDomain(&root);
    serviceAssumeDomain(&root);
    if (R_SUCCEEDED(rc)) rc = serviceDispatch(&root, 0, .out_num_objects = 1, .out_objects = &ds);
    out("OpenDsService -> 0x%X", rc);
    if (R_FAILED(rc)) goto end;
    {
        const u32 cid = 2;
        serviceAssumeDomain(&ds);
        rc = serviceDispatchIn(&ds, 0, cid, .in_num_handles = 1, .in_handles = { CUR_PROCESS_HANDLE });
        out("Bind -> 0x%X", rc);
    }
    {
        const u8 n = 0;
        serviceAssumeDomain(&ds);
        rc = serviceDispatchIn(&ds, 1, n, .out_num_objects = 1, .out_objects = &intf);
        out("RegisterInterface(0) -> 0x%X", rc);
        if (R_FAILED(rc)) goto end;
    }

    // Antes de tocar nada, añadir configuración para que los comandos tengan sentido.
    {
        struct usb_interface_descriptor id = {0};
        id.bLength = USB_DT_INTERFACE_SIZE;
        id.bDescriptorType = USB_DT_INTERFACE;
        id.bNumEndpoints = 2;
        id.bInterfaceClass = 0xFF;
        const u32 speed = UsbDeviceSpeed_High;
        serviceAssumeDomain(&intf);
        rc = serviceDispatchIn(&intf, 10, speed,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
            .buffers = { { &id, USB_DT_INTERFACE_SIZE } });
        out("Interface Append (cmd 10) -> 0x%X", rc);
    }

    out("== IDsInterface: que comando da cada evento ==");
    {
        // 0 = RegisterEndpoint (necesita argumento), 3/4 = PostBuffer (necesitan buffer)
        const u32 skip[] = {0, 3, 4, 10};
        scan_handles("intf", &intf, 1, 14, skip, 4);
    }

    out("== IDsEndpoint ==");
    {
        const u8 addr = USB_ENDPOINT_IN + 1;
        serviceAssumeDomain(&intf);
        rc = serviceDispatchIn(&intf, 0, addr, .out_num_objects = 1, .out_objects = &ep);
        out("RegisterEndpoint(0x81) -> 0x%X", rc);
        if (R_SUCCEEDED(rc)) {
            const u32 skip[] = {0, 4, 5}; // 0 PostBuffer, 4 Stall, 5 SetZlt
            scan_handles("ep", &ep, 1, 8, skip, 3);
        }
    }

end:
    if (ep.session) { serviceAssumeDomain(&ep); serviceClose(&ep); }
    if (intf.session) { serviceAssumeDomain(&intf); serviceClose(&intf); }
    if (ds.session) { serviceAssumeDomain(&ds); serviceClose(&ds); }
    if (root.session) serviceClose(&root);
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
