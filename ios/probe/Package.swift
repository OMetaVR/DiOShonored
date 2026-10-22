// swift-tools-version: 6.0
// Dishonored iOS platform probe. Build with xtool (see research/platform/ios_toolchain.md):
//   xtool dev build --ipa          (unsigned .ipa for SideStore + LiveContainer)
// scripts/build_ipa.sh wraps this and then embeds MoltenVK / DXVK / SDL3 into the .ipa.
import PackageDescription

let package = Package(
    name: "Probe",
    platforms: [.iOS(.v15)],
    products: [
        // xtool wants exactly one library product: the app.
        .library(name: "Probe", targets: ["Probe"]),
    ],
    targets: [
        // Shared C++ d3d9 scene (symlinked from tools/spikes/d3d9_test; DXVK-native headers from dxvk_include).
        .target(
            name: "D3D9Scene",
            exclude: ["dxvk_include"],
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("dxvk_include"),
                .unsafeFlags(["-std=c++17", "-Wno-unknown-pragmas"]),
            ]
        ),
        // Objective-C: all probe logic + UI. No Swift needed beyond the @main shim.
        .target(
            name: "ProbeCore",
            dependencies: ["D3D9Scene"],
            cSettings: [.unsafeFlags(["-fobjc-arc"])],
            linkerSettings: [
                .linkedFramework("UIKit"), .linkedFramework("Foundation"), .linkedFramework("QuartzCore"),
                .linkedFramework("Metal"), .linkedFramework("GameController"),
                .linkedFramework("CoreGraphics"),
                // xtool's swiftc links with `-platform_version ios 15.0 15.0` (sdk = min). Report the real SDK so
                // iOS does not apply old-SDK compatibility behaviour.
                .unsafeFlags(["-Xlinker", "-platform_version", "-Xlinker", "ios", "-Xlinker", "15.0", "-Xlinker", "27.1"]),
            ]
        ),
        .target(name: "Probe", dependencies: ["ProbeCore"]),
    ],
    cxxLanguageStandard: .cxx17
)
