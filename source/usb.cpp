// usb.cpp - Capa USB (usb:ds) para exponer la consola como dispositivo MTP.
#include "usb.h"
#include "log.h"
#include <cstring>

namespace usb {

static UsbDsInterface *g_intf = nullptr;
static UsbDsEndpoint *g_ep_in = nullptr, *g_ep_out = nullptr, *g_ep_irq = nullptr;
static std::atomic<bool> *g_quit = nullptr;
static std::atomic<bool> g_cancel_req{false}; // el PC mandó Cancel Request por el canal de control

#define TRY(call) do { rc = (call); if (R_FAILED(rc)) log_write("    usb: %s -> 0x%X", #call, rc); else log_debug("    usb: %s -> 0x%X", #call, rc); if (R_FAILED(rc)) return rc; } while (0)

static Result log_rc(const char *what, Result rc) {
    if (R_FAILED(rc)) log_write("    usb: %s -> 0x%X", what, rc); else log_debug("    usb: %s -> 0x%X", what, rc);
    return rc;
}

static Result add_endpoint_descs(UsbDeviceSpeed speed, u8 addr_in, u8 addr_out, u8 addr_irq) {
    Result rc;
    u16 bulk_mps = speed == UsbDeviceSpeed_Full ? 0x40 : (speed == UsbDeviceSpeed_High ? 0x200 : 0x400);

    usb_endpoint_descriptor ep{};
    ep.bLength = USB_DT_ENDPOINT_SIZE;
    ep.bDescriptorType = USB_DT_ENDPOINT;
    ep.bmAttributes = USB_TRANSFER_TYPE_BULK;
    ep.wMaxPacketSize = bulk_mps;

    usb_ss_endpoint_companion_descriptor comp{};
    comp.bLength = sizeof(comp);
    comp.bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION;
    comp.bMaxBurst = 0x0F;

    // Bulk IN
    ep.bEndpointAddress = addr_in;
    if (R_FAILED(rc = log_rc("append", usbDsInterface_AppendConfigurationData(g_intf, speed, &ep, USB_DT_ENDPOINT_SIZE)))) return rc;
    if (speed == UsbDeviceSpeed_Super && R_FAILED(rc = log_rc("append", usbDsInterface_AppendConfigurationData(g_intf, speed, &comp, sizeof(comp))))) return rc;
    // Bulk OUT
    ep.bEndpointAddress = addr_out;
    if (R_FAILED(rc = log_rc("append", usbDsInterface_AppendConfigurationData(g_intf, speed, &ep, USB_DT_ENDPOINT_SIZE)))) return rc;
    if (speed == UsbDeviceSpeed_Super && R_FAILED(rc = log_rc("append", usbDsInterface_AppendConfigurationData(g_intf, speed, &comp, sizeof(comp))))) return rc;
    // Interrupt IN (eventos MTP)
    ep.bEndpointAddress = addr_irq;
    ep.bmAttributes = USB_TRANSFER_TYPE_INTERRUPT;
    ep.wMaxPacketSize = 0x1C;
    ep.bInterval = 6;
    if (R_FAILED(rc = log_rc("append", usbDsInterface_AppendConfigurationData(g_intf, speed, &ep, USB_DT_ENDPOINT_SIZE)))) return rc;
    if (speed == UsbDeviceSpeed_Super) {
        comp.bMaxBurst = 0;
        comp.wBytesPerInterval = 0x1C;
        if (R_FAILED(rc = log_rc("append", usbDsInterface_AppendConfigurationData(g_intf, speed, &comp, sizeof(comp))))) return rc;
    }
    return 0;
}

// ---- Diagnóstico: peticiones por el canal de control (endpoint 0) ----
static Thread g_setup_thread;
static bool g_setup_thread_ok = false;
static std::atomic<u32> g_setup_count{0};

u32 setup_count() { return g_setup_count.load(); }

// Comando IDsInterface sin argumentos (para probar números en Switch 2).
static Result intf_cmd_noio(u32 cmd) {
    serviceAssumeDomain(&g_intf->s);
    return serviceDispatch(&g_intf->s, cmd);
}

// Peticiones de control de la clase MTP (Still Image):
//   21 64 = Cancel Request (el PC cancela la operación en curso; 6 bytes de datos)
//   A1 67 = Get Device Status (el PC pregunta si el dispositivo está listo)
// En Switch 2 la consola completa sola la fase de estado: solo se manda/recibe la
// fase de datos (CtrlIn / CtrlOut), sin el paso extra que bloqueaba el canal.
bool cancel_requested() { return g_cancel_req.load(); }
void clear_cancel() { g_cancel_req = false; }

static void handle_setup() {
    alignas(0x1000) static u8 pkt[0x1000];
    alignas(0x1000) static u8 data[0x1000];
    memset(pkt, 0, 8);
    Result rc = usbDsInterface_GetSetupPacket(g_intf, pkt, 8);
    log_debug("    usb: control: %02X %02X %02X %02X %02X %02X %02X %02X (0x%X)", pkt[0], pkt[1], pkt[2], pkt[3],
              pkt[4], pkt[5], pkt[6], pkt[7], rc);
    if (R_FAILED(rc)) { log_write("    usb: control: GetSetupPacket -> 0x%X", rc); return; }

    u8 type = pkt[0], req = pkt[1];
    u16 wLength = pkt[6] | (pkt[7] << 8);
    u32 urb = 0;
    if (type == 0x21 && req == 0x64) {
        // Cancel Request: recibir sus datos (código 0x4001 + id de transacción)
        g_cancel_req = true;
        u32 n = wLength < 0x1000 ? wLength : 0x1000;
        memset(data, 0xEE, 16);
        rc = usbDsInterface_CtrlOutPostBufferAsync(g_intf, data, n, &urb);
        svcSleepThread(20'000'000ULL); // dar tiempo a que lleguen los 6 bytes
        armDCacheFlush(data, 16);
        u16 code = data[0] | (data[1] << 8);
        u32 tid = data[2] | (data[3] << 8) | (data[4] << 16) | ((u32)data[5] << 24);
        log_write("    usb: control: el PC CANCELO (CtrlOut %u -> 0x%X): codigo 0x%04X, operacion tid 0x%X", n, rc, code, tid);
        return;
    }
    if (type == 0xA1 && req == 0x67) {
        // Get Device Status: largo (4) + OK (0x2001)
        data[0] = 4; data[1] = 0; data[2] = 0x01; data[3] = 0x20;
        u32 n = wLength < 4 ? wLength : 4;
        rc = usbDsInterface_CtrlInPostBufferAsync(g_intf, data, n, &urb);
        if (R_FAILED(rc)) log_write("    usb: control: GET_DEVICE_STATUS: CtrlIn -> 0x%X", rc);
        else log_debug("    usb: control: GET_DEVICE_STATUS -> OK");
        return;
    }
    log_write("    usb: control: peticion no manejada %02X %02X (largo %u)", type, req, wLength);
}

static void setup_watch(void *) {
    while (!g_quit->load()) {
        if (!g_intf) { svcSleepThread(100'000'000ULL); continue; }
        Result rc = eventWait(&g_intf->SetupEvent, 100'000'000ULL);
        if (R_FAILED(rc)) continue;
        eventClear(&g_intf->SetupEvent);
        u32 n = ++g_setup_count;
        log_debug("    usb: peticion de control #%u", n);
        handle_setup();
    }
}

Result init(std::atomic<bool> *quit_flag) {
    g_quit = quit_flag;
    Result rc;
    TRY(usbDsInitialize());

    // Switch 2: la interfaz se registra ANTES de cualquier descriptor del
    // dispositivo; si se ponen primero los textos/descriptores (orden de la
    // Switch 1), usb:ds rechaza el registro con 0x4BB25.
    TRY(usbDsRegisterInterface(&g_intf));

    usb_interface_descriptor intf{};
    intf.bLength = USB_DT_INTERFACE_SIZE;
    intf.bDescriptorType = USB_DT_INTERFACE;
    intf.bInterfaceNumber = g_intf->interface_index;
    intf.bNumEndpoints = 3;
    intf.bInterfaceClass = 0x06;    // Still Image
    intf.bInterfaceSubClass = 0x01; // MTP
    intf.bInterfaceProtocol = 0x01;
    intf.iInterface = 0;

    const u8 addr_in = USB_ENDPOINT_IN + 1, addr_out = USB_ENDPOINT_OUT + 1, addr_irq = USB_ENDPOINT_IN + 2;
    const UsbDeviceSpeed speeds[3] = {UsbDeviceSpeed_Full, UsbDeviceSpeed_High, UsbDeviceSpeed_Super};
    for (auto sp : speeds) {
        TRY(usbDsInterface_AppendConfigurationData(g_intf, sp, &intf, USB_DT_INTERFACE_SIZE));
        TRY(add_endpoint_descs(sp, addr_in, addr_out, addr_irq));
    }

    TRY(usbDsInterface_RegisterEndpoint(g_intf, &g_ep_in, addr_in));
    TRY(usbDsInterface_RegisterEndpoint(g_intf, &g_ep_out, addr_out));
    TRY(usbDsInterface_RegisterEndpoint(g_intf, &g_ep_irq, addr_irq));
    TRY(usbDsInterface_EnableInterface(g_intf));

    static const u16 langs[1] = {0x0409};
    u8 iManufacturer, iProduct, iSerial;
    TRY(usbDsAddUsbLanguageStringDescriptor(nullptr, langs, 1));
    TRY(usbDsAddUsbStringDescriptor(&iManufacturer, "Nintendo"));
    TRY(usbDsAddUsbStringDescriptor(&iProduct, "Switch 2 (USBS2)"));
    TRY(usbDsAddUsbStringDescriptor(&iSerial, "USBS2-0001"));

    usb_device_descriptor dev{};
    dev.bLength = USB_DT_DEVICE_SIZE;
    dev.bDescriptorType = USB_DT_DEVICE;
    dev.bcdUSB = 0x0110;
    dev.bMaxPacketSize0 = 0x40;
    dev.idVendor = 0x057E;
    dev.idProduct = 0x201D;
    dev.bcdDevice = 0x0100;
    dev.iManufacturer = iManufacturer;
    dev.iProduct = iProduct;
    dev.iSerialNumber = iSerial;
    dev.bNumConfigurations = 1;
    TRY(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &dev));
    dev.bcdUSB = 0x0200;
    TRY(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &dev));
    dev.bcdUSB = 0x0300;
    dev.bMaxPacketSize0 = 0x09;
    TRY(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &dev));

    static const u8 bos[0x16] = {
        0x05, 0x0F, 0x16, 0x00, 0x02,
        0x07, 0x10, 0x02, 0x02, 0x00, 0x00, 0x00,
        0x0A, 0x10, 0x03, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00,
    };
    TRY(usbDsSetBinaryObjectStore(bos, sizeof(bos)));
    TRY(usbDsEnable());
    if (R_SUCCEEDED(threadCreate(&g_setup_thread, setup_watch, nullptr, nullptr, 0x4000, 0x2C, -2)) &&
        R_SUCCEEDED(threadStart(&g_setup_thread)))
        g_setup_thread_ok = true;
    return 0;
}

