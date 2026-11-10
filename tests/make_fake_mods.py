"""Fake mods for the GZPro-Registry sync tests (tests/run_tests.sh; never installed in the game).
    make_fake_mods.py <Kiryu tools folder> <the game's AssetRegistry.bin> <out Paks folder>     (REPAK=<repak binary>)
B  GZPro-TestCostume_P   own registry: + DA_GZ1_Skin_TestB (package in its utoc) + DA_GZ1_Skin_Ghost (package NOT in it)
D  GZPro-Dup_1_P         own registry: + DA_GZ1_Skin_TestB too (higher pak order: its copy should win)
C  ~mods/GZPro-ManifestMod_P   manifest only: TestC from Mechagodzilla74_Black, TestC2 (per-source rules), one bad source
M  GZPro-Music_P         a pak with no registry and no manifest (ignored)
"""
import json, os, subprocess, sys, tempfile
sys.path.insert(0, sys.argv[1])
import assetregistry as ar, iostore  # noqa: E402

REPAK = os.environ["REPAK"]
base_bin, out = sys.argv[2], sys.argv[3]
base = open(base_bin, "rb").read()
G2K = "/Game/Characters/GZ1_Monsters/Godzilla2K/DA_GZ1_Skin_G2KBurning.DA_GZ1_Skin_G2KBurning"


def container(stem, folder, packages):
    toc = iostore.TocWriter(stem)
    hdr = iostore.ContainerHeader()
    for pkg in packages:
        pid = iostore.package_id(pkg)
        toc.add(iostore.chunk_id(pid, 0, iostore.CHUNK_EXPORT_BUNDLE_DATA), b"fake package " + pkg.encode(),
                "GZPro/Content/" + pkg[len("/Game/"):] + ".uasset")
        hdr.add(pid, [])
    toc.write(os.path.join(folder, stem + ".utoc"), os.path.join(folder, stem + ".ucas"), hdr)


def pak(stem, folder, files):
    with tempfile.TemporaryDirectory() as td:
        for rel, data in files.items():
            p = os.path.join(td, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "wb").write(data)
        subprocess.run([REPAK, "pack", "-q", "--version", "V11", "--mount-point", "../../../", td,
                        os.path.join(folder, stem + ".pak")], check=True)


def registry_with(*names):
    r = ar.Registry(base)
    for pkg in names:
        r.add_asset_like(G2K, pkg, pkg.rsplit("/", 1)[1], {"PrimaryAssetName": ("numberless_name", pkg.rsplit("/", 1)[1])})
    return r.serialize()


os.makedirs(os.path.join(out, "~mods"), exist_ok=True)
B = "/Game/Characters/GZ1_Monsters/Godzilla2K/DA_GZ1_Skin_TestB"
GHOST = "/Game/Characters/GZ1_Monsters/Godzilla2K/DA_GZ1_Skin_Ghost"
pak("GZPro-TestCostume_P", out, {"GZPro/AssetRegistry.bin": registry_with(B, GHOST)})
container("GZPro-TestCostume_P", out, [B])
pak("GZPro-Dup_1_P", out, {"GZPro/AssetRegistry.bin": registry_with(B)})
container("GZPro-Dup_1_P", out, [B])
manifest = {
    "format": 1, "id": "TestC", "title": "Test costume C",
    "sources": [
        "/Game/Characters/GZ1_Monsters/Mechagodzilla/DA_GZ1_Skin_Mechagodzilla74_Black.DA_GZ1_Skin_Mechagodzilla74_Black",
        {"source": "/Game/Characters/GZ1_Monsters/Mechagodzilla/DA_GZ1_Skin_Mechagodzilla74.DA_GZ1_Skin_Mechagodzilla74",
         "rules": [["DA_GZ1_Skin_Mechagodzilla74", "DA_GZ1_Skin_TestC2"]]},
        "/Game/Characters/GZ1_Monsters/Nobody/DA_GZ1_Skin_Nobody.DA_GZ1_Skin_Nobody",
    ],
    "rules": [["DA_GZ1_Skin_Mechagodzilla74_Black", "DA_GZ1_Skin_TestC"]],
}
C1 = "/Game/Characters/GZ1_Monsters/Mechagodzilla/DA_GZ1_Skin_TestC"
C2 = "/Game/Characters/GZ1_Monsters/Mechagodzilla/DA_GZ1_Skin_TestC2"
pak("GZPro-ManifestMod_P", os.path.join(out, "~mods"), {"GZPro/RegistryMods/TestC.json": json.dumps(manifest, indent=1).encode()})
container("GZPro-ManifestMod_P", os.path.join(out, "~mods"), [C1, C2])
pak("GZPro-Music_P", out, {"GZPro/Content/Mods/Probe/ModActor.uasset": b"not a real asset"})
container("GZPro-Music_P", out, [])
print("fake mods written to", out)
