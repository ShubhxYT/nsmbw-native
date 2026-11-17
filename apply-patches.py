#!/usr/bin/env python3
"""apply-patches.py — the fixes this NSMBW port needs, applied idempotently.

    python3 apply-patches.py <path to the recompilation checkout>

Safe to run repeatedly: every patch checks for its own marker first. Each one is a real gap in the
upstream sources, not a preference:

  1. BCA check      the game polls the disc's burst-cutting area forever; no host is asked what to
                    answer, the translated body spins on a DI interrupt that never comes.
  2. macOS netplay  upstream has Windows/Android/Linux restart branches only, so every LAN session
                    on a Mac dies at "restarting into session" (_Exit(0) fallthrough).
  3. Button labels  PlayStation / Xbox / Nintendo naming, picked in F10 > Controls, persisted as
                    [controller] button_layout.
"""
import hashlib
import pathlib
import re
import sys


def read(path: pathlib.Path) -> str:
    if not path.exists():
        sys.exit(f"missing file: {path}")
    return path.read_text()


def write(path: pathlib.Path, text: str) -> None:
    path.write_text(text)


def patch_variant_hash(repo: pathlib.Path, disc: pathlib.Path, report: list[str]) -> None:
    """Point the [E1] checksum at this disc's main.dol.

    A dump whose executable differs from the table is not refused outright: disc identity, FST size,
    entry point and the REL prelink all still validate it, and a difference usually means a patched
    DOL (region-free or video-mode patches are the common ones). The change is recorded so it can be
    the first suspect if the game misbehaves later.
    """
    dol = disc / "sys" / "main.dol"
    if not dol.exists():
        return
    sha = hashlib.sha256(dol.read_bytes()).hexdigest()
    cfg = repo / "projects/nsmbw/variants/variants.toml"
    text = read(cfg)
    match = re.search(r'(\[E1\][\s\S]*?main_dol_sha256 = ")([0-9a-f]+)(")', text)
    if not match:
        sys.exit("variants.toml has no [E1] main_dol_sha256")
    if match.group(2) == sha:
        report.append("disc main.dol matches the supported dump")
        return
    backup = cfg.with_name("variants.toml.orig")
    if not backup.exists():
        backup.write_text(text)
    text = text[: match.start(2)] + sha + text[match.end(2) :]
    write(cfg, text)
    report.append(f"disc main.dol differs from the table ({sha[:12]}…): recorded it")
    report.append("  if the game misbehaves later, that mismatch is the first thing to check")


def patch_bca(repo: pathlib.Path, report: list[str]) -> None:
    dvd = repo / "runtime/src/hle/storage/dvd.cpp"
    text = read(dvd)
    if "DVDCheckDiskBca_801D0CF0" in text:
        report.append("BCA check already patched")
        return
    anchor = "PPC_NATIVE_OVERRIDE_VOID(801CAD30, DVDInit_8015EA1C, (), ());"
    if anchor not in text:
        sys.exit("cannot apply the BCA fix: DVDInit override not found")
    block = anchor + """

// 0x801D0CF0 -> DVDCheckDiskBca
// The BCA lives in the disc's lead-in, which no host file extraction carries, and the translated
// body spins on a DI completion flag that nothing sets (the game polls it forever after NW4R's EF
// init). Answer the way an emulator does without a .bca file: no BCA on this disc.
extern "C" int32_t DVDCheckDiskBca_801D0CF0()
{
    return 0;
}
PPC_NATIVE_OVERRIDE(801D0CF0, DVDCheckDiskBca_801D0CF0, int32_t, (), ());"""
    write(dvd, text.replace(anchor, block, 1))
    report.append("BCA check answered natively")


