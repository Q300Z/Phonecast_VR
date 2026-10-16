#include "phonecast/vr/overlay/OverlaySettingsStore.h"
#include "phonecast/vr/overlay/OverlayController.h"
#include "phonecast/vr/overlay/SettingsMenuController.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

int gFailures = 0;
int gPasses = 0;

void Check(bool condition, const std::string& description, const std::string& extra = "") {
    if (condition) {
        ++gPasses;
        std::cout << "  [PASS] " << description << "\n";
    } else {
        ++gFailures;
        std::cerr << "  [FAIL] " << description;
        if (!extra.empty()) {
            std::cerr << " (" << extra << ")";
        }
        std::cerr << "\n";
    }
}

std::string GenerateBaseValidIni() {
    std::ostringstream ss;
    ss << "version=5\n"
       << "mode=phone-tracked\n"
       << "width=0.072\n"
       << "distance=1.0\n"
       << "alpha=0.9\n"
       << "offset_x=0.0\n"
       << "offset_y=0.0\n"
       << "glance_preview_scale=0.5\n"
       << "radial_long_press_ms=500\n"
       << "notification_width=0.5\n"
       << "notification_distance=0.8\n"
       << "notification_offset_x=0.0\n"
       << "notification_offset_y=0.0\n"
       << "start_distance=1.0\n"
       << "start_offset_x=0.0\n"
       << "start_offset_y=0.0\n"
       << "audio_muted=0\n"
       << "audio_volume=1.0\n"
       << "audio_system_default=1\n"
       << "left_distance=0.3\n"
       << "left_height=0.0\n"
       << "left_lateral=0.0\n"
       << "left_tilt=0.0\n"
       << "left_yaw=0.0\n"
       << "left_scale=1.0\n"
       << "left_orientation=face-user\n"
       << "right_distance=0.3\n"
       << "right_height=0.0\n"
       << "right_lateral=0.0\n"
       << "right_tilt=0.0\n"
       << "right_yaw=0.0\n"
       << "right_scale=1.0\n"
       << "right_orientation=face-user\n"
       << "world_valid=0\n";
    for (int i = 0; i < 16; ++i) {
        ss << "world_" << i << "=0.0\n";
    }
    return ss.str();
}

void ReplaceIniLine(std::string& ini, const std::string& prefix, const std::string& newLine) {
    const auto pos = ini.find(prefix);
    if (pos == std::string::npos) return;
    const auto endPos = ini.find('\n', pos);
    ini.replace(pos, endPos == std::string::npos ? std::string::npos : (endPos - pos), newLine);
}

} // namespace

