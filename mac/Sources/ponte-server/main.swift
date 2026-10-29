// Servidor Ponte de linha de comando: as mesmas opções do servidor de referência em Python,
// para rodar o teste de conformidade (tools/conformance.py) e para usar sem o app.
import Foundation
import PonteCore
import PonteServer

setvbuf(stdout, nil, _IOLBF, 0)

let usage = """
uso: ponte-server --library PASTA [opções]

  --library PASTA          pasta da biblioteca de jogos (obrigatório)
  --port N                 porta HTTP (47101)
  --discovery-port N       porta da descoberta UDP (47100); 0 = qualquer livre
  --no-discovery           sem descoberta UDP
  --name NOME              nome mostrado nos consoles (nome do computador)
  --no-pairing             modo "confiar na rede local"
  --state-dir PASTA        estado: id, consoles pareados, hashes (~/.ponte)
  --profiles PASTA         perfis de compatibilidade (profiles/ do repositório)
  --stable-seconds S       tempo parado para considerar estável (10)
  --rescan S               intervalo entre varreduras (5)
  --convert-folders        converter pastas de PS5 em .exfat antes do catálogo (só macOS)
  --extract                extrair compactados
  --delete-originals       apagar originais em vez de mover para _originais/
  --no-hashes              não calcular sha256 (os consoles conferem só o tamanho)
"""

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data("ponte-server: \(message)\n\n\(usage)\n".utf8))
    exit(2)
}

var args = Array(CommandLine.arguments.dropFirst())
var library: String?
var port = 47101
var discoveryPort: Int? = 47100
var name = ProcessInfo.processInfo.hostName.replacingOccurrences(of: ".local", with: "")
var pairing = true
var stateDir = (NSHomeDirectory() as NSString).appendingPathComponent(".ponte")
var profiles: String?
var stableSeconds = 10.0
var rescan = 5.0
var convert = false
var extract = false
var keepOriginals = true
var hashes = true

func value(_ flag: String) -> String {
    guard !args.isEmpty else { fail("\(flag) precisa de um valor") }
    return args.removeFirst()
}

while !args.isEmpty {
    let a = args.removeFirst()
    switch a {
    case "--library": library = value(a)
    case "--port": port = Int(value(a)) ?? { fail("--port inválido") }()
    case "--discovery-port": discoveryPort = Int(value(a)) ?? { fail("--discovery-port inválido") }()
    case "--no-discovery": discoveryPort = nil
    case "--name": name = value(a)
    case "--no-pairing": pairing = false
    case "--state-dir": stateDir = value(a)
    case "--profiles": profiles = value(a)
    case "--stable-seconds": stableSeconds = Double(value(a)) ?? { fail("--stable-seconds inválido") }()
    case "--rescan": rescan = Double(value(a)) ?? { fail("--rescan inválido") }()
    case "--convert-folders": convert = true
    case "--extract": extract = true
    case "--delete-originals": keepOriginals = false
    case "--no-hashes": hashes = false
    case "-h", "--help":
        print(usage)
        exit(0)
    default: fail("opção desconhecida: \(a)")
    }
}

guard let library else { fail("informe --library") }
guard isDirectoryPath(library) else { fail("a biblioteca não existe: \(library)") }

func isDirectoryPath(_ p: String) -> Bool {
    var dir: ObjCBool = false
    return FileManager.default.fileExists(atPath: p, isDirectory: &dir) && dir.boolValue
}

/// profiles/ do repositório: ao lado do executável (.build/…) ou na pasta atual.
func defaultProfiles() -> String {
    var url = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath().deletingLastPathComponent()
    for _ in 0..<6 {
        let candidate = url.appendingPathComponent("profiles").path
        if isDirectoryPath(candidate) { return candidate }
        url.deleteLastPathComponent()
    }
    return (FileManager.default.currentDirectoryPath as NSString).appendingPathComponent("profiles")
}

var config = PonteConfig(library: (library as NSString).standardizingPath, profilesDir: profiles ?? defaultProfiles(),
                         stateDir: stateDir, name: name)
config.port = port
config.discoveryPort = discoveryPort
config.pairing = pairing
config.stableSeconds = stableSeconds
config.rescanInterval = rescan
config.convertFolders = convert
config.computeHashes = hashes
config.extractArchives = extract
config.keepOriginals = keepOriginals

let server: PonteServer
do {
    server = try PonteServer(config: config)
    try server.start()
} catch {
    fail("não foi possível iniciar: \(error)")
}

print("Ponte em http://0.0.0.0:\(server.port)  — descoberta UDP \(server.discoveryPort.map(String.init) ?? "desligada")")
print("Biblioteca: \(config.library)  (\(server.library.items().count) itens prontos)")
print("Perfis: \(server.profiles.map { "\($0.platform) \($0.firmware)" }.joined(separator: ", "))")
if pairing { print("Código de pareamento: \(server.code)") }

signal(SIGINT) { _ in exit(0) }
signal(SIGTERM) { _ in exit(0) }
while true { sleep(3600) }
