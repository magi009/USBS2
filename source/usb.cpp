// usb.cpp - Capa USB (usb:ds) para exponer la consola como dispositivo MTP.
#include "usb.h"
#include <cstring>

namespace usb {

static UsbDsInterface *g_intf = nullptr;
static UsbDsEndpoint *g_ep_in = nullptr, *g_ep_out = nullptr, *g_ep_irq = nullptr;
static std::atomic<bool> *g_quit = nullptr;

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
    if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(g_intf, speed, &ep, USB_DT_ENDPOINT_SIZE))) return rc;
    if (speed == UsbDeviceSpeed_Super && R_FAILED(rc = usbDsInterface_AppendConfigurationData(g_intf, speed, &comp, sizeof(comp)))) return rc;
    // Bulk OUT
    ep.bEndpointAddress = addr_out;
    if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(g_intf, speed, &ep, USB_DT_ENDPOINT_SIZE))) return rc;
    if (speed == UsbDeviceSpeed_Super && R_FAILED(rc = usbDsInterface_AppendConfigurationData(g_intf, speed, &comp, sizeof(comp)))) return rc;
    // Interrupt IN (eventos MTP)
    ep.bEndpointAddress = addr_irq;
    ep.bmAttributes = USB_TRANSFER_TYPE_INTERRUPT;
    ep.wMaxPacketSize = 0x1C;
    ep.bInterval = 6;
    if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(g_intf, speed, &ep, USB_DT_ENDPOINT_SIZE))) return rc;
    if (speed == UsbDeviceSpeed_Super) {
        comp.bMaxBurst = 0;
        comp.wBytesPerInterval = 0x1C;
        if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(g_intf, speed, &comp, sizeof(comp)))) return rc;
    }
    return 0;
}

Result init(std::atomic<bool> *quit_flag) {
    g_quit = quit_flag;
    Result rc = usbDsInitialize();
    if (R_FAILED(rc)) return rc;

    static const u16 langs[1] = {0x0409};
    u8 iManufacturer, iProduct, iSerial, iInterface;
    if (R_FAILED(rc = usbDsAddUsbLanguageStringDescriptor(nullptr, langs, 1))) return rc;
    if (R_FAILED(rc = usbDsAddUsbStringDescriptor(&iManufacturer, "Nintendo"))) return rc;
    if (R_FAILED(rc = usbDsAddUsbStringDescriptor(&iProduct, "Switch 2 (USBS2)"))) return rc;
    if (R_FAILED(rc = usbDsAddUsbStringDescriptor(&iSerial, "USBS2-0001"))) return rc;
    if (R_FAILED(rc = usbDsAddUsbStringDescriptor(&iInterface, "MTP"))) return rc;

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
    if (R_FAILED(rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &dev))) return rc;
    dev.bcdUSB = 0x0200;
    if (R_FAILED(rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &dev))) return rc;
    dev.bcdUSB = 0x0300;
    dev.bMaxPacketSize0 = 0x09;
    if (R_FAILED(rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &dev))) return rc;

    static const u8 bos[0x16] = {
        0x05, 0x0F, 0x16, 0x00, 0x02,
        0x07, 0x10, 0x02, 0x02, 0x00, 0x00, 0x00,
        0x0A, 0x10, 0x03, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00,
    };
    if (R_FAILED(rc = usbDsSetBinaryObjectStore(bos, sizeof(bos)))) return rc;

    if (R_FAILED(rc = usbDsRegisterInterface(&g_intf))) return rc;

    usb_interface_descriptor intf{};
    intf.bLength = USB_DT_INTERFACE_SIZE;
    intf.bDescriptorType = USB_DT_INTERFACE;
    intf.bInterfaceNumber = g_intf->interface_index;
    intf.bNumEndpoints = 3;
    intf.bInterfaceClass = 0x06;    // Still Image
    intf.bInterfaceSubClass = 0x01; // MTP
    intf.bInterfaceProtocol = 0x01;
    intf.iInterface = iInterface;

    const u8 addr_in = USB_ENDPOINT_IN + 1, addr_out = USB_ENDPOINT_OUT + 1, addr_irq = USB_ENDPOINT_IN + 2;
    const UsbDeviceSpeed speeds[3] = {UsbDeviceSpeed_Full, UsbDeviceSpeed_High, UsbDeviceSpeed_Super};
    for (auto sp : speeds) {
        if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(g_intf, sp, &intf, USB_DT_INTERFACE_SIZE))) return rc;
        if (R_FAILED(rc = add_endpoint_descs(sp, addr_in, addr_out, addr_irq))) return rc;
    }

    if (R_FAILED(rc = usbDsInterface_RegisterEndpoint(g_intf, &g_ep_in, addr_in))) return rc;
    if (R_FAILED(rc = usbDsInterface_RegisterEndpoint(g_intf, &g_ep_out, addr_out))) return rc;
    if (R_FAILED(rc = usbDsInterface_RegisterEndpoint(g_intf, &g_ep_irq, addr_irq))) return rc;
    if (R_FAILED(rc = usbDsInterface_EnableInterface(g_intf))) return rc;
    return usbDsEnable();
}

void exit() { usbDsExit(); }

bool wait_ready() {
    while (!g_quit->load()) {
        if (R_SUCCEEDED(usbDsWaitReady(100'000'000ULL))) return true;
    }
    return false;
}

u32 max_packet() {
    UsbDeviceSpeed sp;
    if (R_SUCCEEDED(usbDsGetSpeed(&sp))) {
        if (sp == UsbDeviceSpeed_Super) return 0x400;
        if (sp == UsbDeviceSpeed_Full) return 0x40;
    }
    return 0x200;
}

static bool xfer(UsbDsEndpoint *ep, void *buf, u32 size, u32 *out) {
    u32 urb = 0;
    if (R_FAILED(usbDsEndpoint_PostBufferAsync(ep, buf, size, &urb))) return false;
    for (;;) {
        Result rc = eventWait(&ep->CompletionEvent, 100'000'000ULL);
        if (R_SUCCEEDED(rc)) break;
        if (g_quit->load()) {
            usbDsEndpoint_Cancel(ep);
            eventWait(&ep->CompletionEvent, 1'000'000'000ULL);
            eventClear(&ep->CompletionEvent);
            return false;
        }
        u32 st = 0;
        if (R_SUCCEEDED(usbDsGetState(&st)) && st != 5) { // cable desconectado
            usbDsEndpoint_Cancel(ep);
            eventWait(&ep->CompletionEvent, 1'000'000'000ULL);
            eventClear(&ep->CompletionEvent);
            return false;
        }
    }
    eventClear(&ep->CompletionEvent);
    UsbDsReportData report;
    if (R_FAILED(usbDsEndpoint_GetReportData(ep, &report))) return false;
    u32 done = 0;
    if (R_FAILED(usbDsParseReportData(&report, urb, nullptr, &done))) return false;
    if (out) *out = done;
    return true;
}

bool read(void *buf, u32 size, u32 *out) {
    if (!xfer(g_ep_out, buf, size, out)) return false;
    armDCacheFlush(buf, *out);
    return true;
}

bool write(const void *buf, u32 size) {
    armDCacheFlush(const_cast<void *>(buf), size);
    u32 done = 0;
    return xfer(g_ep_in, const_cast<void *>(buf), size, &done) && done == size;
}

} // namespace usb
