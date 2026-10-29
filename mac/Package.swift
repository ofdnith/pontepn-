// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "Ponte",
    platforms: [.macOS(.v13)],
    products: [
        .library(name: "PonteCore", targets: ["PonteCore"]),
        .library(name: "PonteServer", targets: ["PonteServer"]),
        .executable(name: "ponte-server", targets: ["ponte-server"]),
        .executable(name: "PonteMac", targets: ["PonteMac"]),
    ],
    targets: [
        // Identificador de formatos, biblioteca, perfis, estado. Só Foundation.
        .target(name: "PonteCore"),
        // Servidor HTTP + descoberta UDP + pareamento (sockets POSIX).
        .target(name: "PonteServer", dependencies: ["PonteCore"]),
        // Servidor de linha de comando, com as mesmas opções da referência em Python.
        .executableTarget(name: "ponte-server", dependencies: ["PonteServer"]),
        // App de barra de menu (SwiftUI, só macOS).
        .executableTarget(name: "PonteMac", dependencies: ["PonteServer"]),
        .testTarget(name: "PonteCoreTests", dependencies: ["PonteCore"]),
        .testTarget(name: "PonteServerTests", dependencies: ["PonteServer"]),
    ]
)
