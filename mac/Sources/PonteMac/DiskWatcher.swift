#if os(macOS)
import DiskArbitration
import Foundation

/// Quando alguém pede para desmontar o volume da biblioteca (ejetar no Finder, Utilitário de Disco,
/// `diskutil`), o Ponte solta os arquivos antes e aprova. Sem isso o macOS recusa ("o disco está em
/// uso pelo Ponte") e a pessoa acaba puxando o cabo, o que dá o aviso de "disco não ejetado
/// corretamente".
///
/// O pedido chega pelo Disk Arbitration antes da desmontagem de verdade: `release` fecha conexões,
/// para o preparo, a varredura e os hashes, e só então a aprovação volta.
final class DiskWatcher {
    private let session: DASession
    private let queue = DispatchQueue(label: "ponte-disk")
    private var bsdName: String?

    /// Chamado na fila do Disk Arbitration. Tem que soltar o disco antes de retornar.
    var release: (() -> Void)?

    private static let callback: DADiskUnmountApprovalCallback = { _, context in
        guard let context else { return nil }
        let watcher = Unmanaged<DiskWatcher>.fromOpaque(context).takeUnretainedValue()
        watcher.release?()
        return nil  // nil = aprovado
    }

    init?() {
        guard let s = DASessionCreate(kCFAllocatorDefault) else { return nil }
        session = s
        DASessionSetDispatchQueue(session, queue)
    }

    deinit {
        DASessionSetDispatchQueue(session, nil)
    }

    /// Passa a vigiar o volume (nil = nenhum). Só os pedidos desse volume chegam aqui: a imagem de
    /// conversão, que também é um disco, não passa por este caminho.
    func watch(volume url: URL?) {
        queue.async { [self] in
            var name: String?
            if let url, let disk = DADiskCreateFromVolumePath(kCFAllocatorDefault, session, url as CFURL),
               let cname = DADiskGetBSDName(disk) {
                name = String(cString: cname)
            }
            if name == bsdName { return }
            let context = Unmanaged.passUnretained(self).toOpaque()
            if bsdName != nil {
                DAUnregisterCallback(session, unsafeBitCast(DiskWatcher.callback, to: UnsafeMutableRawPointer.self), context)
            }
            bsdName = name
            if let name {
                let match = [kDADiskDescriptionMediaBSDNameKey as String: name] as CFDictionary
                DARegisterDiskUnmountApprovalCallback(session, match, DiskWatcher.callback, context)
            }
        }
    }
}
#endif
