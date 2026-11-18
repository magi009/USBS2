// mtp.cpp - Respondedor MTP mínimo: el PC ve la consola como un dispositivo
// con 3 unidades: Tarjeta SD (sdmc:/), Homebrew (sdmc:/switch) y Partidas guardadas.
#include "mtp.h"
#include "usb.h"
#include "saves.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <malloc.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

namespace mtp {

// ---------------- constantes MTP ----------------
enum : u16 {
    CT_COMMAND = 1, CT_DATA = 2, CT_RESPONSE = 3,

    OP_GetDeviceInfo = 0x1001, OP_OpenSession = 0x1002, OP_CloseSession = 0x1003,
    OP_GetStorageIDs = 0x1004, OP_GetStorageInfo = 0x1005, OP_GetObjectHandles = 0x1007,
    OP_GetObjectInfo = 0x1008, OP_GetObject = 0x1009, OP_DeleteObject = 0x100B,
    OP_SendObjectInfo = 0x100C, OP_SendObject = 0x100D, OP_GetDevicePropDesc = 0x1014,
    OP_GetDevicePropValue = 0x1015, OP_MoveObject = 0x1019, OP_GetPartialObject = 0x101B,
    OP_GetObjectPropsSupported = 0x9801, OP_GetObjectPropDesc = 0x9802,
    OP_GetObjectPropValue = 0x9803, OP_SetObjectPropValue = 0x9804,
    OP_GetObjectReferences = 0x9810,

    RC_OK = 0x2001, RC_GeneralError = 0x2002, RC_SessionNotOpen = 0x2003,
    RC_OperationNotSupported = 0x2005, RC_InvalidStorageID = 0x2008,
    RC_InvalidObjectHandle = 0x2009, RC_DevicePropNotSupported = 0x200A,
    RC_StoreFull = 0x200C, RC_AccessDenied = 0x200F, RC_NoValidObjectInfo = 0x2015,
    RC_InvalidParentObject = 0x201A, RC_SessionAlreadyOpen = 0x201E,
    RC_InvalidObjectPropCode = 0xA801,

    FMT_Undefined = 0x3000, FMT_Association = 0x3001,

    PROP_StorageID = 0xDC01, PROP_ObjectFormat = 0xDC02, PROP_ProtectionStatus = 0xDC03,
    PROP_ObjectSize = 0xDC04, PROP_ObjectFileName = 0xDC07, PROP_DateModified = 0xDC09,
    PROP_ParentObject = 0xDC0B, PROP_PersistentUID = 0xDC41, PROP_Name = 0xDC44,

    DPROP_FriendlyName = 0xD402,

    DT_UINT16 = 0x0004, DT_UINT32 = 0x0006, DT_UINT64 = 0x0008, DT_UINT128 = 0x000A, DT_STR = 0xFFFF,
};

static constexpr u32 STOR_SD = 0x00010001, STOR_HB = 0x00020001, STOR_SV = 0x00030001;
static constexpr u32 ROOT = 0xFFFFFFFF;
static constexpr u32 BUF_SIZE = 4 * 1024 * 1024;

// ---------------- estado ----------------
static std::atomic<bool> *g_quit;
static u8 *g_buf;   // datos (alineado 0x1000)
static u8 *g_cmd;   // comandos y respuestas (alineado 0x1000)
static Mutex g_status_mtx;
static char g_status[128] = "Esperando cable USB...";
static std::atomic<u64> g_bytes{0};
static bool g_session = false;

void status(char *out, size_t size, u64 *bytes) {
    mutexLock(&g_status_mtx);
    snprintf(out, size, "%s", g_status);
    mutexUnlock(&g_status_mtx);
    *bytes = g_bytes.load();
}

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *fmt, ...) {
    mutexLock(&g_status_mtx);
    va_list va;
    va_start(va, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, va);
    va_end(va);
    mutexUnlock(&g_status_mtx);
}

// ---------------- objetos ----------------
struct Obj {
    u32 storage;
    int save;        // índice de partida (solo STOR_SV), -1 en otro caso
    std::string rel; // ruta relativa a la raíz de la unidad ("" = raíz de la partida)
    u32 parent;      // 0 = raíz
    bool alive;
};
static std::vector<Obj> g_objs;
static std::unordered_map<std::string, u32> g_keys;