int main() {
    std::cout << "======================================================================\n";
    std::cout << "M4 Challenger 5: Adversarial Verification of OverlaySettingsStore\n";
    std::cout << "======================================================================\n\n";

    const auto tempDir = std::filesystem::temp_directory_path() / "phonecast_m4_challenger5_test";
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);
    const auto iniPath = tempDir / "overlay-settings.ini";

    phonecast::vr::OverlaySettingsStore store(iniPath);

    // -------------------------------------------------------------------------
    // TEST 1: Physical smartphone screen dimensions under PlacementMode::PhoneTracked
    // -------------------------------------------------------------------------
    std::cout << "=== [Test 1] Physical Smartphone Screen Dimensions (PhoneTracked) ===\n";
    const std::vector<float> physicalPhoneWidths = {
        0.04F,   // 40 mm (minimum supported physical display / micro display)
        0.057F,  // 57 mm (compact phone / Galaxy Z Fold outer cover display)
        0.070F,  // 70 mm (synthetic standard phone width)
        0.0715F, // 71.5 mm (Pixel 8)
        0.072F,  // 72 mm (default TrackedCameraWorker metric fallback)
        0.075F,  // 75 mm (standard mainstream smartphone)
        0.0781F, // 78.1 mm (Galaxy S24 Ultra)
        0.082F   // 82 mm (large phablet / wide display)
    };

    for (float width : physicalPhoneWidths) {
        phonecast::vr::OverlaySettings settings;
        settings.placementMode = phonecast::vr::PlacementMode::PhoneTracked;
        settings.widthMeters = width;
        settings.distanceMeters = 0.5F;
        settings.alpha = 1.0F;

        std::string saveError;
        const bool saveOk = store.Save(settings, saveError);
        Check(saveOk, "Save succeeded for width " + std::to_string(width) + "m", saveError);

        phonecast::vr::OverlaySettings loaded;
        bool found = false;
        std::string loadError;
        const bool loadOk = store.Load(loaded, found, loadError);

        Check(loadOk, "Load succeeded for width " + std::to_string(width) + "m", loadError);
        Check(found, "Entry found for width " + std::to_string(width) + "m");
        Check(loaded.placementMode == phonecast::vr::PlacementMode::PhoneTracked,
              "PlacementMode is PhoneTracked for width " + std::to_string(width) + "m");
        Check(std::abs(loaded.widthMeters - width) < 1e-4F,
              "Loaded width (" + std::to_string(loaded.widthMeters) + ") matches expected (" +
              std::to_string(width) + ")");
    }

    // -------------------------------------------------------------------------
    // TEST 2: Strict rejection of dimensions strictly below 0.04m (< 0.04m)
    // -------------------------------------------------------------------------
    std::cout << "\n=== [Test 2] Strict Rejection of Sub-Minimum Dimensions (< 0.04m) ===\n";
    const std::vector<std::string> subMinimumWidths = {
        "0.039999",
        "0.039",
        "0.03",
        "0.02",
        "0.01",
        "0.001",
        "0.0",
        "-0.0001",
        "-0.04",
        "-1.0",
        "-10.0",
        "-1000.0"
    };

    const std::string baseIni = GenerateBaseValidIni();

    for (const auto& widthStr : subMinimumWidths) {
        std::string testIni = baseIni;
        ReplaceIniLine(testIni, "width=", "width=" + widthStr);

        {
            std::ofstream out(iniPath, std::ios::trunc);
            out << testIni;
        }

        phonecast::vr::OverlaySettings loaded;
        // Pre-fill with a sentinel value
        loaded.widthMeters = 0.555F;
        bool found = false;
        std::string loadError;
        const bool loadOk = store.Load(loaded, found, loadError);

        Check(!loadOk, "Width '" + widthStr + "' is rejected by OverlaySettingsStore::Load");
        Check(loadError == "Overlay settings contain an invalid value.",
              "Error string matches verbatim for '" + widthStr + "': " + loadError);
        Check(loaded.widthMeters == 0.555F,
              "Target settings untouched on rejected width '" + widthStr + "'");
    }

    // -------------------------------------------------------------------------
    // TEST 3: Strict rejection of dimensions strictly above 5.0m (> 5.0m)
    // -------------------------------------------------------------------------
    std::cout << "\n=== [Test 3] Strict Rejection of Super-Maximum Dimensions (> 5.0m) ===\n";
    const std::vector<std::string> superMaximumWidths = {
        "5.0001",
        "5.01",
        "5.1",
        "6.0",
        "10.0",
        "100.0",
        "1000.0",
        "1e6",
        "1e30"
    };

    for (const auto& widthStr : superMaximumWidths) {
        std::string testIni = baseIni;
        ReplaceIniLine(testIni, "width=", "width=" + widthStr);

        {
            std::ofstream out(iniPath, std::ios::trunc);
            out << testIni;
        }

        phonecast::vr::OverlaySettings loaded;
        loaded.widthMeters = 0.555F;
        bool found = false;
        std::string loadError;
        const bool loadOk = store.Load(loaded, found, loadError);

        Check(!loadOk, "Width '" + widthStr + "' is rejected by OverlaySettingsStore::Load");
        Check(loadError == "Overlay settings contain an invalid value.",
              "Error string matches verbatim for '" + widthStr + "': " + loadError);
        Check(loaded.widthMeters == 0.555F,
              "Target settings untouched on rejected width '" + widthStr + "'");
    }

    // -------------------------------------------------------------------------
    // TEST 4: Malformed, Non-finite, and Non-numeric Width Inputs
    // -------------------------------------------------------------------------
    std::cout << "\n=== [Test 4] Malformed, Non-finite, and Non-numeric Width Inputs ===\n";
    const std::vector<std::string> malformedWidths = {
        "nan",
        "NAN",
        "inf",
        "-inf",
        "infinity",
        "-infinity",
        "abc",
        "0.072m",
        "0.072 0.05",
        "0.04extra",
        "",
        " "
    };

    for (const auto& widthStr : malformedWidths) {
        std::string testIni = baseIni;
        ReplaceIniLine(testIni, "width=", "width=" + widthStr);

        {
            std::ofstream out(iniPath, std::ios::trunc);
            out << testIni;
        }

        phonecast::vr::OverlaySettings loaded;
        loaded.widthMeters = 0.555F;
        bool found = false;
        std::string loadError;
        const bool loadOk = store.Load(loaded, found, loadError);

        Check(!loadOk, "Malformed width '" + widthStr + "' is rejected");
        Check(loadError == "Overlay settings contain an invalid value.",
              "Verbatim error on malformed '" + widthStr + "': " + loadError);
    }

    // -------------------------------------------------------------------------
    // TEST 5: Exact Boundaries (0.04F and 5.0F)
    // -------------------------------------------------------------------------
    std::cout << "\n=== [Test 5] Exact Boundary Dimensions (0.04m and 5.0m) ===\n";
    {
        // Exactly 0.04m
        std::string testIni = baseIni;
        ReplaceIniLine(testIni, "width=", "width=0.04");
        {
            std::ofstream out(iniPath, std::ios::trunc);
            out << testIni;
        }
        phonecast::vr::OverlaySettings loaded;
        bool found = false;
        std::string loadError;
        Check(store.Load(loaded, found, loadError) && found, "Exact lower bound 0.04m loads successfully", loadError);
        Check(std::abs(loaded.widthMeters - 0.04F) < 1e-4F, "Exact lower bound matches 0.04F");

        // Exactly 5.0m
        testIni = baseIni;
        ReplaceIniLine(testIni, "width=", "width=5.0");
        {
            std::ofstream out(iniPath, std::ios::trunc);
            out << testIni;
        }
        Check(store.Load(loaded, found, loadError) && found, "Exact upper bound 5.0m loads successfully", loadError);
        Check(std::abs(loaded.widthMeters - 5.0F) < 1e-4F, "Exact upper bound matches 5.0F");
    }

    // -------------------------------------------------------------------------
    // TEST 6: OverlayController & SettingsMenuController Width Bounds
    // -------------------------------------------------------------------------
    std::cout << "\n=== [Test 6] OverlayController & SettingsMenuController Integration ===\n";
    {
        // OverlayController ScaleDown should clamp at 0.04F
        phonecast::vr::OverlaySettings initial;
        initial.placementMode = phonecast::vr::PlacementMode::HeadLocked;
        initial.widthMeters = 0.06F;
        phonecast::vr::OverlayController controller(initial);

        // ScaleDown steps by -0.05F: 0.06F - 0.05F = 0.01F -> clamp to 0.04F
        controller.Apply(phonecast::vr::OverlayAction::ScaleDown);
        Check(std::abs(controller.Settings().widthMeters - 0.04F) < 1e-4F,
              "OverlayController::ScaleDown clamps at 0.04F (actual: " +
              std::to_string(controller.Settings().widthMeters) + ")");

        // Further ScaleDown should remain clamped at 0.04F
        controller.Apply(phonecast::vr::OverlayAction::ScaleDown);
        Check(std::abs(controller.Settings().widthMeters - 0.04F) < 1e-4F,
              "OverlayController::ScaleDown maintains clamp at 0.04F");

        // SettingsMenuController Appearance slider normalized minimum should correspond to 0.04F
        phonecast::vr::SettingsMenuController menu;
        menu.Open(initial);
        // Navigate to Appearance
        menu.Handle(phonecast::vr::SettingsMenuCommand::Activate);
        // Set width slider to minimum (normalized 0.0F)
        menu.Handle({phonecast::vr::SettingsMenuCommand::SetNormalized, 0.0F});
        Check(std::abs(menu.Draft().widthMeters - 0.04F) < 1e-4F,
              "SettingsMenuController slider normalized 0.0F sets width to 0.04F (actual: " +
              std::to_string(menu.Draft().widthMeters) + ")");

        // Set width slider to maximum (normalized 1.0F) -> 2.0F
        menu.Handle({phonecast::vr::SettingsMenuCommand::SetNormalized, 1.0F});
        Check(std::abs(menu.Draft().widthMeters - 2.0F) < 1e-4F,
              "SettingsMenuController slider normalized 1.0F sets width to 2.0F (actual: " +
              std::to_string(menu.Draft().widthMeters) + ")");
    }

    // Cleanup
    std::filesystem::remove_all(tempDir, ec);

    std::cout << "\n======================================================================\n";
    std::cout << "SUMMARY: " << gPasses << " PASSED, " << gFailures << " FAILED\n";
    std::cout << "======================================================================\n";

    return gFailures == 0 ? 0 : 1;
}
