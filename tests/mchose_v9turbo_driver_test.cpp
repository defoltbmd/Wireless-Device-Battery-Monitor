// Exercise the production MCHOSE V9 Turbo family headset driver (3837:6009 / 600A / 600D dongle,
// bare "65 01" frame) with simulated replies; no hardware access.
#include "../src/drivers/drivers.h"

#include <algorithm>
#include <cassert>
#include <cstdio>

namespace {
std::vector<std::vector<BYTE>> pending;
int opens = 0;
int writes = 0;

HidColl Collection(USHORT pid = 0x600A, USHORT page = 0xFF90, USHORT usage = 0x0001, USHORT outLen = 65,
                   USHORT inLen = 65) {
    HidColl c;
    c.path = L"mchose-v9turbo";
    c.vid = 0x3837;
    c.pid = pid;
    c.iface = 0;
    c.caps.UsagePage = page;
    c.caps.Usage = usage;
    c.caps.InputReportByteLength = inLen;
    c.caps.OutputReportByteLength = outLen;
    return c;
}

// Battery frame laid out the way the headset sends it: report id, command, level, state.
std::vector<BYTE> Frame(BYTE level, BYTE state) {
    std::vector<BYTE> r(64, 0);
    r[0] = 0x55;
    r[1] = 0x65;
    r[2] = level;
    r[3] = state;
    return r;
}

PollContext Poll(std::vector<std::vector<BYTE>> replies, std::vector<HidColl> colls) {
    pending = std::move(replies);
    writes = 0;
    PollContext ctx;
    ctx.colls = std::move(colls);
    PollMchoseV9Turbo(ctx);
    return ctx;
}

PollContext Poll(std::vector<std::vector<BYTE>> replies, const HidColl& c = Collection()) {
    return Poll(std::move(replies), std::vector<HidColl>{ c });
}
}  // namespace

// Stand-ins for the Win32 HID transport. The production driver is linked unchanged.
bool HidDev::Open(const HidColl& c) {
    c_ = c;
#ifdef _WIN32
    h_ = reinterpret_cast<HANDLE>(1);
#else
    fd_ = 1;
#endif
    ++opens;
    return true;
}
void HidDev::Close() {
#ifdef _WIN32
    h_ = INVALID_HANDLE_VALUE;
#else
    fd_ = -1;
#endif
}
// The scripted replies stand for what the dongle will answer next, so there is nothing stale to drop.
void HidDev::Flush() {}
bool HidDev::Write(const std::vector<BYTE>& data, int) {
    // The only frame this driver may send: the read-only battery query, zero padded to the 64-byte
    // report by the transport (the driver passes the three meaningful bytes).
    const std::vector<BYTE> expected = { 0x55, 0x65, 0x01 };
    assert(data.size() == expected.size());
    assert(std::equal(expected.begin(), expected.end(), data.begin()));
    ++writes;
    return true;
}
int HidDev::Read(std::vector<BYTE>& out, int timeoutMs) {
    assert(timeoutMs > 0 && timeoutMs <= 1000);
    if (pending.empty()) return -1;
    out = pending.front();
    pending.erase(pending.begin());
    return (int)out.size();
}
std::wstring HidProduct(const HidColl& c) { return c.pid == 0x600A ? L"MCHOSE V9 Turbo+" : L"MCHOSE V9 Turbo"; }
std::wstring Hex4(USHORT v) {
    wchar_t text[8];
    swprintf(text, 8, L"%04X", v);
    return text;
}
int Clamp100(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

int main() {
    // Discharging, as a real V9 Turbo+ dongle answers on FF90:0001.
    auto ctx = Poll({ Frame(64, 2) });
    assert(ctx.out.size() == 1 && ctx.IsClaimed(0x3837, 0x600A));
    {
        const auto& s = ctx.out[0];
        assert(s.percent == 64 && s.connected && !s.charging && !s.full && !s.asleep);
        assert(s.name == L"MCHOSE V9 Turbo+" && s.key == L"mchose:600A");
        assert(s.kind == DeviceKind::Headset && s.via == L"2.4 GHz receiver" && s.graceSec == 1800);
        assert(writes == 1);  // one reply is enough, no retries
    }

    // Charging.
    ctx = Poll({ Frame(88, 3) });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 88 && ctx.out[0].charging && !ctx.out[0].full);

    // Fully charged.
    ctx = Poll({ Frame(100, 4) });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 100 && ctx.out[0].full && !ctx.out[0].charging);

    // Headset switched off or asleep: the dongle still answers, so the entry stays and keeps the last
    // value instead of showing a bogus 0%.
    ctx = Poll({ Frame(0, 26) });
    assert(ctx.out.size() == 1 && ctx.out[0].asleep && ctx.out[0].percent == -1);

    // The second vendor collection (FF82:0001) answers the same query.
    ctx = Poll({ Frame(37, 2) }, Collection(0x600A, 0xFF82, 0x0001));
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 37);

    // Unrelated traffic on the same collection is ignored, and the answer is taken within the deadline.
    ctx = Poll({ std::vector<BYTE>(64, 0), Frame(12, 2) });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 12 && writes == 1);

    // An unsolicited push of the same frame counts too (no fresh answer needed).
    ctx = Poll({ Frame(51, 3) });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 51 && ctx.out[0].charging);

    // Nothing answers (headset off, receiver idle): nothing is reported and the id stays unclaimed.
    ctx = Poll({});
    assert(ctx.out.empty() && !ctx.IsClaimed(0x3837, 0x600A));

    // FF90:0001 and FF82:0001 only: the FF21:0001 interface and the feature-only FF22:0001 collection
    // are not the battery protocol.
    ctx = Poll({ Frame(40, 2) }, Collection(0x600A, 0xFF21, 0x0001, 2, 2));
    assert(ctx.out.empty() && !ctx.IsClaimed(0x3837, 0x600A));
    ctx = Poll({ Frame(40, 2) }, Collection(0x600A, 0xFF22, 0x0001));
    assert(ctx.out.empty() && !ctx.IsClaimed(0x3837, 0x600A));

    // The rest of the family uses the same frame: V9 Turbo Headset and V9 Turbo get their own keys, so
    // two headsets do not overwrite each other's history or settings.
    ctx = Poll({ Frame(30, 2) }, Collection(0x6009));
    assert(ctx.out.size() == 1 && ctx.out[0].key == L"mchose:6009" && ctx.IsClaimed(0x3837, 0x6009));
    ctx = Poll({ Frame(70, 2) }, Collection(0x600D));
    assert(ctx.out.size() == 1 && ctx.out[0].key == L"mchose:600D" && ctx.out[0].percent == 70);

    // Both dongles present at once: one entry each, and the second one is not skipped.
    ctx = Poll({ Frame(30, 2), Frame(70, 2) }, std::vector<HidColl>{ Collection(0x6009), Collection(0x600D) });
    assert(ctx.out.size() == 2 && ctx.out[0].key == L"mchose:6009" && ctx.out[1].key == L"mchose:600D");

    // A different product on the same vendor id (the 3837:6008 dock, a 3837 mouse) is left alone.
    ctx = Poll({ Frame(40, 2) }, Collection(0x6008));
    assert(ctx.out.empty() && !ctx.IsClaimed(0x3837, 0x6008));

    // A level above 100 is rejected as a malformed frame.
    ctx = Poll({ Frame(120, 2) });
    assert(ctx.out.empty());

    puts("MCHOSE V9 Turbo driver tests passed");
    return 0;
}