void exit() {
    if (g_setup_thread_ok) {
        threadWaitForExit(&g_setup_thread);
        threadClose(&g_setup_thread);
        g_setup_thread_ok = false;
    }
    usbDsExit();
}

static void update_max_packet();

bool wait_ready() {
    Result last = 0;
    while (!g_quit->load()) {
        Result rc = usbDsWaitReady(100'000'000ULL);
        if (R_SUCCEEDED(rc)) { update_max_packet(); return true; }
        if (rc != last) { log_debug("    usb: usbDsWaitReady -> 0x%X", rc); last = rc; }
    }
    return false;
}

static u32 g_mps = 0x200;

u32 max_packet() { return g_mps; }

static void update_max_packet() {
    UsbDeviceSpeed sp = UsbDeviceSpeed_None;
    Result rc = usbDsGetSpeed(&sp);
    g_mps = 0x200;
    if (R_SUCCEEDED(rc)) {
        if (sp == UsbDeviceSpeed_Super) g_mps = 0x400;
        else if (sp == UsbDeviceSpeed_Full) g_mps = 0x40;
    }
    log_write("    usb: velocidad %d (paquete max %u)", (int)sp, g_mps);
}

// Una transferencia en curso por endpoint (como mucho).
struct Pending { UsbDsEndpoint *ep; void *buf; u32 size; u32 urb; bool active; bool is_read; };
static Pending g_rd = {}, g_wr = {};