static std::string key_of(u32 st, int sv, const std::string &rel) {
    return std::to_string(st) + "|" + std::to_string(sv) + "|" + rel;
}

static u32 handle_for(u32 st, int sv, const std::string &rel, u32 parent) {
    std::string k = key_of(st, sv, rel);
    auto it = g_keys.find(k);
    if (it != g_keys.end()) {
        Obj &o = g_objs[it->second - 1];
        o.alive = true;
        o.parent = parent;
        return it->second;
    }
    g_objs.push_back({st, sv, rel, parent, true});
    u32 h = (u32)g_objs.size();
    g_keys[k] = h;
    return h;
}

static Obj *get_obj(u32 h) {
    if (h == 0 || h > g_objs.size()) return nullptr;
    Obj &o = g_objs[h - 1];
    return o.alive ? &o : nullptr;
}

static void reset_objects() {
    g_objs.clear();
    g_keys.clear();
}

static bool valid_storage(u32 st) { return st == STOR_SD || st == STOR_HB || st == STOR_SV; }

static bool base_path(u32 st, int sv, std::string &out) {
    if (st == STOR_SD) { out = "sdmc:/"; return true; }
    if (st == STOR_HB) { out = "sdmc:/switch/"; return true; }
    if (st == STOR_SV) {
        std::string dev;
        if (!saves::mount(sv, dev)) return false;
        out = dev + ":/";
        return true;
    }
    return false;
}

static bool full_path(const Obj &o, std::string &out) {
    std::string b;
    if (!base_path(o.storage, o.save, b)) return false;
    out = b + o.rel;
    return true;
}

static std::string join(const std::string &a, const std::string &b) { return a.empty() ? b : a + "/" + b; }

static std::string obj_name(const Obj &o) {
    if (o.rel.empty()) return o.storage == STOR_SV ? saves::name(o.save) : "";
    auto p = o.rel.rfind('/');
    return p == std::string::npos ? o.rel : o.rel.substr(p + 1);
}

static bool obj_stat(const Obj &o, bool &is_dir, u64 &size, time_t &mtime) {
    std::string p;
    if (!full_path(o, p)) return false;
    if (o.rel.empty()) { is_dir = true; size = 0; mtime = 0; return true; }
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    is_dir = S_ISDIR(st.st_mode);
    size = is_dir ? 0 : (u64)st.st_size;
    mtime = st.st_mtime;
    return true;
}

static void touch_save(const Obj &o) {
    if (o.storage == STOR_SV) saves::commit(o.save);
}

static bool rm_rf(const std::string &p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return unlink(p.c_str()) == 0;
    DIR *d = opendir(p.c_str());
    if (d) {
        while (auto *e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            rm_rf(p + "/" + e->d_name);
        }
        closedir(d);
    }
    return rmdir(p.c_str()) == 0;
}

// Tras renombrar/mover un objeto, actualiza las rutas de sus hijos ya conocidos.
static void reparent_paths(const Obj &o, const std::string &old_rel, const std::string &new_rel) {
    std::string pre = old_rel + "/";
    for (u32 i = 0; i < g_objs.size(); i++) {
        Obj &c = g_objs[i];
        if (c.storage != o.storage || c.save != o.save) continue;
        if (c.rel == old_rel || c.rel.compare(0, pre.size(), pre) == 0) {
            g_keys.erase(key_of(c.storage, c.save, c.rel));
            c.rel = new_rel + c.rel.substr(old_rel.size());
            g_keys[key_of(c.storage, c.save, c.rel)] = i + 1;
        }
    }
}

// ---------------- codificación ----------------
static std::u16string utf8_to_16(const std::string &s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        u32 c = (u8)s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (n == 2) c &= 0x1F; else if (n == 3) c &= 0x0F; else if (n == 4) c &= 0x07;
        for (int k = 1; k < n && i + k < s.size(); k++) c = (c << 6) | ((u8)s[i + k] & 0x3F);
        i += n;
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back((char16_t)(0xD800 + (c >> 10)));
            out.push_back((char16_t)(0xDC00 + (c & 0x3FF)));
        } else out.push_back((char16_t)c);
    }
    return out;
}

