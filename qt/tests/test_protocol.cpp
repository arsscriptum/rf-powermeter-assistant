// Protocol unit tests: plain C++, no framework. Run with ctest or directly.

#include "MeterProtocol.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                         \
    do {                                                                           \
        const std::string _a = (a), _b = (b);                                      \
        if (_a != _b) {                                                            \
            std::printf("FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__,      \
                        _a.c_str(), _b.c_str());                                   \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

static bool near(double a, double b, double eps = 1e-6)
{
    return std::fabs(a - b) < eps;
}

static void feed(rfpm::StreamParser& p, const std::string& s, rfpm::ParseResult& r)
{
    p.feed(s.data(), s.size(), r);
}

static void testRecords()
{
    rfpm::StreamParser p;
    rfpm::ParseResult r;
    feed(p, "-63300000u+03600229m+12000158w", r);
    CHECK(r.samples.size() == 3);
    CHECK(near(r.samples[0], -63.3f, 1e-4));
    CHECK(near(r.samples[1], 3.6f, 1e-4));   // mW terminator: a parser splitting on 'u' only would miss it
    CHECK(near(r.samples[2], 12.0f, 1e-4));  // W terminator
    CHECK_EQ_STR(r.lastRecord, "+12000158w");
    CHECK(p.totalRecords() == 3);
    CHECK(p.malformedTokens() == 0);
}

static void testSplitAcrossChunks()
{
    rfpm::StreamParser p;
    rfpm::ParseResult r;
    const std::string stream = "-58600000u-58700000u-58800000u";
    for (char c : stream) p.feed(&c, 1, r);   // one byte at a time
    CHECK(r.samples.size() == 3);
    CHECK(near(r.samples[2], -58.8f, 1e-4));
}

static void testBlocksAndSettings()
{
    rfpm::StreamParser p;
    rfpm::ParseResult r;
    std::string s;
    for (int i = 0; i < 500; ++i) s += "-45000000u";
    s += "AR5880+10.0a";   // end of the sweep, with a Read reply injected between A and a
    feed(p, s, r);
    // The block closes on the 'a' itself, before the next record arrives
    CHECK(r.blocks.size() == 1);
    CHECK(r.blocks[0].size() == 500);
    feed(p, "-44000000u", r);
    CHECK(r.settings.size() == 1);
    CHECK(r.settings[0].frequencyMhz == 5880);
    CHECK(near(r.settings[0].offsetDb, 10.0));
    CHECK_EQ_STR(r.settings[0].raw, "R5880+10.0");
    CHECK(r.samples.size() == 501);

    rfpm::ParseResult r2;
    feed(p, "Aa", r2);
    CHECK(r2.blocks.size() == 1);
    CHECK(r2.blocks[0].size() == 1);   // the single record after the previous marker
}

static void testNegativeOffsetReply()
{
    rfpm::StreamParser p;
    rfpm::ParseResult r;
    feed(p, "-50000000uAR0001-03.5a-50100000u", r);
    CHECK(r.settings.size() == 1);
    CHECK(r.settings[0].frequencyMhz == 1);
    CHECK(near(r.settings[0].offsetDb, -3.5));
}

static void testMalformed()
{
    rfpm::StreamParser p;
    rfpm::ParseResult r;
    feed(p, "0000u-63300000u", r);   // partial token at connect
    CHECK(r.samples.size() == 1);
    CHECK(p.malformedTokens() == 1);
    // runaway garbage never grows the token buffer without bound
    std::string junk(10000, 'x');
    feed(p, junk + "-10000000u", r);
    CHECK(r.samples.size() == 2);
    CHECK(near(r.samples[1], -10.0f, 1e-4));
}

static void testCommands()
{
    CHECK_EQ_STR(rfpm::readCommand(), "Read\r\n");
    CHECK_EQ_STR(*rfpm::sampleRateCommand(1), "K01\r\n");
    CHECK_EQ_STR(*rfpm::sampleRateCommand(18), "K18\r\n");
    CHECK(!rfpm::sampleRateCommand(0));
    CHECK(!rfpm::sampleRateCommand(19));

    CHECK_EQ_STR(*rfpm::setCommand(5880, 0.0), "A5880+00.0\r\n");
    CHECK_EQ_STR(*rfpm::setCommand(1000, 10), "A1000+10.0\r\n");
    CHECK_EQ_STR(*rfpm::setCommand(2400, -3.25), "A2400-03.3\r\n");   // rounds to tenths (half away from zero)
    CHECK_EQ_STR(*rfpm::setCommand(1, 99.9), "A0001+99.9\r\n");
    CHECK_EQ_STR(*rfpm::setCommand(915, -0.04), "A0915+00.0\r\n");    // no "-00.0"
    CHECK(!rfpm::setCommand(0, 0));
    CHECK(!rfpm::setCommand(10000, 0));
    CHECK(!rfpm::setCommand(1000, 100.0));
    CHECK(!rfpm::setCommand(1000, std::nan("")));

    // the dangerous short form never passes the gate
    CHECK(!rfpm::isSafeCommand("A0010\r\n"));
    CHECK(!rfpm::isSafeCommand("A5880+10.0\r"));
    CHECK(!rfpm::isSafeCommand("\rRead\r\n"));
    CHECK(!rfpm::isSafeCommand("K00\r\n"));
    CHECK(rfpm::isSafeCommand("A5880-10.0\r\n"));
}

static void testTimebaseAndFormatting()
{
    const auto& tbs = rfpm::timebases();
    CHECK(tbs.size() == 17);
    const auto& def = tbs[rfpm::defaultTimebaseIndex()];
    CHECK(def.k == 1);
    CHECK(def.samples == 500);
    CHECK_EQ_STR(def.label, "1 ms \xC2\xB7 500 kSa/s");
    CHECK(tbs[0].samples == 50);   // 100 us = zoomed view of the K01 sweep
    CHECK(tbs.back().k == 18);
    CHECK(near(rfpm::sweepDuration(1), 1e-3));

    CHECK_EQ_STR(rfpm::formatDbm(-58.6), "-58.6");
    CHECK_EQ_STR(rfpm::formatDbm(3.6), "+3.6");
    CHECK_EQ_STR(rfpm::formatDbm(-0.02), "+0.0");
    CHECK_EQ_STR(rfpm::formatWatts(rfpm::dbmToWatts(0)), "1.000 mW");
    CHECK_EQ_STR(rfpm::formatWatts(rfpm::dbmToWatts(-58.5)), "1.413 nW");
    CHECK_EQ_STR(rfpm::formatWatts(rfpm::dbmToWatts(30)), "1.000 W");
    CHECK_EQ_STR(rfpm::formatSi(12170, "Hz"), "12.17 kHz");
    CHECK_EQ_STR(rfpm::formatTime(82.19e-6), "82.19 \xC2\xB5s");
    CHECK_EQ_STR(rfpm::formatTime(0), "0 s");
    CHECK_EQ_STR(rfpm::formatOffset(-3.5), "-03.5");
    CHECK(near(rfpm::wattsToDbm(rfpm::dbmToWatts(-42.0)), -42.0, 1e-9));
}

int main()
{
    testRecords();
    testSplitAcrossChunks();
    testBlocksAndSettings();
    testNegativeOffsetReply();
    testMalformed();
    testCommands();
    testTimebaseAndFormatting();
    if (g_failures == 0) std::printf("all protocol tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
