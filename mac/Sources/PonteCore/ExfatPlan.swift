import Foundation

/// Tamanho e cluster da imagem .exfat (SPEC §7.4). Mesma fórmula do mkexfat_macos.sh do
/// ShadowMountPlus.
public struct ExfatPlan: Equatable, Sendable {
    public static let kib: Int64 = 1024
    public static let mib: Int64 = 1024 * 1024
    public static let clusterSmall: Int64 = 32 * 1024
    public static let clusterLarge: Int64 = 64 * 1024

    public let cluster: Int64
    public let sizeMiB: Int64
    public let rawBytes: Int64
    public let fileCount: Int
    public let dirCount: Int

    public var sizeBytes: Int64 { sizeMiB * ExfatPlan.mib }

    public static func chooseCluster(_ fileSizes: [Int64], force64K: Bool = false) -> Int64 {
        if force64K { return clusterLarge }
        guard !fileSizes.isEmpty else { return clusterSmall }
        let avg = Double(fileSizes.reduce(0, +)) / Double(fileSizes.count)
        return avg >= Double(mib) ? clusterLarge : clusterSmall
    }

    public static func make(fileSizes: [Int64], dirCount: Int, force64K: Bool = false) -> ExfatPlan {
        let cluster = chooseCluster(fileSizes, force64K: force64K)
        let raw = fileSizes.reduce(0, +)
        let allocated = fileSizes.reduce(Int64(0)) { $0 + ($1 + cluster - 1) / cluster * cluster }
        let clusters = allocated / cluster
        let fat = clusters * 4
        let bitmap = (clusters + 7) / 8
        let dirents = Int64(fileSizes.count + dirCount) * 256
        let base = Double(allocated + fat + bitmap + dirents + 32 * mib)
        let spare = min(max(base * 0.005, Double(64 * mib)), Double(512 * mib))
        let total = max(base + spare, Double(raw + 64 * mib))
        return ExfatPlan(cluster: cluster, sizeMiB: Int64((total / Double(mib)).rounded(.up)), rawBytes: raw,
                         fileCount: fileSizes.count, dirCount: dirCount)
    }

    /// Plano para uma pasta: todos os arquivos e subpastas (exceto lixo do Finder).
    public static func make(folder: String, force64K: Bool = false) throws -> ExfatPlan {
        let files = try Library.listFiles(folder)
        var dirs = Set<String>()
        for (rel, _, _) in files {
            var parent = (rel as NSString).deletingLastPathComponent
            while !parent.isEmpty {
                dirs.insert(parent)
                parent = (parent as NSString).deletingLastPathComponent
            }
        }
        return make(fileSizes: files.map { $0.2.size }, dirCount: dirs.count, force64K: force64K)
    }
}