static std::string utf16_to_8(const std::u16string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        u32 c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size()) c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

struct W {
    std::vector<u8> v;
    void u8_(u8 x) { v.push_back(x); }
    void u16_(u16 x) { u8_(x & 0xFF); u8_(x >> 8); }
    void u32_(u32 x) { u16_(x & 0xFFFF); u16_(x >> 16); }
    void u64_(u64 x) { u32_((u32)x); u32_((u32)(x >> 32)); }
    void str(const std::string &s) {
        std::u16string u = utf8_to_16(s);
        if (u.empty()) { u8_(0); return; }
        if (u.size() > 254) u.resize(254);
        u8_((u8)(u.size() + 1));
        for (char16_t c : u) u16_(c);
        u16_(0);
    }
    void a16(std::initializer_list<u16> l) { u32_((u32)l.size()); for (u16 x : l) u16_(x); }
    void a32(const std::vector<u32> &l) { u32_((u32)l.size()); for (u32 x : l) u32_(x); }
};

static u16 rd16(const u8 *p) { return p[0] | (p[1] << 8); }
static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

static std::string rd_str(const u8 *p, size_t avail, size_t *used) {
    if (avail < 1) { if (used) *used = 0; return ""; }
    u8 n = p[0];
    size_t need = 1 + (size_t)n * 2;
    if (used) *used = need;
    if (need > avail) return "";
    std::u16string u;
    for (u8 i = 0; i < n; i++) {
        char16_t c = rd16(p + 1 + i * 2);
        if (!c) break;
        u.push_back(c);
    }
    return utf16_to_8(u);
}

static std::string mtp_date(time_t t) {
    if (t == 0) return "";
    char b[32];
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(b, sizeof(b), "%Y%m%dT%H%M%S", &tmv);
    return b;
}

// ---------------- transporte ----------------
struct Cmd {
    u16 code;
    u32 tid;
    u32 p[5];
    int np;
};

static bool send_response(u32 tid, u16 code, std::initializer_list<u32> params = {}) {
    u32 len = 12 + 4 * (u32)params.size();
    u8 *b = g_cmd;
    memset(b, 0, len);
    b[0] = len & 0xFF; b[1] = (len >> 8) & 0xFF;
    b[4] = CT_RESPONSE; b[6] = code & 0xFF; b[7] = code >> 8;
    memcpy(b + 8, &tid, 4);
    int i = 0;
    for (u32 x : params) memcpy(b + 12 + 4 * i++, &x, 4);
    return usb::write(b, len);
}

static void put_header(u8 *b, u64 total, u16 code, u32 tid) {
    u32 len = total > 0xFFFFFFFFULL ? 0xFFFFFFFF : (u32)total;
    memcpy(b, &len, 4);
    b[4] = CT_DATA; b[5] = 0;
    b[6] = code & 0xFF; b[7] = code >> 8;
    memcpy(b + 8, &tid, 4);
}

static bool send_zlp_if_needed(u64 total) {
    if (total % usb::max_packet() == 0) return usb::write(g_buf, 0);
    return true;
}

static bool send_data(const Cmd &c, const std::vector<u8> &payload) {
    u64 total = 12 + payload.size();
    if (total > BUF_SIZE) return false;
    put_header(g_buf, total, c.code, c.tid);
    if (!payload.empty()) memcpy(g_buf + 12, payload.data(), payload.size());
    return usb::write(g_buf, (u32)total) && send_zlp_if_needed(total);
}

// Recibe una fase de datos del PC. sink() recibe los trozos de carga útil.
static bool recv_data(const std::function<bool(const u8 *, size_t)> &sink) {
    u32 got = 0;
    do {
        if (!usb::read(g_buf, BUF_SIZE, &got)) return false;
    } while (got == 0); // ZLP sobrante de la transferencia anterior
    if (got < 12) return false;
    u32 len = rd32(g_buf);
    bool unknown = len == 0xFFFFFFFF;
    u64 recvd = got;
    bool ok = sink(g_buf + 12, got - 12);
    while (unknown ? got == BUF_SIZE : recvd < len) {
        if (!usb::read(g_buf, BUF_SIZE, &got)) return false;
        if (got == 0) break;
        recvd += got;
        if (ok) ok = sink(g_buf, got);
        g_bytes += got;
    }
    return true;
}

