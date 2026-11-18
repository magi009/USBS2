// usbprobe 2 - Busca qué paso de configuración de usb:ds impide registrar una
// interfaz en Switch 2, y prueba la secuencia completa "registrar primero".
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

#define STEP(name, call) do { Result _r = (call); out("  %-28s -> 0x%X", name, _r); if (R_FAILED(_r)) goto done; } while (0)

static const u16 g_langs[1] = {0x0409};
static const u8 g_bos[0x16] = {
    0x05, 0x0F, 0x16, 0x00, 0x02,
    0x07, 0x10, 0x02, 0x02, 0x00, 0x00, 0x00,
    0x0A, 0x10, 0x03, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00,
};

static void dev_desc(struct usb_device_descriptor *d, u16 bcd, u8 mps0) {
    memset(d, 0, sizeof(*d));
    d->bLength = USB_DT_DEVICE_SIZE;
    d->bDescriptorType = USB_DT_DEVICE;
    d->bcdUSB = bcd;
    d->bMaxPacketSize0 = mps0;
    d->idVendor = 0x057E;
    d->idProduct = 0x201D;
    d->bcdDevice = 0x0100;
    d->bNumConfigurations = 1;
}


// ---- IPC directo (sin usbDsInitialize de libounce) ----
typedef struct { Service root, ds; } Raw;

static Result raw_open_bind(Raw *r) {
    memset(r, 0, sizeof(*r));
    Result rc = smGetService(&r->root, "usb:ds");
    if (R_FAILED(rc)) return rc;
    rc = serviceConvertToDomain(&r->root);
    if (R_SUCCEEDED(rc)) {
        serviceAssumeDomain(&r->root);
        rc = serviceDispatch(&r->root, 0, .out_num_objects = 1, .out_objects = &r->ds);
    }
    if (R_FAILED(rc)) { serviceClose(&r->root); return rc; }
    const u32 cid = 2;
    serviceAssumeDomain(&r->ds);
    return serviceDispatchIn(&r->ds, 0, cid, .in_num_handles = 1, .in_handles = { CUR_PROCESS_HANDLE });
}

static void raw_close(Raw *r) {
    serviceAssumeDomain(&r->ds);
    serviceClose(&r->ds);
    serviceClose(&r->root);
}

static Result raw_state_event(Raw *r) {
    Handle h = INVALID_HANDLE;
    serviceAssumeDomain(&r->ds);
    Result rc = serviceDispatch(&r->ds, 2, .out_handle_attrs = { SfOutHandleAttr_HipcCopy }, .out_handles = &h);
    if (R_SUCCEEDED(rc)) svcCloseHandle(h);
    return rc;
}

static Result raw_cmd(Raw *r, u32 cmd) {
    serviceAssumeDomain(&r->ds);
    return serviceDispatch(&r->ds, cmd);
}

static Result raw_register(Raw *r) {
    Service s;
    const u8 intf = 0;
    serviceAssumeDomain(&r->ds);
    Result rc = serviceDispatchIn(&r->ds, 1, intf, .out_num_objects = 1, .out_objects = &s);
    if (R_SUCCEEDED(rc)) { serviceAssumeDomain(&s); serviceClose(&s); }
    return rc;
}

static void raw_test(const char *name, bool ev, bool clear) {
    Raw r;
    Result rc = raw_open_bind(&r);
    if (R_FAILED(rc)) { out("%-34s bind -> 0x%X", name, rc); return; }
    Result re = ev ? raw_state_event(&r) : 0;
    Result rcl = clear ? raw_cmd(&r, 4) : 0;
    Result rr = raw_register(&r);
    out("%-34s ev=0x%X clear=0x%X REG=0x%X", name, re, rcl, rr);
    raw_close(&r);
}

// Ejecuta los primeros `upto` pasos de configuración y luego intenta registrar.
static void test_prefix(int upto) {
    static const char *names[] = {"(nada)", "+idioma", "+texto", "+desc Full", "+desc High", "+desc Super", "+BOS"};
    out("T%d: init %s, luego registrar", upto, names[upto]);
    UsbDsInterface *intf = NULL;
    u8 idx;
    struct usb_device_descriptor d;
    STEP("usbDsInitialize", usbDsInitialize());
    if (upto >= 1) STEP("AddLanguageString", usbDsAddUsbLanguageStringDescriptor(NULL, g_langs, 1));
    if (upto >= 2) STEP("AddString", usbDsAddUsbStringDescriptor(&idx, "Nintendo"));
    if (upto >= 3) { dev_desc(&d, 0x0110, 0x40); STEP("SetDeviceDesc Full", usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &d)); }
    if (upto >= 4) { dev_desc(&d, 0x0200, 0x40); STEP("SetDeviceDesc High", usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &d)); }
    if (upto >= 5) { dev_desc(&d, 0x0300, 0x09); STEP("SetDeviceDesc Super", usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &d)); }
    if (upto >= 6) STEP("SetBinaryObjectStore", usbDsSetBinaryObjectStore(g_bos, sizeof(g_bos)));
    STEP("RegisterInterface", usbDsRegisterInterface(&intf));
done:
    usbDsExit();
}