def patch_macos_netplay(repo: pathlib.Path, report: list[str]) -> None:
    start = repo / "runtime/src/netplay/netplay_start.cpp"
    text = read(start)
    if "_NSGetExecutablePath" in text:
        report.append("macOS netplay restart already patched")
        return

    old_inc = """#elif defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <unistd.h>
#endif"""
    new_inc = """#elif defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <unistd.h>
#include <vector>
#endif"""
    if old_inc in text:
        text = text.replace(old_inc, new_inc, 1)

    anchor = """        char* const argv[] = {exe, nullptr};
        execv(exe, argv);
    }
    RT_LOGF(RT_TAG_RUNTIME, "netplay: could not start the game again\\n");
#endif
    std::_Exit(0);
}"""
    apple = """        char* const argv[] = {exe, nullptr};
        execv(exe, argv);
    }
    RT_LOGF(RT_TAG_RUNTIME, "netplay: could not start the game again\\n");
#elif defined(__APPLE__)
    // macOS: the same program again in this process's place. There is no /proc, so every descriptor
    // from 3 up is marked close-on-exec instead: the lobby's LAN socket must not survive, or the
    // session cannot bind port 52130. The scripted-lobby variable is dropped so a test run does not
    // open a second room after the restart.
    char exe[PATH_MAX] = {};
    uint32_t exeSize = sizeof(exe);
    if (_NSGetExecutablePath(exe, &exeSize) == 0) {
        EndProcessTranscript();
        for (int fd = 3; fd < 512; ++fd) {
            fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
        }
        unsetenv("NSMBW_LOBBY_TEST");
        char* const argv[] = {exe, nullptr};
        execv(exe, argv);
    }
    RT_LOGF(RT_TAG_RUNTIME, "netplay: could not start the game again\\n");
#endif
    std::_Exit(0);
}"""
    if anchor not in text:
        sys.exit("cannot apply the netplay fix: restart block not found")
    write(start, text.replace(anchor, apple, 1))
    report.append("macOS netplay restart added")


LABEL_BLOCK = """

// Which family's names the runtime prints for a physical button. Auto keeps the positional label
// that names both families ("South (A / Cross)"); the others print one family only, so a player
// can see their own pad's names even when detection guesses wrong.
enum class Layout : int32_t { Auto = 0, PlayStation = 1, Xbox = 2, Nintendo = 3 };

inline Layout& LayoutStorage() { static Layout value = Layout::Auto; return value; }
inline Layout GetLayout() { return LayoutStorage(); }
inline void SetLayout(Layout value) { LayoutStorage() = value; }

inline std::string LabelFor(const NativeButtonItem& item) {
    const Layout layout = GetLayout();
    if (layout == Layout::Auto) {
        return item.label;
    }
    struct Entry { const char* key; const char* playstation; const char* xbox; const char* nintendo; };
    static constexpr Entry kEntries[] = {
        {"south", "Cross (\\u2715)", "A", "B"},
        {"east", "Circle (\\u25CB)", "B", "A"},
        {"west", "Square (\\u25A1)", "X", "Y"},
        {"north", "Triangle (\\u25B3)", "Y", "X"},
        {"back", "Create", "View", "\\u2212"},
        {"start", "Options", "Menu", "+"},
        {"guide", "PS", "Guide", "Home"},
        {"left_shoulder", "L1", "LB", "L"},
        {"right_shoulder", "R1", "RB", "R"},
        {"left_trigger", "L2", "LT", "ZL"},
        {"right_trigger", "R2", "RT", "ZR"},
        {"left_stick", "L3", "L3", "L3"},
        {"right_stick", "R3", "R3", "R3"},
        {"misc1", "Share", "Share", "Mic"},
        {"touchpad", "Touchpad", "Touchpad", "Touchpad"},
    };
    const std::string_view key = item.configName;
    for (const auto& entry : kEntries) {
        if (key != entry.key) {
            continue;
        }
        switch (layout) {
        case Layout::PlayStation: return entry.playstation;
        case Layout::Xbox: return entry.xbox;
        case Layout::Nintendo: return entry.nintendo;
        default: break;
        }
    }
    return item.label;
}

inline std::string LabelForValue(uint32_t nativeButton) {
    return LabelFor(NativeButtonForValue(nativeButton));
}"""


