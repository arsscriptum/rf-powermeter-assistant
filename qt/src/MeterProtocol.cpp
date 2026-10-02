// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   MeterProtocol.cpp                                                            ║
// ╚════════════════════════════════════════════════════════════════════════════════╝

#include "MeterProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rfpm {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
int digit(char c) { return c - '0'; }

// Sample period for each K command. Duplicates are real (the vendor app maps several of its
// timebase positions to the same hardware rate). Index = K, [0] unused.
constexpr double kPeriods[] = {
    0,
    2e-6,    4e-6,    8e-6,    16e-6,   32e-6,      // K01..K05
    64e-6,   128e-6,  256e-6,  256e-6,  512e-6,     // K06..K10
    512e-6,  1024e-6, 2048e-6, 4096e-6, 4096e-6,    // K11..K15
    8192e-6, 8192e-6, 16384e-6                      // K16..K18
};

// Sustained wire rate measured live per K (records/s).
constexpr int kWireRates[] = {
    0,
    3170, 3170, 3100, 3030, 2880, 2630, 2280, 1730, 1760,
    1160, 1210, 730,  340,  150,  160,  160,  160,  160
};

std::string fixed(double v, int decimals)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

std::string trimZeros(std::string s)
{
    if (s.find('.') == std::string::npos) return s;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

}  // namespace

// ─── parser ─────────────────────────────────────────────────────────────────────

void StreamParser::reset()
{
    token_.clear();
    block_.clear();
    records_ = 0;
    malformed_ = 0;
}

void StreamParser::feed(const char* data, std::size_t size, ParseResult& out)
{
    for (std::size_t i = 0; i < size; ++i) {
        const char c = data[i];
        if (c == 'u' || c == 'm' || c == 'w') {
            completeToken(c, out);
            continue;
        }
        // The 'a' of the "Aa" block marker closes the current sweep. Close it as soon as the
        // byte arrives, not when the next record completes: at slow timebases the next record
        // is a whole sweep (up to 8 s) away. Record chars are only sign + digits, so any 'a'
        // is the marker.
        if (c == 'a') closeBlock(out);
        if (token_.size() >= 64) token_.erase(0, 32);   // runaway garbage: keep the tail
        token_.push_back(c);
    }
}

void StreamParser::closeBlock(ParseResult& out)
{
    if (block_.empty()) return;
    out.blocks.push_back(std::move(block_));
    block_ = std::vector<float>();
    block_.reserve(kSamplesPerBlock + 16);
}

void StreamParser::completeToken(char unit, ParseResult& out)
{
    const std::string& t = token_;
    const std::size_t n = t.size();

    // Settings reply, e.g. "AR5880+00.0a-58600000": extract it wherever it sits.
    if (n > 9) {
        for (std::size_t i = 0; i + 10 <= n; ++i) {
            if (t[i] != 'R') continue;
            if (!isDigit(t[i + 1]) || !isDigit(t[i + 2]) || !isDigit(t[i + 3]) || !isDigit(t[i + 4])) continue;
            const char sign = t[i + 5];
            if (sign != '+' && sign != '-') continue;
            if (!isDigit(t[i + 6]) || !isDigit(t[i + 7]) || t[i + 8] != '.' || !isDigit(t[i + 9])) continue;
            SettingsReply reply;
            reply.frequencyMhz = digit(t[i + 1]) * 1000 + digit(t[i + 2]) * 100 + digit(t[i + 3]) * 10 + digit(t[i + 4]);
            reply.offsetDb = (digit(t[i + 6]) * 10 + digit(t[i + 7]) + digit(t[i + 9]) * 0.1) * (sign == '-' ? -1 : 1);
            reply.offsetDb = std::round(reply.offsetDb * 10.0) / 10.0;
            reply.raw = t.substr(i, 10);
            out.settings.push_back(std::move(reply));
            break;
        }
    }

    // The record is the last 9 chars: sign + 3 dBm digits (+-DD.d) + 5 watts digits.
    // The watts field is ignored: watts are recomputed from dBm, which keeps resolution at
    // low levels where the meter prints 000.00.
    bool ok = false;
    if (n >= 9) {
        const std::size_t s = n - 9;
        const char sign = t[s];
        if (sign == '+' || sign == '-') {
            ok = true;
            for (std::size_t i = 1; i <= 8 && ok; ++i) ok = isDigit(t[s + i]);
            if (ok) {
                const double v = (digit(t[s + 1]) * 10 + digit(t[s + 2]) + digit(t[s + 3]) * 0.1) * (sign == '-' ? -1 : 1);
                const float dbm = static_cast<float>(std::round(v * 10.0) / 10.0);
                out.samples.push_back(dbm);
                block_.push_back(dbm);
                ++records_;
                out.lastRecord.assign(t, s, 9);
                out.lastRecord.push_back(unit);
            }
        }
    }
    if (!ok) ++malformed_;

    // A sweep never exceeds 500 samples; if a marker got lost, don't let the block grow forever.
    if (block_.size() > 2 * kSamplesPerBlock) block_.clear();

    token_.clear();
}

// ─── timebase ───────────────────────────────────────────────────────────────────

double samplePeriod(int k)
{
    return (k >= kMinK && k <= kMaxK) ? kPeriods[k] : 0.0;
}

double sweepDuration(int k)
{
    return samplePeriod(k) * kSamplesPerBlock;
}