// ---------------- operaciones ----------------
static bool op_device_info(const Cmd &c) {
    W w;
    w.u16_(100);
    w.u32_(6);
    w.u16_(100);
    w.str("microsoft.com: 1.0;");
    w.u16_(0);
    w.a16({OP_GetDeviceInfo, OP_OpenSession, OP_CloseSession, OP_GetStorageIDs, OP_GetStorageInfo,
           OP_GetObjectHandles, OP_GetObjectInfo, OP_GetObject, OP_DeleteObject, OP_SendObjectInfo,
           OP_SendObject, OP_GetDevicePropDesc, OP_GetDevicePropValue, OP_MoveObject, OP_GetPartialObject,
           OP_GetObjectPropsSupported, OP_GetObjectPropDesc, OP_GetObjectPropValue, OP_SetObjectPropValue,
           OP_GetObjectReferences});
    w.a16({});
    w.a16({DPROP_FriendlyName});
    w.a16({});
    w.a16({FMT_Undefined, FMT_Association});
    w.str("Nintendo");
    w.str("Switch 2 (USBS2)");
    w.str("1.0");
    w.str("USBS2-0001");
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static bool op_storage_ids(const Cmd &c) {
    W w;
    w.a32({STOR_SD, STOR_HB, STOR_SV});
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static bool op_storage_info(const Cmd &c) {
    u32 st = c.p[0];
    if (!valid_storage(st)) return send_response(c.tid, RC_InvalidStorageID);
    u64 total = 0, free_ = 0;
    struct statvfs vfs;
    if (statvfs("sdmc:/", &vfs) == 0) {
        total = (u64)vfs.f_blocks * vfs.f_frsize;
        free_ = (u64)vfs.f_bavail * vfs.f_frsize;
    }
    if (st == STOR_SV) { total = 1ULL << 30; free_ = 1ULL << 30; } // valor nominal: el límite real lo pone cada partida
    W w;
    w.u16_(st == STOR_SV ? 0x0003 : 0x0004);
    w.u16_(0x0002);
    w.u16_(0x0000);
    w.u64_(total);
    w.u64_(free_);
    w.u32_(0xFFFFFFFF);
    w.str(st == STOR_SD ? "Tarjeta SD" : st == STOR_HB ? "Homebrew (sdmc-switch)" : "Partidas guardadas");
    w.str(st == STOR_SD ? "sdmc" : st == STOR_HB ? "switch" : "saves");
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static void list_children(u32 st, u32 parent_h, std::vector<u32> &out) {
    if (parent_h == ROOT || parent_h == 0) {
        if (st == STOR_SV) {
            for (int i = 0; i < saves::count(); i++) out.push_back(handle_for(STOR_SV, i, "", 0));
            return;
        }
        std::string base;
        if (!base_path(st, -1, base)) return;
        if (DIR *d = opendir(base.c_str())) {
            while (auto *e = readdir(d)) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                out.push_back(handle_for(st, -1, e->d_name, 0));
            }
            closedir(d);
        }
        return;
    }
    Obj *o = get_obj(parent_h);
    if (!o) return;
    Obj parent = *o; // copia: handle_for puede reubicar el vector
    std::string p;
    if (!full_path(parent, p)) return;
    if (DIR *d = opendir(p.c_str())) {
        while (auto *e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            out.push_back(handle_for(parent.storage, parent.save, join(parent.rel, e->d_name), parent_h));
        }
        closedir(d);
    }
}

static bool op_object_handles(const Cmd &c) {
    u32 st = c.p[0], parent = c.np > 2 ? c.p[2] : 0;
    if (st != 0xFFFFFFFF && !valid_storage(st)) return send_response(c.tid, RC_InvalidStorageID);
    if (parent != 0 && parent != ROOT && !get_obj(parent)) return send_response(c.tid, RC_InvalidParentObject);
    std::vector<u32> hs;
    if (st == 0xFFFFFFFF && (parent == 0 || parent == ROOT)) {
        for (u32 s : {STOR_SD, STOR_HB, STOR_SV}) list_children(s, ROOT, hs);
    } else {
        if (st == 0xFFFFFFFF) st = get_obj(parent)->storage;
        list_children(st, parent, hs);
    }
    W w;
    w.a32(hs);
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static bool op_object_info(const Cmd &c) {
    Obj *op = get_obj(c.p[0]);
    if (!op) return send_response(c.tid, RC_InvalidObjectHandle);
    Obj o = *op;
    bool dir; u64 size; time_t mt;
    if (!obj_stat(o, dir, size, mt)) return send_response(c.tid, RC_InvalidObjectHandle);
    W w;
    w.u32_(o.storage);
    w.u16_(dir ? FMT_Association : FMT_Undefined);
    w.u16_(0);
    w.u32_(size > 0xFFFFFFFFULL ? 0xFFFFFFFF : (u32)size);
    w.u16_(0); w.u32_(0); w.u32_(0); w.u32_(0); w.u32_(0); w.u32_(0); w.u32_(0);
    w.u32_(o.parent);
    w.u16_(dir ? 0x0001 : 0x0000);
    w.u32_(0); w.u32_(0);
    w.str(obj_name(o));
    w.str(mtp_date(mt));
    w.str(mtp_date(mt));
    w.str("");
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static bool stream_file(const Cmd &c, const std::string &path, u64 offset, u64 len) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return send_response(c.tid, RC_AccessDenied);
    if (offset) fseeko(f, (off_t)offset, SEEK_SET);
    u64 total = 12 + len;
    put_header(g_buf, total, c.code, c.tid);
    u32 fill = 12;
    u64 left = len;
    bool ok = true;
    set_status("Enviando %s", path.c_str());
    for (;;) {
        u32 want = (u32)std::min<u64>(BUF_SIZE - fill, left);
        size_t n = want ? fread(g_buf + fill, 1, want, f) : 0;
        if (n < want) memset(g_buf + fill + n, 0, want - n); // archivo truncado: rellena para no romper el protocolo
        fill += want;
        left -= want;
        if (fill == 0) break;
        if (!usb::write(g_buf, fill)) { ok = false; break; }
        g_bytes += fill;
        if (left == 0) break;
        fill = 0;
    }
    fclose(f);
    if (!ok) return false;
    if (!send_zlp_if_needed(total)) return false;
    set_status("Conectado al PC");
    if (c.code == OP_GetPartialObject) return send_response(c.tid, RC_OK, {(u32)len});
    return send_response(c.tid, RC_OK);
}

static bool op_get_object(const Cmd &c) {
    Obj *op = get_obj(c.p[0]);
    if (!op) return send_response(c.tid, RC_InvalidObjectHandle);
    Obj o = *op;
    bool dir; u64 size; time_t mt;
    std::string p;
    if (!obj_stat(o, dir, size, mt) || dir || !full_path(o, p)) return send_response(c.tid, RC_InvalidObjectHandle);
    if (c.code == OP_GetPartialObject) {
        u64 off = c.p[1], max = c.p[2];
        if (off > size) off = size;
        return stream_file(c, p, off, std::min<u64>(max, size - off));
    }
    return stream_file(c, p, 0, size);
}

static bool op_delete(const Cmd &c) {
    Obj *op = get_obj(c.p[0]);
    if (!op) return send_response(c.tid, RC_InvalidObjectHandle);
    if (op->rel.empty()) return send_response(c.tid, RC_AccessDenied); // raíz de una partida
    std::string p;
    if (!full_path(*op, p)) return send_response(c.tid, RC_GeneralError);
    bool ok = rm_rf(p);
    touch_save(*op);
    if (ok) op->alive = false;
    return send_response(c.tid, ok ? RC_OK : RC_AccessDenied);
}

// estado entre SendObjectInfo y SendObject
static struct { bool pending = false; u32 handle = 0; std::string path; } g_send;

static bool op_send_object_info(const Cmd &c) {
    u32 st = c.p[0], parent_h = c.np > 1 ? c.p[1] : ROOT;
    std::vector<u8> data;
    if (!recv_data([&](const u8 *p, size_t n) { data.insert(data.end(), p, p + n); return data.size() < 0x10000; }))
        return false;
    g_send.pending = false;
    if (data.size() < 53) return send_response(c.tid, RC_NoValidObjectInfo);

    u16 fmt = rd16(&data[4]);
    std::string name = rd_str(&data[52], data.size() - 52, nullptr);
    if (name.empty() || name.find('/') != std::string::npos) return send_response(c.tid, RC_NoValidObjectInfo);

    Obj parent{st, -1, "", 0, true};
    if (parent_h != ROOT && parent_h != 0) {
        Obj *po = get_obj(parent_h);
        if (!po) return send_response(c.tid, RC_InvalidParentObject);
        parent = *po;
        st = parent.storage;
    } else {
        parent_h = 0;
        if (!valid_storage(st)) return send_response(c.tid, RC_InvalidStorageID);
        if (st == STOR_SV) return send_response(c.tid, RC_AccessDenied); // no se crean partidas nuevas
    }

    std::string pp;
    if (!full_path(parent, pp)) return send_response(c.tid, RC_GeneralError);
    std::string rel = join(parent.rel, name);
    std::string path = (pp.empty() || pp.back() == '/') ? pp + name : pp + "/" + name;

    if (fmt == FMT_Association) {
        if (mkdir(path.c_str(), 0777) != 0 && errno != EEXIST) return send_response(c.tid, RC_AccessDenied);
        touch_save(parent);
    }
    u32 h = handle_for(st, parent.save, rel, parent_h);
    if (fmt != FMT_Association) {
        g_send.pending = true;
        g_send.handle = h;
        g_send.path = path;
    }
    return send_response(c.tid, RC_OK, {st, parent_h == 0 ? ROOT : parent_h, h});
}

static bool op_send_object(const Cmd &c) {
    if (!g_send.pending) {
        if (!recv_data([](const u8 *, size_t) { return false; })) return false;
        return send_response(c.tid, RC_NoValidObjectInfo);
    }
    g_send.pending = false;
    FILE *f = fopen(g_send.path.c_str(), "wb");
    static char *vbuf = (char *)malloc(1 << 20);
    if (f && vbuf) setvbuf(f, vbuf, _IOFBF, 1 << 20);
    bool werr = !f;
    set_status("Recibiendo %s", g_send.path.c_str());
    bool ok = recv_data([&](const u8 *p, size_t n) {
        if (werr) return false;
        if (n && fwrite(p, 1, n, f) != n) werr = true;
        return !werr;
    });
    if (f) fclose(f);
    Obj *o = get_obj(g_send.handle);
    if (o) touch_save(*o);
    if (!ok) return false;
    set_status("Conectado al PC");
    return send_response(c.tid, werr ? RC_StoreFull : RC_OK);
}

static bool op_move(const Cmd &c) {
    Obj *op = get_obj(c.p[0]);
    if (!op) return send_response(c.tid, RC_InvalidObjectHandle);
    if (op->rel.empty()) return send_response(c.tid, RC_AccessDenied);
    u32 dst_st = c.p[1], dst_parent = c.p[2];
    Obj dst{dst_st, -1, "", 0, true};
    if (dst_parent && dst_parent != ROOT) {
        Obj *d = get_obj(dst_parent);
        if (!d) return send_response(c.tid, RC_InvalidParentObject);
        dst = *d;
    } else {
        dst_parent = 0;
        if (dst_st == STOR_SV) return send_response(c.tid, RC_AccessDenied);
    }
    // solo dentro de la misma unidad (y de la misma partida)
    if (dst.storage != op->storage || dst.save != op->save) return send_response(c.tid, RC_OperationNotSupported);
    std::string from, base;
    if (!full_path(*op, from) || !base_path(op->storage, op->save, base)) return send_response(c.tid, RC_GeneralError);
    std::string new_rel = join(dst.rel, obj_name(*op));
    std::string old_rel = op->rel;
    if (rename(from.c_str(), (base + new_rel).c_str()) != 0) return send_response(c.tid, RC_AccessDenied);
    Obj copy = *op;
    reparent_paths(copy, old_rel, new_rel);
    if (Obj *o2 = get_obj(c.p[0])) o2->parent = dst_parent;
    touch_save(copy);
    return send_response(c.tid, RC_OK);
}

static void prop_desc(W &w, u16 prop) {
    w.u16_(prop);
    switch (prop) {
    case PROP_StorageID: case PROP_ParentObject: w.u16_(DT_UINT32); w.u8_(0); w.u32_(0); break;
    case PROP_ObjectFormat: case PROP_ProtectionStatus: w.u16_(DT_UINT16); w.u8_(0); w.u16_(0); break;
    case PROP_ObjectSize: w.u16_(DT_UINT64); w.u8_(0); w.u64_(0); break;
    case PROP_PersistentUID: w.u16_(DT_UINT128); w.u8_(0); w.u64_(0); w.u64_(0); break;
    case PROP_ObjectFileName: w.u16_(DT_STR); w.u8_(1); w.str(""); break;
    default: w.u16_(DT_STR); w.u8_(0); w.str(""); break;
    }
    w.u32_(0);
    w.u8_(0);
}

static bool known_prop(u16 p) {
    switch (p) {
    case PROP_StorageID: case PROP_ObjectFormat: case PROP_ProtectionStatus: case PROP_ObjectSize:
    case PROP_ObjectFileName: case PROP_DateModified: case PROP_ParentObject: case PROP_PersistentUID:
    case PROP_Name: return true;
    }
    return false;
}

static bool op_props_supported(const Cmd &c) {
    W w;
    w.a16({PROP_StorageID, PROP_ObjectFormat, PROP_ProtectionStatus, PROP_ObjectSize, PROP_ObjectFileName,
           PROP_DateModified, PROP_ParentObject, PROP_PersistentUID, PROP_Name});
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static bool op_prop_desc(const Cmd &c) {
    if (!known_prop((u16)c.p[0])) return send_response(c.tid, RC_InvalidObjectPropCode);
    W w;
    prop_desc(w, (u16)c.p[0]);
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static bool op_prop_value(const Cmd &c) {
    u32 h = c.p[0];
    Obj *op = get_obj(h);
    if (!op) return send_response(c.tid, RC_InvalidObjectHandle);
    Obj o = *op;
    u16 prop = (u16)c.p[1];
    if (!known_prop(prop)) return send_response(c.tid, RC_InvalidObjectPropCode);
    bool dir; u64 size; time_t mt;
    if (!obj_stat(o, dir, size, mt)) return send_response(c.tid, RC_InvalidObjectHandle);
    W w;
    switch (prop) {
    case PROP_StorageID: w.u32_(o.storage); break;
    case PROP_ObjectFormat: w.u16_(dir ? FMT_Association : FMT_Undefined); break;
    case PROP_ProtectionStatus: w.u16_(0); break;
    case PROP_ObjectSize: w.u64_(size); break;
    case PROP_ObjectFileName: case PROP_Name: w.str(obj_name(o)); break;
    case PROP_DateModified: w.str(mtp_date(mt)); break;
    case PROP_ParentObject: w.u32_(o.parent); break;
    case PROP_PersistentUID: w.u64_(h); w.u64_(o.storage); break;
    }
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static bool op_set_prop_value(const Cmd &c) {
    std::vector<u8> data;
    if (!recv_data([&](const u8 *p, size_t n) { data.insert(data.end(), p, p + n); return data.size() < 0x1000; }))
        return false;
    Obj *op = get_obj(c.p[0]);
    if (!op) return send_response(c.tid, RC_InvalidObjectHandle);
    if ((u16)c.p[1] != PROP_ObjectFileName) return send_response(c.tid, RC_AccessDenied);
    if (op->rel.empty()) return send_response(c.tid, RC_AccessDenied);
    std::string name = data.empty() ? "" : rd_str(data.data(), data.size(), nullptr);
    if (name.empty() || name.find('/') != std::string::npos) return send_response(c.tid, RC_GeneralError);
    std::string from, base;
    if (!full_path(*op, from) || !base_path(op->storage, op->save, base)) return send_response(c.tid, RC_GeneralError);
    std::string old_rel = op->rel;
    auto slash = old_rel.rfind('/');
    std::string new_rel = slash == std::string::npos ? name : old_rel.substr(0, slash + 1) + name;
    if (rename(from.c_str(), (base + new_rel).c_str()) != 0) return send_response(c.tid, RC_AccessDenied);
    Obj copy = *op;
    reparent_paths(copy, old_rel, new_rel);
    touch_save(copy);
    return send_response(c.tid, RC_OK);
}

static bool op_dev_prop(const Cmd &c) {
    if ((u16)c.p[0] != DPROP_FriendlyName) return send_response(c.tid, RC_DevicePropNotSupported);
    W w;
    if (c.code == OP_GetDevicePropDesc) {
        w.u16_(DPROP_FriendlyName);
        w.u16_(DT_STR);
        w.u8_(0);
        w.str("Switch 2");
        w.str("Switch 2");
        w.u8_(0);
    } else {
        w.str("Switch 2");
    }
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static bool op_refs(const Cmd &c) {
    W w;
    w.a32({});
    return send_data(c, w.v) && send_response(c.tid, RC_OK);
}

static bool dispatch(const Cmd &c) {
    if (c.code == OP_GetDeviceInfo) return op_device_info(c);
    if (c.code == OP_OpenSession) {
        if (g_session) return send_response(c.tid, RC_SessionAlreadyOpen);
        g_session = true;
        reset_objects();
        saves::scan();
        set_status("Conectado al PC (%d partidas)", saves::count());
        return send_response(c.tid, RC_OK);
    }
    if (!g_session) return send_response(c.tid, RC_SessionNotOpen);
    switch (c.code) {
    case OP_CloseSession:
        g_session = false;
        saves::unmount_all();
        set_status("Sesión cerrada");
        return send_response(c.tid, RC_OK);
    case OP_GetStorageIDs: return op_storage_ids(c);
    case OP_GetStorageInfo: return op_storage_info(c);
    case OP_GetObjectHandles: return op_object_handles(c);
    case OP_GetObjectInfo: return op_object_info(c);
    case OP_GetObject: case OP_GetPartialObject: return op_get_object(c);
    case OP_DeleteObject: return op_delete(c);
    case OP_SendObjectInfo: return op_send_object_info(c);
    case OP_SendObject: return op_send_object(c);
    case OP_MoveObject: return op_move(c);
    case OP_GetObjectPropsSupported: return op_props_supported(c);
    case OP_GetObjectPropDesc: return op_prop_desc(c);
    case OP_GetObjectPropValue: return op_prop_value(c);
    case OP_SetObjectPropValue: return op_set_prop_value(c);
    case OP_GetDevicePropDesc: case OP_GetDevicePropValue: return op_dev_prop(c);
    case OP_GetObjectReferences: return op_refs(c);
    default: return send_response(c.tid, RC_OperationNotSupported);
    }
}

static void session_loop() {
    while (!g_quit->load()) {
        u32 got = 0;
        if (!usb::read(g_cmd, 0x1000, &got)) return;
        if (got == 0) continue;
        if (got < 12 || rd16(g_cmd + 4) != CT_COMMAND) continue;
        Cmd c{};
        c.code = rd16(g_cmd + 6);
        c.tid = rd32(g_cmd + 8);
        c.np = std::min<int>((int)(got - 12) / 4, 5);
        for (int i = 0; i < c.np; i++) c.p[i] = rd32(g_cmd + 12 + 4 * i);
        if (!dispatch(c)) return;
    }
}

static void thread_main(void *) {
    while (!g_quit->load()) {
        set_status("Esperando cable USB...");
        if (!usb::wait_ready()) break;
        set_status("Cable conectado, esperando al PC...");
        session_loop();
        g_session = false;
        g_send.pending = false;
        saves::unmount_all();
    }
}

static Thread g_thread;

Result start(std::atomic<bool> *quit) {
    g_quit = quit;
    mutexInit(&g_status_mtx);
    g_buf = (u8 *)memalign(0x1000, BUF_SIZE);
    g_cmd = (u8 *)memalign(0x1000, 0x1000);
    if (!g_buf || !g_cmd) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    mkdir("sdmc:/switch", 0777);
    Result rc = usb::init(quit);
    if (R_FAILED(rc)) return rc;
    rc = threadCreate(&g_thread, thread_main, nullptr, nullptr, 0x20000, 0x2C, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_thread);
    return rc;
}

void stop() {
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    usb::exit();
    free(g_buf);
    free(g_cmd);
}

} // namespace mtp
