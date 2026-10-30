#include <cstdint>
#include <string>

#include "hexpaths.h"
#include "testharness.h"

using TestHarness::check;

namespace {

void testHash()
{
    TestHarness::section("fnv1a64");

    const uint64_t plain = hexviewer::fnv1a64(L"C:\\x\\A.bin");
    check(plain == hexviewer::fnv1a64(L"\\\\?\\C:\\x\\A.bin"),
          "the extended prefix does not change the hash");
    check(plain == hexviewer::fnv1a64(L"//?/C:/x/A.bin"),
          "forward separators do not change the hash");
    check(plain == hexviewer::fnv1a64(L"C:/x/A.bin"),
          "mixed separators do not change the hash");
    check(plain != hexviewer::fnv1a64(L"C:\\x\\a.bin"),
          "case is preserved: A.bin and a.bin hash differently");

    const uint64_t unc = hexviewer::fnv1a64(L"\\\\server\\share\\A.bin");
    check(unc == hexviewer::fnv1a64(L"\\\\?\\UNC\\server\\share\\A.bin"),
          "the UNC extended prefix does not change the hash");
    check(plain != unc, "different paths hash differently");

    std::wstring longName;
    for (int index = 0; index < 255; ++index) {
        longName += static_cast<wchar_t>(L'a' + (index % 26));
    }
    const std::wstring base = L"C:\\d\\";
    const std::wstring first = base + longName;
    std::wstring second = longName;
    second[200] = L'Z';
    check(hexviewer::fnv1a64(first) == hexviewer::fnv1a64(first),
          "the hash is stable across calls");
    check(hexviewer::fnv1a64(first) != hexviewer::fnv1a64(base + second),
          "255-character paths differing at position 200 hash differently");
}

void testCapBaseName()
{
    TestHarness::section("capBaseName");

    check(hexviewer::capBaseName(L"abc.bin") == L"abc.bin",
          "short names pass through unchanged");
    const std::wstring exact64(64, L'x');
    check(hexviewer::capBaseName(exact64) == exact64,
          "a 64-character name passes through unchanged");

    std::wstring name;
    for (int index = 0; index < 100; ++index) {
        name += static_cast<wchar_t>(L'a' + (index % 26));
    }
    const std::wstring capped = hexviewer::capBaseName(name);
    check(capped.size() == 64, "the result is exactly 64 characters");
    check(capped == name.substr(0, 30) + L"..." + name.substr(69),
          "the layout is 30 leading + ... + 31 trailing");
    check(capped.find(L"...") == 30, "the ellipsis sits in the middle");

    const std::wstring capped32 = hexviewer::capBaseName(name, 32);
    check(capped32.size() == 32, "the 32-character cap is honored");
    check(capped32 == name.substr(0, 14) + L"..." + name.substr(85),
          "the 32-character layout is 14 leading + ... + 15 trailing");

    // A high surrogate on the left boundary shrinks the left side by one code
    // unit instead of splitting the pair.
    std::wstring withHigh(29, L'a');
    withHigh += static_cast<wchar_t>(0xD83D);
    withHigh += std::wstring(40, L'b');
    const std::wstring cappedHigh = hexviewer::capBaseName(withHigh);
    check(cappedHigh.size() == 63, "the left side shrinks by one code unit");
    check(cappedHigh.find(static_cast<wchar_t>(0xD83D)) == std::wstring::npos,
          "the high surrogate is excluded, not split");
    check(cappedHigh.substr(29, 3) == L"...", "the ellipsis moved one unit left");

    // A low surrogate on the right boundary shrinks the right side.
    std::wstring withLow(39, L'a');
    withLow += static_cast<wchar_t>(0xDE00);
    withLow += std::wstring(30, L'b');
    const std::wstring cappedLow = hexviewer::capBaseName(withLow);
    check(cappedLow.size() == 63, "the right side shrinks by one code unit");
    check(cappedLow.find(static_cast<wchar_t>(0xDE00)) == std::wstring::npos,
          "the low surrogate is excluded, not split");

    // A cap of exactly 4 leaves the leading run at zero code units, so the
    // leading-boundary check has nothing to inspect and must not index
    // name[size_t(-1)].
    const std::wstring four = hexviewer::capBaseName(name, 4);
    check(four.size() <= 4, "a 4-character cap stays within the limit");
    check(four == L"..." + name.substr(name.size() - 1),
          "a 4-character cap keeps the ellipsis and the last code unit");

    // Below 4 there is no room for the ellipsis; the truncated head must not
    // keep a dangling high surrogate.
    check(hexviewer::capBaseName(std::wstring(30, L'a'), 3) == L"aaa",
          "a 3-character cap truncates without an ellipsis");
    std::wstring splitPair(L"ab");
    splitPair += static_cast<wchar_t>(0xD83D);
    splitPair += std::wstring(20, L'c');
    check(hexviewer::capBaseName(splitPair, 3) == L"ab",
          "a high surrogate on the cut is dropped, not kept dangling");
    check(hexviewer::capBaseName(splitPair, 0).empty(),
          "a zero cap returns nothing");
}

void testOutputFileName()
{
    TestHarness::section("outputFileName");

    const std::wstring path = L"C:\\d\\" + std::wstring(255, L'n');
    const std::wstring name = hexviewer::outputFileName(path);
    check(name.size() == 16 + 3 + 64 + 7,
          "a 255-character name produces the 90-character maximum");
    check(name.substr(16, 3) == L" - ", "the hash and name are separated by ' - '");
    check(name.substr(0, 16).find_first_not_of(L"0123456789abcdef")
              == std::wstring::npos,
          "the hash is 16 lowercase hex digits");
    check(name.compare(name.size() - 7, 7, L".hextxt") == 0,
          "the dump extension is .hextxt");

    std::wstring other(255, L'n');
    other[200] = L'Z';
    check(hexviewer::outputFileName(L"C:\\d\\" + other).substr(0, 16)
              != name.substr(0, 16),
          "paths differing at position 200 get different hash prefixes");
}

void testSafeOutputPath()
{
    TestHarness::section("safeOutputPath");

    const std::wstring longName(255, L'n');
    const std::wstring longInput = L"C:\\d\\" + longName;

    const auto empty = hexviewer::safeOutputPath(L"", L"T:\\tmp\\hex-viewer", longInput);
    check(!empty.ok, "an empty preferred root fails immediately");

    const auto normal = hexviewer::safeOutputPath(L"C:\\pkg", L"", L"C:\\a.bin");
    check(normal.ok
              && normal.txtPath
                     == L"C:\\pkg\\dumps\\" + hexviewer::outputFileName(L"C:\\a.bin"),
          "a short preferred root keeps the full 64-character name");
    check(normal.targetDir == L"C:\\pkg\\dumps", "the target dir is the dumps directory");

    // preferred(150) + \dumps\ (7) + 90 = 247 >= 240 shrinks to the 32-name.
    const std::wstring deep150(147, L'd');  // "C:\" + 147 = 150 characters
    const auto shrink = hexviewer::safeOutputPath(L"C:\\" + deep150, L"", longInput);
    check(shrink.ok && shrink.txtPath.size() == 150 + 7 + 16 + 3 + 32 + 7,
          "a 247-character candidate shrinks the embedded name to 32");

    // preferred(141) + 7 + 90 = 238 < 240 keeps the full name.
    const std::wstring exact141(138, L'e');  // "C:\" + 138 = 141 characters
    const auto fits = hexviewer::safeOutputPath(L"C:\\" + exact141, L"", longInput);
    check(fits.ok && fits.txtPath.size() == 238,
          "238 stays under the budget with the full name");

    // preferred(143) + 7 + 90 = 240 reaches the budget and shrinks.
    const std::wstring exact143(140, L'e');  // "C:\" + 140 = 143 characters
    const auto over = hexviewer::safeOutputPath(L"C:\\" + exact143, L"", longInput);
    check(over.ok && over.txtPath.size() == 143 + 7 + 16 + 3 + 32 + 7,
          "240 shrinks the embedded name first");

    const std::wstring veryDeep(247, L'd');  // "C:\" + 247 = 250 characters
    const auto fallback = hexviewer::safeOutputPath(L"C:\\" + veryDeep,
                                                    L"T:\\tmp\\hex-viewer", longInput);
    check(fallback.ok && fallback.targetDir == L"T:\\tmp\\hex-viewer\\dumps",
          "a too-deep preferred root falls back to the temp root");

    const auto noFallback = hexviewer::safeOutputPath(L"C:\\" + veryDeep, L"", longInput);
    check(!noFallback.ok, "no usable root fails");

    const auto sameFallback = hexviewer::safeOutputPath(L"C:\\" + veryDeep,
                                                        L"C:\\" + veryDeep, longInput);
    check(!sameFallback.ok, "a fallback equal to the preferred root is skipped");
}

void testOutputFileNameReservedCharacters()
{
    TestHarness::section("outputFileName reserved characters");

    // ':' is illegal in a name component and makes Windows read the rest as an
    // NTFS stream name, so an alternate-data-stream input must not leak the
    // colon into the dump name.
    const std::wstring stream = hexviewer::outputFileName(
        L"C:\\d\\file.bin:stream");
    check(stream.find(L':') == std::wstring::npos,
          "an NTFS stream path yields a colon-free dump name");
    check(stream.find(L"file.bin_stream") != std::wstring::npos,
          "the colon becomes an underscore");

    // A drive root has no base name to embed; the hash still makes the name
    // unique, so the empty slot is legal instead of dropping the drive colon.
    const std::wstring root = hexviewer::outputFileName(L"C:\\");
    check(root == root.substr(0, 16) + L" - .hextxt",
          "a drive root keeps the empty base-name slot");
    check(root.find(L':') == std::wstring::npos,
          "a drive root yields a colon-free dump name");

    // Separators cannot reach the component (fileName() splits on them), so
    // the set under test is the one that can survive a real path.
    const wchar_t *reserved = L"<>:\"|?*";
    const std::wstring hostile = std::wstring(L"C:\\d\\a") + reserved + L"b";
    const std::wstring sanitized = hexviewer::outputFileName(hostile);
    for (const wchar_t *unit = reserved; *unit != L'\0'; ++unit) {
        check(sanitized.find(*unit) == std::wstring::npos,
              "a reserved character is replaced in the dump name");
    }
    check(sanitized.find(L"a_______b") != std::wstring::npos,
          "every reserved character maps to a single underscore");

    // Control characters cannot survive a real file name either, but the
    // replacement must not depend on the caller's file system.
    const std::wstring control = hexviewer::outputFileName(
        std::wstring(L"C:\\d\\a") + wchar_t(1) + L"b");
    check(control.find(wchar_t(1)) == std::wstring::npos,
          "a control character is replaced in the dump name");

    // NTFS stores raw UTF-16 code units, so a lone surrogate can reach the dump
    // name; the percent-encoded handoff to SeerViewer would rewrite it to
    // U+FFFD and the viewer would poll for a name that does not exist.
    const std::wstring loneHigh = hexviewer::outputFileName(
        std::wstring(L"C:\\d\\a") + wchar_t(0xD83D) + L"b");
    check(loneHigh.find(wchar_t(0xD83D)) == std::wstring::npos,
          "an unpaired high surrogate is replaced in the dump name");
    check(loneHigh.find(L"a_b") != std::wstring::npos,
          "the unpaired high surrogate becomes a single underscore");

    const std::wstring loneLow = hexviewer::outputFileName(
        std::wstring(L"C:\\d\\a") + wchar_t(0xDE00) + L"b");
    check(loneLow.find(wchar_t(0xDE00)) == std::wstring::npos,
          "an unpaired low surrogate is replaced in the dump name");

    // A well-formed pair must survive so the capped name keeps its glyph.
    const std::wstring pair = std::wstring(1, wchar_t(0xD83D))
                              + std::wstring(1, wchar_t(0xDE00));
    const std::wstring kept = hexviewer::outputFileName(L"C:\\d\\a" + pair + L"b");
    check(kept.find(pair) != std::wstring::npos,
          "a well-formed surrogate pair is preserved");
}

void testTmpFileName()
{
    TestHarness::section("tmpFileName");

    check(hexviewer::tmpFileName(L"C:\\a\\b.txt", 4242, 7) == L"C:\\a\\b.txt.4242_7.tmp",
          "the tmp name appends pid and nonce");
    check(hexviewer::tmpFileName(L"C:\\a\\b.txt", 4243, 7)
              != hexviewer::tmpFileName(L"C:\\a\\b.txt", 4242, 7),
          "different invocations get different tmp names");
}

}  // namespace

int main()
{
    testHash();
    testCapBaseName();
    testOutputFileName();
    testOutputFileNameReservedCharacters();
    testSafeOutputPath();
    testTmpFileName();
    return TestHarness::summarize("hexpaths_test");
}
