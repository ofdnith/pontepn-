"""Cálculo de tamanho e cluster da imagem .exfat (SPEC §7.4).

Mesma fórmula do mkexfat_macos.sh do ShadowMountPlus. A criação da imagem em
si (mkfile, hdiutil, newfs_exfat) é feita no app do Mac; aqui só o plano.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

KIB = 1024
MIB = 1024 * 1024

CLUSTER_SMALL = 32 * KIB
CLUSTER_LARGE = 64 * KIB


@dataclass(frozen=True)
class ExfatPlan:
    cluster: int
    size_mib: int
    raw_bytes: int
    file_count: int
    dir_count: int

    @property
    def size_bytes(self) -> int:
        return self.size_mib * MIB


def choose_cluster(file_sizes: list[int], force_64k: bool = False) -> int:
    if force_64k:
        return CLUSTER_LARGE
    if not file_sizes:
        return CLUSTER_SMALL
    avg = sum(file_sizes) / len(file_sizes)
    return CLUSTER_LARGE if avg >= MIB else CLUSTER_SMALL


def plan_exfat(file_sizes: list[int], dir_count: int, force_64k: bool = False) -> ExfatPlan:
    cluster = choose_cluster(file_sizes, force_64k)
    raw = sum(file_sizes)
    allocated = sum(math.ceil(s / cluster) * cluster for s in file_sizes)
    clusters = allocated // cluster
    fat = clusters * 4
    bitmap = math.ceil(clusters / 8)
    dirents = (len(file_sizes) + dir_count) * 256
    base = allocated + fat + bitmap + dirents + 32 * MIB
    spare = min(max(base * 0.005, 64 * MIB), 512 * MIB)
    total = max(base + spare, raw + 64 * MIB)
    return ExfatPlan(cluster, math.ceil(total / MIB), raw, len(file_sizes), dir_count)


def mac_commands(plan: ExfatPlan, image: str, source_dir: str, mount_point: str) -> list[str]:
    """Os comandos que o app do Mac executa, na ordem (documentação executável)."""
    return [
        f'mkfile -n {plan.size_mib}m "{image}"',
        f'hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount -nobrowse "{image}"',
        f"newfs_exfat -b {plan.cluster} -v <TITLE_ID> <dispositivo>",
        f'diskutil mount -mountPoint "{mount_point}" <dispositivo>',
        f'# copiar "{source_dir}/" para "{mount_point}/" arquivo a arquivo, sem xattrs nem .DS_Store',
        f'diskutil unmount "{mount_point}"',
        "hdiutil detach <dispositivo>",
    ]