static bool begin(Pending &p, UsbDsEndpoint *ep, void *buf, u32 size, bool is_read) {
    if (!is_read) armDCacheFlush(buf, size);
    p = {ep, buf, size, 0, false, is_read};
    Result rc = usbDsEndpoint_PostBufferAsync(ep, buf, size, &p.urb);
    if (R_FAILED(rc)) { log_write("    usb: PostBufferAsync(%u) -> 0x%X", size, rc); return false; }
    p.active = true;
    return true;
}

static void cancel(Pending &p) {
    if (!p.active) return;
    usbDsEndpoint_Cancel(p.ep);
    eventWait(&p.ep->CompletionEvent, 1'000'000'000ULL);
    eventClear(&p.ep->CompletionEvent);
    p.active = false;
}

static Status finish(Pending &p, u32 *out, u32 timeout_ms) {
    if (!p.active) return Status::Error;
    u64 waited_ms = 0;
    for (;;) {
        Result rc = eventWait(&p.ep->CompletionEvent, 100'000'000ULL);
        if (R_SUCCEEDED(rc)) break;
        waited_ms += 100;
        if (g_quit->load()) { cancel(p); return Status::Error; }
        UsbState st;
        if (R_SUCCEEDED(usbDsGetState(&st)) && st != UsbState_Configured) {
            log_write("    usb: cable desconectado (estado %d)", (int)st);
            cancel(p);
            return Status::Error;
        }
        if (timeout_ms && waited_ms >= timeout_ms) { cancel(p); return Status::Timeout; }
        if (g_cancel_req.load()) { cancel(p); return Status::Timeout; } // el PC canceló (Cancel Request)
    }
    eventClear(&p.ep->CompletionEvent);
    p.active = false;
    UsbDsReportData report;
    Result grc = usbDsEndpoint_GetReportData(p.ep, &report);
    if (R_FAILED(grc)) { log_write("    usb: GetReportData -> 0x%X", grc); return Status::Error; }
    u32 done = 0;
    Result parc = usbDsParseReportData(&report, p.urb, nullptr, &done);
    if (R_FAILED(parc)) { log_write("    usb: ParseReportData -> 0x%X", parc); return Status::Error; }
    if (p.is_read) armDCacheFlush(p.buf, done);
    if (g_log_verbose) {
        const u8 *b = (const u8 *)p.buf;
        char hex[3 * 32 + 1] = {0};
        u32 lim = done <= 32 ? done : 16;
        for (u32 i = 0; i < lim; i++) snprintf(hex + 3 * i, 4, "%02X ", b[i]);
        log_write("    usb %s %u de %u bytes (urb %u): %s", p.is_read ? "LEE" : "ESCRIBE", done, p.size, p.urb, hex);
    }
    if (out) *out = done;
    return Status::Ok;
}

// ---- Cola de lecturas PC -> consola (varias URB publicadas a la vez) ----
// En Switch 2 los datos que llegan sin una lectura publicada hacen que el PC
// cancele; con varias lecturas en cola nunca hay un hueco.
static constexpr int RXQ_MAX = 4;
static struct {
    void *buf[RXQ_MAX];
    u32 urb[RXQ_MAX];
    bool posted[RXQ_MAX];
    u32 seq[RXQ_MAX];  // orden de publicación
    bool fin[RXQ_MAX];  // completada (visto en un informe)
    u32 fin_size[RXQ_MAX], fin_status[RXQ_MAX];
    u32 next_seq = 0;
    int n = 0;
    u32 size = 0;
    int head = 0;      // la próxima en completarse (orden de publicación)
} g_rxq;

bool rx_repost(int i) {
    if (i < 0 || i >= g_rxq.n || g_rxq.posted[i]) return true;
    armDCacheFlush(g_rxq.buf[i], g_rxq.size);
    Result rc = usbDsEndpoint_PostBufferAsync(g_ep_out, g_rxq.buf[i], g_rxq.size, &g_rxq.urb[i]);
    if (R_FAILED(rc)) { log_write("    usb: rx PostBufferAsync -> 0x%X", rc); return false; }
    g_rxq.posted[i] = true;
    g_rxq.fin[i] = false;
    g_rxq.seq[i] = g_rxq.next_seq++;
    return true;
}

bool rx_start(void *const *bufs, int n, u32 size) {
    g_rxq.n = n > RXQ_MAX ? RXQ_MAX : n;
    g_rxq.size = size;
    g_rxq.head = 0;
    for (int i = 0; i < g_rxq.n; i++) { g_rxq.buf[i] = bufs[i]; g_rxq.posted[i] = false; }
    for (int i = 0; i < g_rxq.n; i++)
        if (!rx_repost(i)) return false;
    return true;
}

void rx_stop() {
    bool any = false;
    for (int i = 0; i < g_rxq.n; i++) any |= g_rxq.posted[i];
    if (any) {
        usbDsEndpoint_Cancel(g_ep_out);
        eventWait(&g_ep_out->CompletionEvent, 1'000'000'000ULL);
        eventClear(&g_ep_out->CompletionEvent);
    }
    for (int i = 0; i < g_rxq.n; i++) { g_rxq.posted[i] = false; g_rxq.fin[i] = false; }
    g_rxq.head = 0;
}

// Busca la entrada de un urb en el informe del endpoint.
static const UsbDsReportEntry *find_entry(const UsbDsReportData &r, u32 urb) {
    u32 c = r.report_count > 8 ? 8 : r.report_count;
    for (u32 k = 0; k < c; k++) if (r.report[k].id == urb) return &r.report[k];
    return nullptr;
}


// Espera a que se complete una lectura de la cola (la más antigua que haya terminado).
// Devuelve su índice; el buffer queda en manos del que llama hasta que haga rx_repost(índice).
// En la Switch 2 cada finalización aparece en un solo informe: si no se guarda en ese
// momento, esa lectura parece que nunca termina y sus datos se pierden.
Status rx_wait(int *idx, u32 *got, u32 timeout_ms) {
    if (g_rxq.n == 0) return Status::Error;
    bool any = false;
    for (int i = 0; i < g_rxq.n; i++) any |= g_rxq.posted[i];
    if (!any) return Status::Error;
    u64 waited_ms = 0;
    for (;;) {
        UsbDsReportData report{};
        if (R_SUCCEEDED(usbDsEndpoint_GetReportData(g_ep_out, &report))) {
            // El informe solo trae cada finalización una vez: se guardan todas las
            // que aparezcan, aunque no sean la que buscamos ahora.
            u32 cnt = report.report_count > 8 ? 8 : report.report_count;
            for (u32 k = 0; k < cnt; k++) {
                const UsbDsReportEntry &e = report.report[k];
                if (e.urb_status < 3) continue;
                for (int i = 0; i < g_rxq.n; i++)
                    if (g_rxq.posted[i] && !g_rxq.fin[i] && g_rxq.urb[i] == e.id) {
                        g_rxq.fin[i] = true;
                        g_rxq.fin_size[i] = e.transferredSize;
                        g_rxq.fin_status[i] = e.urb_status;
                    }
            }
        }
        {
            int best = -1;
            for (int i = 0; i < g_rxq.n; i++) {
                if (!g_rxq.posted[i] || !g_rxq.fin[i]) continue;
                if (best < 0 || (s32)(g_rxq.seq[i] - g_rxq.seq[best]) < 0) best = i;
            }
            if (best >= 0 && g_rxq.fin_status[best] != 3) {
                log_write("    usb: rx urb %u estado %u", g_rxq.urb[best], g_rxq.fin_status[best]);
                return Status::Error;
            }
            if (best >= 0) {
                int i = best;
                u32 done = g_rxq.fin_size[i];
                // ¿quedó alguna publicada más antigua sin completar?
                for (int k = 0; k < g_rxq.n; k++)
                    if (k != i && g_rxq.posted[k] && (s32)(g_rxq.seq[k] - g_rxq.seq[i]) < 0) {
                        log_debug("    usb: lectura fuera de orden (cola %d urb %u completo antes que cola %d urb %u)",
                                  i, g_rxq.urb[i], k, g_rxq.urb[k]);
                        break;
                    }
                g_rxq.posted[i] = false;
                g_rxq.fin[i] = false;
                // nueva cabeza = la publicada más antigua que quede
                int h = -1;
                for (int k = 0; k < g_rxq.n; k++)
                    if (g_rxq.posted[k] && (h < 0 || (s32)(g_rxq.seq[k] - g_rxq.seq[h]) < 0)) h = k;
                g_rxq.head = h >= 0 ? h : (i + 1) % g_rxq.n;
                armDCacheFlush(g_rxq.buf[i], done);
                if (g_log_verbose) {
                    const u8 *b = (const u8 *)g_rxq.buf[i];
                    char hex[3 * 32 + 1] = {0};
                    for (u32 k = 0; k < done && k < 32; k++) snprintf(hex + 3 * k, 4, "%02X ", b[k]);
                    log_write("    usb LEE %u (cola %d, urb %u): %s", done, i, g_rxq.urb[i], hex);
                }
                *idx = i;
                *got = done;
                return Status::Ok;
            }
        }
        Result rc = eventWait(&g_ep_out->CompletionEvent, 100'000'000ULL);
        if (R_SUCCEEDED(rc)) { eventClear(&g_ep_out->CompletionEvent); continue; }
        waited_ms += 100;
        if (g_quit->load()) return Status::Error;
        UsbState st;
        if (R_SUCCEEDED(usbDsGetState(&st)) && st != UsbState_Configured) {
            log_write("    usb: cable desconectado (estado %d)", (int)st);
            return Status::Error;
        }
        if (timeout_ms && waited_ms >= timeout_ms) return Status::Timeout;
        if (g_cancel_req.load()) {
            return Status::Timeout; // el PC canceló (Cancel Request)
        }
    }
}

bool begin_read(void *buf, u32 size) { return begin(g_rd, g_ep_out, buf, size, true); }
Status end_read(u32 *out, u32 timeout_ms) { return finish(g_rd, out, timeout_ms); }
bool begin_write(const void *buf, u32 size) { return begin(g_wr, g_ep_in, const_cast<void *>(buf), size, false); }
Status end_write(u32 timeout_ms) {
    u32 size = g_wr.size, done = 0;
    Status s = finish(g_wr, &done, timeout_ms);
    if (s == Status::Ok && done != size) return Status::Error;
    return s;
}

Status read(void *buf, u32 size, u32 *out, u32 timeout_ms) {
    if (!begin_read(buf, size)) return Status::Error;
    return end_read(out, timeout_ms);
}

Status write(const void *buf, u32 size, u32 timeout_ms) {
    if (!begin_write(buf, size)) return Status::Error;
    return end_write(timeout_ms);
}

void cancel_all() { cancel(g_rd); cancel(g_wr); rx_stop(); }

} // namespace usb
