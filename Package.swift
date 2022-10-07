// swift-tools-version: 5.6
// The swift-tools-version declares the minimum version of Swift required to build this package.
//
// lz4-cpp-SwiftPM
//

import PackageDescription

let package = Package(
    name: "lz4-cpp",
    products: [
        .library(
            name: "lz4",
            targets: ["lz4"]),
    ],
    targets: [
        .target(
            name: "lz4",
            path: "/",
            exclude: [
                "build/",
                "contrib/",
                "doc/",
                "examples/",
                "lib/dll",
                "lib/liblz4-dll.rc.in",
                "lib/liblz4.pc.in",
                "ossfuzz/",
                "programs/",
                "tests/",
                "appveyor.yml",
                "INSTALL",
                "LICENSE",
                "Makefile",
                "Makefile.in",
                "NEWS",
                "README.md",
                "tmp",
                "tmpsparse"
            ],
            publicHeadersPath: "include")
    ]
)