def patch_labels(repo: pathlib.Path, report: list[str]) -> None:
    names = repo / "runtime/include/controller_button_names.h"
    text = read(names)
    if "enum class Layout" in text:
        report.append("button-label layouts already patched")
        return
    anchor = """inline const NativeButtonItem& NativeButtonForValue(uint32_t nativeButton) {
    const auto it = std::find_if(kNativeButtons.begin(), kNativeButtons.end(),
                                 [&](const NativeButtonItem& item) { return PADAxisButtonIdentity(nativeButton) == PADAxisButtonIdentity(item.nativeButton); });
    return it == kNativeButtons.end() ? *FindNativeButton("unmapped") : *it;
}"""
    if anchor not in text:
        sys.exit("cannot apply the button-labels fix: label table not found")
    write(names, text.replace(anchor, anchor + LABEL_BLOCK, 1))
    report.append("button-label layouts added")


def patch_label_config(repo: pathlib.Path, report: list[str]) -> None:
    cfg = repo / "runtime/include/runtime_config.h"
    text = read(cfg)
    if "buttonLayout" in text:
        return
    text = text.replace(
        "    std::optional<bool> rumbleEnabled;",
        "    std::optional<bool> rumbleEnabled;\n"
        "    // 0 auto, 1 PlayStation, 2 Xbox, 3 Nintendo. [controller] button_layout.\n"
        "    std::optional<int32_t> buttonLayout;", 1)
    text = text.replace(
        '    config.rumbleEnabled = FindConfigValue<bool>(document, "controller", "rumble");',
        '    config.rumbleEnabled = FindConfigValue<bool>(document, "controller", "rumble");\n'
        '    if (auto value = FindConfigInt(document, "controller", "button_layout"); value && *value >= 0 && *value <= 3) {\n'
        "        config.buttonLayout = *value;\n"
        "    }", 1)
    text = text.replace(
        """inline bool SetRumbleEnabled(bool value) {
    Mutable().rumbleEnabled = value;
    return WriteSetting("controller", "rumble", value ? "true" : "false");
}""",
        """inline bool SetRumbleEnabled(bool value) {
    Mutable().rumbleEnabled = value;
    return WriteSetting("controller", "rumble", value ? "true" : "false");
}

inline int32_t ButtonLayout(int32_t fallback = 0) {
    return Get().buttonLayout.value_or(fallback);
}

inline bool SetButtonLayout(int32_t value) {
    Mutable().buttonLayout = value;
    return WriteSetting("controller", "button_layout", std::to_string(value));
}""", 1)
    write(cfg, text)

    main = repo / "runtime/src/main.cpp"
    text = read(main)
    if "ControllerNames::SetLayout" not in text:
        text = text.replace('#include "abi_bridge.h"',
                            '#include "abi_bridge.h"\n#include "controller_button_names.h"', 1)
        text = text.replace(
            "        DisplaySettings::Initialize(displaySettings);",
            "        DisplaySettings::Initialize(displaySettings);\n"
            "        static constexpr const char* kButtonLayoutNames[] = {\"auto\", \"playstation\", \"xbox\", \"nintendo\"};\n"
            "        const int32_t buttonLayoutIndex = RuntimeConfigFile::ButtonLayout(0);\n"
            "        ControllerNames::SetLayout(static_cast<ControllerNames::Layout>(buttonLayoutIndex));\n"
            '        RT_LOGF(RT_TAG_RUNTIME, "button labels: %s\\n", kButtonLayoutNames[buttonLayoutIndex]);', 1)
        write(main, text)

    overlay = repo / "runtime/src/settings_overlay.cpp"
    text = read(overlay)
    if "Show names as" in text:
        return
    text = text.replace(
        """    uint32_t mappingCount = 0;
    PADButtonMapping* mappings = PADGetButtonMappings(static_cast<uint32_t>(g_controllerPort), &mappingCount);""",
        """    ImGui::SeparatorText("Button labels");
    static int g_buttonLabelLayout = RuntimeConfigFile::ButtonLayout(0);
    static constexpr const char* kButtonLayoutNames[] = {"Auto (name both)", "PlayStation", "Xbox", "Nintendo"};
    if (ImGui::Combo("Show names as", &g_buttonLabelLayout, kButtonLayoutNames,
                     static_cast<int>(std::size(kButtonLayoutNames)))) {
        ControllerNames::SetLayout(static_cast<ControllerNames::Layout>(g_buttonLabelLayout));
        RuntimeConfigFile::SetButtonLayout(g_buttonLabelLayout);
        Keybinds::NoteBindingsChanged();
    }

    uint32_t mappingCount = 0;
    PADButtonMapping* mappings = PADGetButtonMappings(static_cast<uint32_t>(g_controllerPort), &mappingCount);""", 1)
    replacements = [
        ("std::string(isDefault ? kRemoteActionDefaults[i] : NativeButtonForValue(native).label)",
         "std::string(isDefault ? kRemoteActionDefaults[i] : ControllerNames::LabelForValue(native))"),
        ('const std::string primaryCaption = std::string(current.label) + "##primary";',
         'const std::string primaryCaption = ControllerNames::LabelFor(current) + "##primary";'),
        ('const char* altLabel = altBound ? NativeButtonForValue(altIt->nativeButton).label : "None";',
         'const std::string altLabel = altBound ? ControllerNames::LabelForValue(altIt->nativeButton) : "None";'),
        ("return ShortButtonLabel(NativeButtonForValue(native).label);",
         "const std::string label = ControllerNames::LabelForValue(native);\n"
         "        return ShortButtonLabel(label.c_str());"),
        ("return ShortButtonLabel(NativeButtonForValue(mappings[i].nativeButton).label);",
         "const std::string label = ControllerNames::LabelForValue(mappings[i].nativeButton);\n"
         "            return ShortButtonLabel(label.c_str());"),
    ]
    for old, new in replacements:
        text = text.replace(old, new)
    write(overlay, text)
    report.append("F10 button-label picker added")


