// MCHOSE V9 Turbo family headsets on their 2.4 GHz dongle (VID 3837): V9 Turbo+ (0x600A, tested),
// V9 Turbo Headset (0x6009) and V9 Turbo (0x600D), all untested except the Turbo+.
//
// These are the "bare" profile from M HUB: report id 0x55 with the command frame starting at byte 1,
// without the MC header (0xAA start, version, length, checksum) that the V9 Pro 2 (3837:6011) and the
// R9 (3837:6013) use, so they cannot share PollMchoseHeadset. The framing is the one the
// first-generation 291D:385D dongles use (see mchose_v9pro.cpp), on a different vendor id and
// collection:
//   request: [55][65][01][00 x61]  (read-only battery query, 64 bytes with the report id)
//   reply:   [55][65][level][state] with state 2 = discharging, 3 = charging, 4 = full,
//            26 (0x1A) = headset off or asleep. The headset also pushes the same frame shape
//            (report id 0x55, byte 1 = 0x65) as an unsolicited battery notification.
// Protocol from MCHOSE's own M HUB web driver (cdn.mchose.com.cn customPage bundle,
// audio.v9Turbo.readBattery, parseBatteryResponse), which registers this profile for the family keys
// NAME$$$VID$$$PID-decimal: V9 TURBO+ = 3837:600A, V9 TURBO HEADSET = 3837:6009, V9 TURBO =
// 3837:600D. Reported on a real V9 Turbo+ dongle: the headset answers on both the FF90:0001 and the
// FF82:0001 collection (64-byte input and output reports), charging shown as state 3.
// The dock is a separate product (3837:6008, "V9 Turbo" in the product string) and is not matched here.
#include "drivers.h"

namespace {

constexpr USHORT kVid = 0x3837;
constexpr USHORT kPidV9TurboPlus = 0x600A;
constexpr USHORT kPidV9TurboHeadset = 0x6009;
constexpr USHORT kPidV9Turbo = 0x600D;
constexpr BYTE kReportId = 0x55;
constexpr BYTE kReadBattery = 0x65;
constexpr BYTE kStateDischarge = 2;
constexpr BYTE kStateCharging = 3;
constexpr BYTE kStateFull = 4;
constexpr BYTE kStateSleep = 26;

enum class Reply { None, Off, Level };

// True for the vendor collections that answer the battery query: FF90:0001 and FF82:0001, both with
// 64-byte input and output reports. The interface's FF21:0001 (2-byte reports) and the 64-byte
// feature-only FF22:0001 collection do not carry it.
bool IsBatteryCollection(const HidColl& c) {
    return (c.page() == 0xFF90 || c.page() == 0xFF82) && c.usage() == 0x0001 && c.outLen() >= 64 &&
           c.inLen() >= 64;
}

// One input report (r[0] = report ID) carrying the battery frame, either the reply or a pushed one.
Reply ParseBattery(const std::vector<BYTE>& r, int& percent, bool& charging, bool& full) {
    if (r.size() < 4 || r[0] != kReportId || r[1] != kReadBattery) return Reply::None;
    const BYTE state = r[3];
    if (state == kStateSleep) return Reply::Off;
    percent = r[2];
    charging = state == kStateCharging;
    full = state == kStateFull;
    return percent <= 100 ? Reply::Level : Reply::None;
}

Reply QueryBattery(const HidColl& c, int& percent, bool& charging, bool& full) {
    HidDev d;
    if (!d.Open(c)) return Reply::None;
    d.Flush();
    if (!d.Write({kReportId, kReadBattery, 0x01})) return Reply::None;  // zero-padded to 64 bytes

    std::vector<BYTE> r;
    ULONGLONG end = GetTickCount64() + 1000;
    while ((LONGLONG)end - (LONGLONG)GetTickCount64() > 0) {
        int left = (int)((LONGLONG)end - (LONGLONG)GetTickCount64());
        if (d.Read(r, left) < 0) break;
        Reply got = ParseBattery(r, percent, charging, full);
        if (got != Reply::None) return got;
    }
    return Reply::None;
}

}  // namespace

void PollMchoseV9Turbo(PollContext& ctx) {
    for (USHORT pid : {kPidV9TurboPlus, kPidV9TurboHeadset, kPidV9Turbo}) {
        if (ctx.IsClaimed(kVid, pid)) continue;
        for (const HidColl* c : ctx.Find(kVid, pid, IsBatteryCollection)) {
            int pct = -1;
            bool chg = false, full = false;
            Reply got = QueryBattery(*c, pct, chg, full);
            if (got == Reply::None) continue;

            DeviceStatus s;
            s.key = L"mchose:" + Hex4(pid);
            s.name = HidProduct(*c);
            if (s.name.empty()) s.name = pid == kPidV9TurboPlus ? L"MCHOSE V9 Turbo+" : L"MCHOSE V9 Turbo";
            s.kind = DeviceKind::Headset;
            s.via = L"2.4 GHz receiver";
            if (got == Reply::Off) {
                s.asleep = true;  // the dongle answered, the headset is off or asleep: keep the last value
            } else {
                s.percent = pct;
                s.charging = chg;
                s.full = full;
            }
            s.graceSec = 1800;
            ctx.Add(s, kVid, pid);
            break;
        }
    }
}