// Secuencia completa con el registro primero (lo que usará USBS2).
static void test_register_first(void) {
    out("T7: registrar PRIMERO y configurar despues");
    UsbDsInterface *intf = NULL;
    UsbDsEndpoint *ep_in = NULL, *ep_out = NULL, *ep_irq = NULL;
    u8 iMan, iProd, iSer;
    struct usb_device_descriptor d;

    STEP("usbDsInitialize", usbDsInitialize());
    STEP("RegisterInterface", usbDsRegisterInterface(&intf));
    out("  (interfaz numero %u)", intf->interface_index);

    struct usb_interface_descriptor id = {0};
    id.bLength = USB_DT_INTERFACE_SIZE;
    id.bDescriptorType = USB_DT_INTERFACE;
    id.bInterfaceNumber = intf->interface_index;
    id.bNumEndpoints = 3;
    id.bInterfaceClass = 0x06;
    id.bInterfaceSubClass = 0x01;
    id.bInterfaceProtocol = 0x01;

    struct usb_endpoint_descriptor ep = {0};
    ep.bLength = USB_DT_ENDPOINT_SIZE;
    ep.bDescriptorType = USB_DT_ENDPOINT;

    struct usb_ss_endpoint_companion_descriptor comp = {0};
    comp.bLength = sizeof(comp);
    comp.bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION;

    const UsbDeviceSpeed speeds[3] = {UsbDeviceSpeed_Full, UsbDeviceSpeed_High, UsbDeviceSpeed_Super};
    const u16 mps[3] = {0x40, 0x200, 0x400};
    for (int s = 0; s < 3; s++) {
        bool ss = speeds[s] == UsbDeviceSpeed_Super;
        STEP("Append intf", usbDsInterface_AppendConfigurationData(intf, speeds[s], &id, USB_DT_INTERFACE_SIZE));
        const u8 addrs[3] = {USB_ENDPOINT_IN + 1, USB_ENDPOINT_OUT + 1, USB_ENDPOINT_IN + 2};
        for (int e = 0; e < 3; e++) {
            ep.bEndpointAddress = addrs[e];
            ep.bmAttributes = e < 2 ? USB_TRANSFER_TYPE_BULK : USB_TRANSFER_TYPE_INTERRUPT;
            ep.wMaxPacketSize = e < 2 ? mps[s] : 0x1C;
            ep.bInterval = e < 2 ? 0 : 6;
            STEP("Append endpoint", usbDsInterface_AppendConfigurationData(intf, speeds[s], &ep, USB_DT_ENDPOINT_SIZE));
            if (ss) {
                comp.bMaxBurst = e < 2 ? 0x0F : 0;
                comp.wBytesPerInterval = e < 2 ? 0 : 0x1C;
                STEP("Append companion", usbDsInterface_AppendConfigurationData(intf, speeds[s], &comp, sizeof(comp)));
            }
        }
        out("  velocidad %d configurada", (int)speeds[s]);
    }
    STEP("RegisterEndpoint IN", usbDsInterface_RegisterEndpoint(intf, &ep_in, USB_ENDPOINT_IN + 1));
    STEP("RegisterEndpoint OUT", usbDsInterface_RegisterEndpoint(intf, &ep_out, USB_ENDPOINT_OUT + 1));
    STEP("RegisterEndpoint IRQ", usbDsInterface_RegisterEndpoint(intf, &ep_irq, USB_ENDPOINT_IN + 2));
    STEP("EnableInterface", usbDsInterface_EnableInterface(intf));

    STEP("AddLanguageString", usbDsAddUsbLanguageStringDescriptor(NULL, g_langs, 1));
    STEP("AddString fab", usbDsAddUsbStringDescriptor(&iMan, "Nintendo"));
    STEP("AddString prod", usbDsAddUsbStringDescriptor(&iProd, "Switch 2 (USBS2)"));
    STEP("AddString serie", usbDsAddUsbStringDescriptor(&iSer, "USBS2-0001"));
    dev_desc(&d, 0x0110, 0x40);
    d.iManufacturer = iMan; d.iProduct = iProd; d.iSerialNumber = iSer;
    STEP("SetDeviceDesc Full", usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &d));
    d.bcdUSB = 0x0200;
    STEP("SetDeviceDesc High", usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &d));
    d.bcdUSB = 0x0300; d.bMaxPacketSize0 = 0x09;
    STEP("SetDeviceDesc Super", usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &d));
    STEP("SetBinaryObjectStore", usbDsSetBinaryObjectStore(g_bos, sizeof(g_bos)));
    STEP("usbDsEnable", usbDsEnable());
    out("  T7 COMPLETO: la consola deberia aparecer como dispositivo USB");
    {
        UsbState st = 0;
        usbDsGetState(&st);
        out("  estado USB ahora: %d (5 = configurado por el PC)", (int)st);
    }
done:
    usbDsExit();
}

int main(int argc, char **argv) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    g_log = fopen("sdmc:/switch/usbprobe.log", "w");
    out("usbprobe 3 - causa del bloqueo de usb:ds");
    out("== IPC directo: bind + ... + registrar ==");
    raw_test("V1 bind+registrar", false, false);
    raw_test("V2 bind+evento+registrar", true, false);
    raw_test("V3 bind+ClearDeviceData+registrar", false, true);
    raw_test("V4 bind+evento+Clear+registrar", true, true);
    out("");
    out("== libounce (con parche Switch 2) ==");
    for (int i = 0; i <= 6; i++) test_prefix(i);
    test_register_first();
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
