#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <fstream>

#include <windows.h>

#include "args.h"
#include "paths.h"
#include "geometry.h"
#include "hash.h"
#include "output_writer.h"

#define TEST_CHECK(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "Assertion failed: " #expr << " at line " << __LINE__ << std::endl; \
            std::exit(1); \
        } \
    } while (0)

using namespace seer_ocr;

namespace {

QuadBox makeBox(float x0, float y0, float x1, float y1)
{
    QuadBox box;
    box.points[0] = {x0, y0};
    box.points[1] = {x1, y0};
    box.points[2] = {x1, y1};
    box.points[3] = {x0, y1};
    return box;
}

float centerX(const QuadBox &box)
{
    return (box.points[0].x + box.points[1].x + box.points[2].x + box.points[3].x) * 0.25f;
}

float centerY(const QuadBox &box)
{
    return (box.points[0].y + box.points[1].y + box.points[2].y + box.points[3].y) * 0.25f;
}

}  // namespace

void testCommandLineParsing()
{
    // Basic 3 positionals
    {
        std::vector<std::wstring> args = {
            L"SeerOcrWorker.exe",
            L"C:\\shot.png",
            L"C:\\SeerViewer.exe",
            L"sample.pdf"
        };
        auto opts = parseCommandLine(args);
        TEST_CHECK(opts.valid);
        TEST_CHECK(opts.screenshot_path == L"C:\\shot.png");
        TEST_CHECK(opts.seerviewer_exe == L"C:\\SeerViewer.exe");
        TEST_CHECK(opts.input_file == L"sample.pdf");
        TEST_CHECK(opts.ep == ExecutionProvider::Auto);
        TEST_CHECK(opts.enable_cls == true);
        TEST_CHECK(opts.wait_timeout_ms == 120000);
        TEST_CHECK(opts.console == false);
    }

    // Flags parsing
    {
        std::vector<std::wstring> args = {
            L"SeerOcrWorker.exe",
            L"--ep=dml",
            L"--cls=0",
            L"--wait-timeout=15000",
            L"--console",
            L"--max-side=2048",
            L"C:\\shot.png",
            L"C:\\SeerViewer.exe",
            L"sample.pdf"
        };
        auto opts = parseCommandLine(args);
        TEST_CHECK(opts.valid);
        TEST_CHECK(opts.ep == ExecutionProvider::Dml);
        TEST_CHECK(opts.enable_cls == false);
        TEST_CHECK(opts.wait_timeout_ms == 15000);
        TEST_CHECK(opts.console == true);
        TEST_CHECK(opts.max_side == 2048);
    }

    // Missing positionals
    {
        std::vector<std::wstring> args = {
            L"SeerOcrWorker.exe",
            L"C:\\shot.png"
        };
        auto opts = parseCommandLine(args);
        TEST_CHECK(!opts.valid);
    }

    // Unknown flag
    {
        std::vector<std::wstring> args = {
            L"SeerOcrWorker.exe",
            L"--unknown-flag",
            L"C:\\shot.png",
            L"C:\\SeerViewer.exe",
            L"sample.pdf"
        };
        auto opts = parseCommandLine(args);
        TEST_CHECK(!opts.valid);
    }

    std::cout << "[PASS] testCommandLineParsing" << std::endl;
}

void testPathSanitization()
{
    // Normal file
    TEST_CHECK(sanitizeBaseName(L"C:\\Users\\test\\docs\\report.pdf") == L"report.pdf");
    TEST_CHECK(sanitizeBaseName(L"archive/sub/image.png") == L"image.png");

    // Invalid characters
    TEST_CHECK(sanitizeBaseName(L"foo:bar*baz?.txt") == L"foo_bar_baz_.txt");

    // Trailing dots and spaces
    TEST_CHECK(sanitizeBaseName(L"name . . ") == L"name");

    // Empty input
    TEST_CHECK(sanitizeBaseName(L"") == L"unnamed");

    // DOS device names
    TEST_CHECK(sanitizeBaseName(L"CON") == L"_CON");
    TEST_CHECK(sanitizeBaseName(L"NUL") == L"_NUL");
    TEST_CHECK(sanitizeBaseName(L"com1") == L"_com1");

    // Truncation to 80 chars
    std::wstring long_name(100, L'a');
    TEST_CHECK(sanitizeBaseName(long_name).size() == 80);

    std::cout << "[PASS] testPathSanitization" << std::endl;
}

