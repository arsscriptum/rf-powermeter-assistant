// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   MeterProtocol.h                                                              ║
// ║   Wire protocol of the "USB RF Power Meter V5" (STM32 + AD8317/AD8318)         ║
// ╚════════════════════════════════════════════════════════════════════════════════╝
//
// Pure C++17 (no Qt): stream parser, command builders, timebase table, formatting.
//
//  Stream record (10 bytes), e.g. "-72400000u":
//
//     [+|-] D D d D D D d d <unit>
//       |   dBm   |  watts  |  'u' = uW, 'm' = mW, 'w' = W (also the record terminator)
//
//  The dBm field is the finished, calibrated value (band calibration + offset are applied
//  by the firmware). Every 500 records the meter emits the block marker "Aa": one block is
//  one 500-sample sweep taken at the sample period set with K01..K18.
//
//  Commands (CRLF terminated, one command per write):
//     Read               -> reply "R<freq:4><+-##.#>" injected between the 'A' and 'a' of the
//                           next block marker
//     A<freq:4><+-##.#>  -> set band-cal frequency (MHz) and offset (dB). NEVER send the short
//                           "A<freq:4>" form: it corrupts the meter state (stream pegs at -99.9).
//     K01..K18           -> set the sweep sample rate (write-only)
//
//  Reference: https://github.com/LostInNovo/rf-power-meter-v5-companion/blob/main/PROTOCOL.md

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rfpm {

constexpr int kSamplesPerBlock = 500;
constexpr int kMinFrequencyMhz = 1;
constexpr int kMaxFrequencyMhz = 9999;
constexpr double kMaxOffsetDb = 99.9;
constexpr int kMinK = 1;
constexpr int kMaxK = 18;
constexpr int kDefaultBaud = 460800;
constexpr std::uint16_t kUsbVendorId = 0x0483;   // STMicroelectronics
constexpr std::uint16_t kUsbProductId = 0x5740;  // Virtual COM Port

/// A "Read" reply found in the stream, e.g. "R5880+00.0".
struct SettingsReply
{
    int frequencyMhz = 0;
    double offsetDb = 0.0;
    std::string raw;
};

/// Everything one chunk of serial bytes produced.
struct ParseResult
{
    std::vector<float> samples;               // every dBm value, in order
    std::vector<std::vector<float>> blocks;   // sweeps closed by an 'a' marker in this chunk
    std::vector<SettingsReply> settings;      // settings replies
    std::string lastRecord;                   // last raw record, e.g. "-63300000u" (empty if none)

    void clear()
    {
        samples.clear();
        blocks.clear();
        settings.clear();
        lastRecord.clear();
    }
};

/// Incremental parser. Tokens are split at the record terminators ('u', 'm', 'w'): a token
/// normally ends in a 9-char record (sign + 8 digits) and may carry block-marker chars
/// ("Aa") and/or an embedded settings reply in front of it.
class StreamParser
{
public:
    void reset();
    void feed(const char* data, std::size_t size, ParseResult& out);

    std::int64_t totalRecords() const { return records_; }
    /// Tokens that did not end in a valid record. One partial token at connect is normal.
    std::int64_t malformedTokens() const { return malformed_; }

private:
    void completeToken(char unit, ParseResult& out);
    void closeBlock(ParseResult& out);

    std::string token_;
    std::vector<float> block_;
    std::int64_t records_ = 0;
    std::int64_t malformed_ = 0;
};

// ─── timebase ───────────────────────────────────────────────────────────────────

/// One position of the sweep timebase: the visible window and the K command (hardware
/// sample rate) that produces it. Windows shorter than one 500-sample sweep at K01 are
/// zoomed views of the K01 sweep.
struct Timebase
{
    double windowSec;
    int k;
    double periodSec;
    int samples;        // samples visible in the window (<= 500)
    std::string label;  // "1 ms · 500 kSa/s"
};

const std::vector<Timebase>& timebases();
int defaultTimebaseIndex();                 // 1 ms - 500 kSa/s
double samplePeriod(int k);                 // seconds, k in 1..18 (0 if out of range)
double sweepDuration(int k);                // 500 x period
int nominalWireRate(int k);                 // measured records/s for that K

// ─── commands ───────────────────────────────────────────────────────────────────

std::string readCommand();                                     // "Read\r\n"
std::optional<std::string> sampleRateCommand(int k);           // "K01\r\n"
/// Full-form "A<freq:4><+-##.#>\r\n". nullopt when out of range: the short form is never built.
std::optional<std::string> setCommand(int frequencyMhz, double offsetDb);
/// Final gate before anything touches the port: only the verified command shapes pass.
bool isSafeCommand(const std::string& command);

// ─── units and formatting ───────────────────────────────────────────────────────

double dbmToWatts(double dbm);
double wattsToDbm(double watts);
/// "+3.6" / "-58.6": always signed, fixed decimals.
std::string formatDbm(double dbm, int decimals = 1);
/// Auto-scaled unit (W / mW / µW / nW / pW) with fixed decimals: "1.422 nW".
std::string formatWatts(double watts, int decimals = 3);
/// SI prefix, trailing zeros trimmed: (12170, "Hz") -> "12.17 kHz".
std::string formatSi(double value, const std::string& unit, int digits = 2);
/// formatSi for seconds: "82.19 µs".
std::string formatTime(double seconds);
/// Offset as the meter prints it: "+00.0".
std::string formatOffset(double offsetDb);

}  // namespace rfpm