def patch_lan_beta(repo: pathlib.Path, report: list[str]) -> None:
    """Mark LAN play as beta wherever the player meets it.

    Rooms work (verified on one Mac with two instances and on Windows), but a room can fail to
    start or drop, so the label belongs in front of the player rather than in a changelog.
    """
    ui = repo / "runtime/src/netplay/netplay_ui.cpp"
    text = read(ui)
    if "LAN (BETA)" in text:
        report.append("LAN beta label already applied")
    else:
        text = text.replace('    Title(c, "LAN", 0, 112, 40);',
                            '    Title(c, "LAN (BETA)", 0, 112, 40);', 1)
        text = text.replace('    Text(c, "Tailscale works, but is unstable.", 0, -38, 22, 1, 0xFFF0B0FFu);',
                            '    Text(c, "BETA: rooms can fail to start.", 0, -38, 22, 1, 0xFFF0B0FFu);\n'
                            '    Text(c, "Tailscale works, but is unstable.", 0, -70, 20, 1, 0xFFF0B0FFu);', 1)
        write(ui, text)
        report.append("LAN screen marked beta")

    overlay = repo / "runtime/src/settings_overlay.cpp"
    text = read(overlay)
    if "LAN play is in beta" in text:
        return
    anchor = '    ImGui::TextUnformatted("Rooms are found on your Wi-Fi by themselves, and over Tailscale through");'
    if anchor not in text:
        return
    text = text.replace(anchor,
                        '    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "LAN play is in beta: rooms can fail to start or drop.");\n'
                        '    ImGui::Separator();\n' + anchor, 1)
    write(overlay, text)
    report.append("F10 LAN settings marked beta")


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = sys.argv[1:]
    disc: pathlib.Path | None = None
    if "--disc" in flags:
        at = flags.index("--disc")
        if at + 1 >= len(flags):
            sys.exit("--disc needs the path of an extracted disc (one holding sys/ and files/)")
        disc = pathlib.Path(flags[at + 1]).resolve()
        args = [a for a in args if a != flags[at + 1]]

    if len(args) != 1:
        sys.exit(__doc__)
    repo = pathlib.Path(args[0]).resolve()
    if not (repo / "runtime/src/hle/storage/dvd.cpp").exists():
        sys.exit(f"{repo} does not look like the recompilation checkout")

    report: list[str] = []
    patch_bca(repo, report)
    patch_macos_netplay(repo, report)
    patch_labels(repo, report)
    patch_label_config(repo, report)
    patch_lan_beta(repo, report)
    if disc is not None:
        patch_variant_hash(repo, disc, report)
    for line in report:
        print("    •", line)


if __name__ == "__main__":
    main()