void testOutputBaseNameIsDirectoryScoped()
{
    // md5 sanity: the fingerprint must be a real hash, not a placeholder.
    TEST_CHECK(md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    TEST_CHECK(md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");

    // Same file name, different folders: two output sets, never one.
    const size_t kPrefixLength = 8 + 3;  // fingerprint + " - "
    const auto in_a = outputBaseName(L"C:\\docs\\report.pdf");
    const auto in_b = outputBaseName(L"D:\\archive\\report.pdf");
    TEST_CHECK(in_a != in_b);
    TEST_CHECK(in_a.size() > kPrefixLength && in_b.size() > kPrefixLength);
    TEST_CHECK(in_a.substr(kPrefixLength) == L"report.pdf");
    TEST_CHECK(in_b.substr(kPrefixLength) == L"report.pdf");

    // Same folder: identical output set, so a re-run replaces its own result.
    TEST_CHECK(outputBaseName(L"C:\\docs\\report.pdf")
               == outputBaseName(L"C:\\docs\\report.pdf"));

    // The fingerprint is 8 hex chars + " - "; the ".ocr" infix stays with the
    // name so sweepOldOutputs()'s "*.ocr.*" glob still matches.
    TEST_CHECK(in_a.size() > 8);
    TEST_CHECK(in_a.substr(8, 3) == L" - ");
    for (size_t i = 0; i < 8; ++i) {
        const wchar_t ch = in_a[i];
        const bool hex = (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
        TEST_CHECK(hex);
    }

    // Windows paths are case-insensitive and take both separators; the same
    // folder written two ways must fingerprint the same.
    TEST_CHECK(outputBaseName(L"C:\\Path\\file.png")
               == outputBaseName(L"c:/path/file.png"));

    // A bare name has no directory; it still has to be deterministic.
    TEST_CHECK(outputBaseName(L"file.png") == outputBaseName(L"file.png"));
    TEST_CHECK(outputBaseName(L"file.png") != outputBaseName(L"other.png"));

    std::cout << "[PASS] testOutputBaseNameIsDirectoryScoped" << std::endl;
}

void testJsonFormatting()
{
    OcrJsonMeta meta;
    meta.source_image = "C:\\Temp\\shot.png";
    meta.img_width    = 1000;
    meta.img_height   = 800;
    meta.ep_used      = "dml";
    meta.fallback     = false;
    meta.elapsed_ms   = 1234;

    std::vector<OcrLineResult> lines;
    OcrLineResult l1;
    l1.index       = 0;
    l1.text        = "Hello \"World\"\nTest";
    l1.score       = 0.9876f;
    l1.orientation = 0;
    l1.box[0][0] = 10; l1.box[0][1] = 20;
    l1.box[1][0] = 100; l1.box[1][1] = 20;
    l1.box[2][0] = 100; l1.box[2][1] = 40;
    l1.box[3][0] = 10; l1.box[3][1] = 40;
    lines.push_back(l1);

    std::string json = formatOcrJson(meta, lines);
    TEST_CHECK(json.find("\"schema\": \"seer.ocr/1\"") != std::string::npos);
    TEST_CHECK(json.find("\"source_image\": \"C:\\\\Temp\\\\shot.png\"") != std::string::npos);
    TEST_CHECK(json.find("\"text\": \"Hello \\\"World\\\"\\nTest\"") != std::string::npos);
    TEST_CHECK(json.find("\"score\": 0.9876") != std::string::npos);
    TEST_CHECK(json.find("\"elapsed_ms\": 1234") != std::string::npos);

    std::cout << "[PASS] testJsonFormatting" << std::endl;
}

void testGeometryAndSorting()
{
    std::vector<PointF> square = {
        {0.0f, 0.0f},
        {10.0f, 0.0f},
        {10.0f, 10.0f},
        {0.0f, 10.0f}
    };
    float area = polygonArea(square);
    TEST_CHECK(std::abs(area - 100.0f) < 1e-3f);

    float peri = polygonPerimeter(square);
    TEST_CHECK(std::abs(peri - 40.0f) < 1e-3f);

    QuadBox box;
    bool ok = unclipPolygon(square, 1.5f, box);
    TEST_CHECK(ok);
    // Unclipped box should be larger than 10x10
    float unclipped_w = box.points[1].x - box.points[0].x;
    TEST_CHECK(unclipped_w > 10.0f);

    // Reading order test: 2 boxes in single column
    QuadBox b1;
    b1.points[0] = {10, 50}; b1.points[1] = {100, 50};
    b1.points[2] = {100, 70}; b1.points[3] = {10, 70};

    QuadBox b2;
    b2.points[0] = {10, 10}; b2.points[1] = {100, 10};
    b2.points[2] = {100, 30}; b2.points[3] = {10, 30};

    auto sorted = sortReadingOrder({b1, b2});
    TEST_CHECK(sorted.size() == 2);
    // Top box b2 (y=10..30) should come first before b1 (y=50..70)
    TEST_CHECK(sorted[0].points[0].y < sorted[1].points[0].y);

    std::cout << "[PASS] testGeometryAndSorting" << std::endl;
}

void testUnclipUnorderedCloud()
{
    // runDetection pushes boundary pixels in BFS discovery order, not perimeter
    // order. Feed exactly that shape here: a 100x20 rectangle, row by row.
    std::vector<PointF> cloud;
    for (int x = 0; x < 100; ++x) {
        cloud.push_back({static_cast<float>(x), 0.0f});
        cloud.push_back({static_cast<float>(x), 19.0f});
    }
    for (int y = 1; y < 19; ++y) {
        cloud.push_back({0.0f, static_cast<float>(y)});
        cloud.push_back({99.0f, static_cast<float>(y)});
    }

    QuadBox box;
    TEST_CHECK(unclipPolygon(cloud, 1.5f, box));

    const auto dist = [](const PointF &a, const PointF &b) {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    };
    const float w = dist(box.points[0], box.points[1]);
    const float h = dist(box.points[0], box.points[3]);

    // Unclipped by ~12px on every side, never collapsed to a sliver.
    TEST_CHECK(w > 110.0f && w < 140.0f);
    TEST_CHECK(h > 30.0f && h < 60.0f);

    const float cx = (box.points[0].x + box.points[2].x) * 0.5f;
    const float cy = (box.points[0].y + box.points[2].y) * 0.5f;
    TEST_CHECK(std::abs(cx - 49.5f) < 2.0f);
    TEST_CHECK(std::abs(cy - 9.5f) < 2.0f);

    std::cout << "[PASS] testUnclipUnorderedCloud" << std::endl;
}

void testReadingOrderBands()
{
    // Single column, one visual line of two boxes whose centers interleave in y
    // (10 vs 11) with 20px heights. The old "same line ? x : y" comparator was
    // not a strict weak ordering for exactly this shape.
    QuadBox a = makeBox(0, 0, 200, 20);     // line 1, left
    QuadBox b = makeBox(210, 1, 400, 21);   // line 1, right
    QuadBox c = makeBox(0, 30, 200, 50);    // line 2

    auto sorted = sortReadingOrder({b, c, a});
    TEST_CHECK(sorted.size() == 3);
    TEST_CHECK(centerX(sorted[0]) < centerX(sorted[1]));
    TEST_CHECK(std::abs(centerY(sorted[0]) - 10.0f) < 2.0f);
    TEST_CHECK(std::abs(centerY(sorted[1]) - 11.0f) < 2.0f);
    TEST_CHECK(std::abs(centerY(sorted[2]) - 40.0f) < 2.0f);

    std::cout << "[PASS] testReadingOrderBands" << std::endl;
}

void testSha256AndSums()
{
    wchar_t temp[MAX_PATH] = {0};
    TEST_CHECK(GetTempPathW(MAX_PATH, temp) > 0);

    const std::wstring dir = std::wstring(temp) + L"SeerOcrHashTest";
    CreateDirectoryW(dir.c_str(), nullptr);

    const std::wstring file = dir + L"\\probe.bin";
    {
        std::ofstream out(file.c_str(), std::ios::binary);
        out << "abc";
    }

    const std::string hex = sha256FileHex(file);
    TEST_CHECK(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    const std::wstring sums_path = dir + L"\\SHA256SUMS";
    {
        std::ofstream sums(sums_path.c_str(), std::ios::binary);
        sums << hex << " *probe.bin\n";
    }

    std::map<std::string, std::string> parsed;
    TEST_CHECK(loadSha256Sums(sums_path, parsed));
    TEST_CHECK(parsed["probe.bin"] == hex);

    std::string log;
    TEST_CHECK(verifyModelFiles(dir, {L"probe.bin"}, log));

    {
        std::ofstream sums(sums_path.c_str(), std::ios::binary);
        sums << std::string(64, '0') << " *probe.bin\n";
    }
    log.clear();
    TEST_CHECK(!verifyModelFiles(dir, {L"probe.bin"}, log));

    // No pin at all stays acceptable: development drops are not blocked.
    DeleteFileW(sums_path.c_str());
    log.clear();
    TEST_CHECK(verifyModelFiles(dir, {L"probe.bin"}, log));

    DeleteFileW(file.c_str());
    RemoveDirectoryW(dir.c_str());

    std::cout << "[PASS] testSha256AndSums" << std::endl;
}

int main()
{
    std::cout << "Starting OCR Plugin Unit Tests..." << std::endl;
    testCommandLineParsing();
    testPathSanitization();
    testOutputBaseNameIsDirectoryScoped();
    testJsonFormatting();
    testGeometryAndSorting();
    testUnclipUnorderedCloud();
    testReadingOrderBands();
    testSha256AndSums();
    std::cout << "All OCR Plugin Unit Tests Passed!" << std::endl;
    return 0;
}