int nominalWireRate(int k)
{
    return (k >= kMinK && k <= kMaxK) ? kWireRates[k] : 0;
}

const std::vector<Timebase>& timebases()
{
    static const std::vector<Timebase> table = [] {
        const struct { double window; int k; } windows[] = {
            {100e-6, 1}, {200e-6, 1}, {500e-6, 1}, {1e-3, 1},   {2e-3, 2},    {4e-3, 3},
            {8e-3, 4},   {16e-3, 5},  {32e-3, 6},  {64e-3, 7},  {128e-3, 8},  {256e-3, 10},
            {512e-3, 12}, {1.024, 13}, {2.048, 14}, {4.096, 16}, {8.192, 18},
        };
        std::vector<Timebase> list;
        for (const auto& w : windows) {
            Timebase tb;
            tb.windowSec = w.window;
            tb.k = w.k;
            tb.periodSec = samplePeriod(w.k);
            tb.samples = static_cast<int>(std::min<double>(kSamplesPerBlock, std::round(w.window / tb.periodSec)));
            tb.label = formatTime(w.window) + " \xC2\xB7 " + formatSi(1.0 / tb.periodSec, "Sa/s", 1);
            list.push_back(tb);
        }
        return list;
    }();
    return table;
}

int defaultTimebaseIndex()
{
    return 3;
}

// ─── commands ───────────────────────────────────────────────────────────────────

std::string readCommand()
{
    return "Read\r\n";
}

std::optional<std::string> sampleRateCommand(int k)
{
    if (k < kMinK || k > kMaxK) return std::nullopt;
    char buf[16];
    std::snprintf(buf, sizeof buf, "K%02d\r\n", k);
    return std::string(buf);
}

std::optional<std::string> setCommand(int frequencyMhz, double offsetDb)
{
    if (frequencyMhz < kMinFrequencyMhz || frequencyMhz > kMaxFrequencyMhz) return std::nullopt;
    if (std::isnan(offsetDb) || std::fabs(offsetDb) > kMaxOffsetDb + 1e-9) return std::nullopt;
    char buf[32];
    std::snprintf(buf, sizeof buf, "A%04d%s\r\n", frequencyMhz, formatOffset(offsetDb).c_str());
    std::string cmd(buf);
    if (!isSafeCommand(cmd)) return std::nullopt;
    return cmd;
}

bool isSafeCommand(const std::string& c)
{
    if (c == "Read\r\n") return true;
    // K01..K18
    if (c.size() == 5 && c[0] == 'K' && isDigit(c[1]) && isDigit(c[2]) && c[3] == '\r' && c[4] == '\n') {
        const int k = digit(c[1]) * 10 + digit(c[2]);
        return k >= kMinK && k <= kMaxK;
    }
    // A####+##.# only: the short "A####" form corrupts the meter
    if (c.size() == 12 && c[0] == 'A' && isDigit(c[1]) && isDigit(c[2]) && isDigit(c[3]) && isDigit(c[4])
        && (c[5] == '+' || c[5] == '-') && isDigit(c[6]) && isDigit(c[7]) && c[8] == '.' && isDigit(c[9])
        && c[10] == '\r' && c[11] == '\n') {
        return true;
    }
    return false;
}

// ─── units and formatting ───────────────────────────────────────────────────────

double dbmToWatts(double dbm)
{
    return std::pow(10.0, (dbm - 30.0) / 10.0);
}

double wattsToDbm(double watts)
{
    return 10.0 * std::log10(watts) + 30.0;
}

std::string formatDbm(double dbm, int decimals)
{
    if (std::isnan(dbm)) return "--.-";
    const double scale = std::pow(10.0, decimals);
    double r = std::round(dbm * scale) / scale;
    if (r == 0.0) r = 0.0;   // no "-0.0"
    std::string s = fixed(std::fabs(r), decimals);
    return (r < 0 ? "-" : "+") + s;
}

std::string formatWatts(double watts, int decimals)
{
    if (std::isnan(watts)) return "--";
    struct { double scale; const char* unit; } const units[] = {
        {1.0, "W"}, {1e-3, "mW"}, {1e-6, "\xC2\xB5W"}, {1e-9, "nW"},
    };
    for (const auto& u : units) {
        if (watts >= u.scale) return fixed(watts / u.scale, decimals) + " " + u.unit;
    }
    return fixed(watts * 1e12, decimals) + " pW";
}

std::string formatSi(double value, const std::string& unit, int digits)
{
    static const char* const prefixes[] = {"p", "n", "\xC2\xB5", "m", "", "k", "M", "G"};
    if (std::isnan(value) || std::isinf(value)) return "-- " + unit;
    const double a = std::fabs(value);
    int e = 0;
    if (a > 0) {
        // 0.9995 keeps 999.96 from printing as "1000" in the lower prefix
        e = static_cast<int>(std::floor(std::log10(a / 0.9995) / 3.0));
        e = std::clamp(e, -4, 3);
    }
    const double num = value / std::pow(1000.0, e);
    return trimZeros(fixed(num, digits)) + " " + prefixes[e + 4] + unit;
}

std::string formatTime(double seconds)
{
    return formatSi(seconds, "s", 2);
}

std::string formatOffset(double offsetDb)
{
    double r = std::round(offsetDb * 10.0) / 10.0;
    if (r == 0.0) r = 0.0;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%c%04.1f", r < 0 ? '-' : '+', std::fabs(r));
    return buf;
}

}  // namespace rfpm
