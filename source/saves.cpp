// saves.cpp - Lista y monta partidas guardadas bajo demanda (sv0:/ ... sv7:/).
#include "saves.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace saves {

static constexpr int MAX_MOUNTS = 8;

struct Entry {
    FsSaveDataAttribute attr{};
    FsSaveDataSpaceId space = FsSaveDataSpaceId_User;
    std::string name;
    int slot = -1;
};

static std::vector<Entry> g_entries;
static int g_slot_owner[MAX_MOUNTS];
static u64 g_slot_use[MAX_MOUNTS];
static u64 g_tick = 0;
static bool g_ns = false, g_acc = false;

static std::string sanitize(std::string s) {
    for (auto &c : s)
        if (strchr("\\/:*?\"<>|", c) || (unsigned char)c < 0x20) c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? "_" : s;
}

static std::string game_name(u64 tid) {
    static std::map<u64, std::string> cache;
    auto it = cache.find(tid);
    if (it != cache.end()) return it->second;

    char hex[17];
    snprintf(hex, sizeof(hex), "%016lX", tid);
    std::string name = hex;
    if (g_ns) {
        static NsApplicationControlData *ctrl = nullptr;
        if (!ctrl) ctrl = new NsApplicationControlData;
        u64 size = 0;
        if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, tid, ctrl, sizeof(*ctrl), &size))) {
            NacpLanguageEntry *lang = nullptr;
            if (R_SUCCEEDED(nacpGetLanguageEntry(&ctrl->nacp, &lang)) && lang && lang->name[0])
                name = lang->name;
        }
    }
    return cache[tid] = sanitize(name);
}

static std::string user_name(AccountUid uid) {
    char fallback[24];
    snprintf(fallback, sizeof(fallback), "usuario %08X", (u32)uid.uid[0]);
    if (!g_acc) return fallback;
    AccountProfile prof;
    if (R_FAILED(accountGetProfile(&prof, uid))) return fallback;
    AccountProfileBase base{};
    std::string out = fallback;
    if (R_SUCCEEDED(accountProfileGet(&prof, nullptr, &base)) && base.nickname[0]) out = base.nickname;
    accountProfileClose(&prof);
    return sanitize(out);
}

void init() {
    g_ns = R_SUCCEEDED(nsInitialize());
    g_acc = R_SUCCEEDED(accountInitialize(AccountServiceType_Application));
    for (int i = 0; i < MAX_MOUNTS; i++) g_slot_owner[i] = -1;
}

void exit() {
    unmount_all();
    if (g_acc) accountExit();
    if (g_ns) nsExit();
}

void unmount_all() {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (g_slot_owner[i] < 0) continue;
        char dev[16];
        snprintf(dev, sizeof(dev), "sv%d", i);
        fsdevCommitDevice(dev);
        fsdevUnmountDevice(dev);
        g_entries[g_slot_owner[i]].slot = -1;
        g_slot_owner[i] = -1;
    }
}

void scan() {
    unmount_all();
    g_entries.clear();
    std::set<std::string> used;

    FsSaveDataInfoReader reader;
    if (R_FAILED(fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User))) return;
    FsSaveDataInfo info;
    s64 got = 0;
    while (R_SUCCEEDED(fsSaveDataInfoReaderRead(&reader, &info, 1, &got)) && got == 1) {
        if (info.application_id == 0) continue;
        if (info.save_data_type != FsSaveDataType_Account && info.save_data_type != FsSaveDataType_Device) continue;

        Entry e;
        e.space = (FsSaveDataSpaceId)info.save_data_space_id;
        e.attr.application_id = info.application_id;
        e.attr.uid = info.uid;
        e.attr.save_data_type = info.save_data_type;
        e.attr.save_data_rank = info.save_data_rank;
        e.attr.save_data_index = info.save_data_index;

        std::string n = game_name(info.application_id);
        n += info.save_data_type == FsSaveDataType_Account ? " - " + user_name(info.uid) : " (dispositivo)";
        if (used.count(n)) {
            char sfx[24];
            snprintf(sfx, sizeof(sfx), " [%016lX]", info.save_data_id);
            n += sfx;
        }
        used.insert(n);
        e.name = n;
        g_entries.push_back(e);
    }
    fsSaveDataInfoReaderClose(&reader);
}

int count() { return (int)g_entries.size(); }

const std::string &name(int idx) { return g_entries[idx].name; }

bool mount(int idx, std::string &dev_out) {
    if (idx < 0 || idx >= (int)g_entries.size()) return false;
    Entry &e = g_entries[idx];
    g_tick++;
    if (e.slot >= 0) {
        g_slot_use[e.slot] = g_tick;
        dev_out = "sv" + std::to_string(e.slot);
        return true;
    }
    // slot libre o el menos usado recientemente
    int slot = -1;
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (g_slot_owner[i] < 0) { slot = i; break; }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < MAX_MOUNTS; i++)
            if (g_slot_use[i] < g_slot_use[slot]) slot = i;
        char dev[16];
        snprintf(dev, sizeof(dev), "sv%d", slot);
        fsdevCommitDevice(dev);
        fsdevUnmountDevice(dev);
        g_entries[g_slot_owner[slot]].slot = -1;
        g_slot_owner[slot] = -1;
    }

    FsFileSystem fs;
    if (R_FAILED(fsOpenSaveDataFileSystem(&fs, e.space, &e.attr))) return false;
    char dev[16];
    snprintf(dev, sizeof(dev), "sv%d", slot);
    if (fsdevMountDevice(dev, fs) < 0) {
        fsFsClose(&fs);
        return false;
    }
    e.slot = slot;
    g_slot_owner[slot] = idx;
    g_slot_use[slot] = g_tick;
    dev_out = dev;
    return true;
}

void commit(int idx) {
    if (idx < 0 || idx >= (int)g_entries.size() || g_entries[idx].slot < 0) return;
    char dev[16];
    snprintf(dev, sizeof(dev), "sv%d", g_entries[idx].slot);
    fsdevCommitDevice(dev);
}

} // namespace saves
