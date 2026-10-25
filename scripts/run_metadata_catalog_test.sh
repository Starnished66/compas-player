#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
run_dir="$(mktemp -d "${TMPDIR:-/tmp}/compas-metadata-catalog.XXXXXX")"
cleanup() { chmod -R u+w "$run_dir" 2>/dev/null || true; rm -rf "$run_dir"; }
trap cleanup EXIT

common_flags=(
    -std=gnu11 -DHOST_BUILD -O1 -g -Wall -Wextra -Werror
    -ffunction-sections -fdata-sections
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-format-truncation -Wno-restrict
    -I"$repo_root" -I"$repo_root/src/library" -I"$repo_root/src/core"
    -I"$repo_root/src/ui" -I"$repo_root/src/audio" -I"$repo_root/src/hardware"
    -I"$repo_root/src/plugins" -I"$repo_root/src/network"
)
sanitize_flags=()
if [[ "${SANITIZE:-0}" == "1" ]]; then
    sanitize_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi

"${CC:-cc}" "${common_flags[@]}" "${sanitize_flags[@]}" -Dpath_cache_drop=path_cache_drop_impl \
    -c "$repo_root/src/library/path_cache.c" -o "$run_dir/path_cache.o"
"${CC:-cc}" "${common_flags[@]}" "${sanitize_flags[@]}" \
    "$repo_root/src/library/metadata_catalog_order_spec.c" \
    "$repo_root/src/library/metadata_db.c" \
    "$repo_root/src/library/tagcache.c" \
    "$repo_root/src/library/albumart.c" \
    "$repo_root/src/library/subsonic_saved_servers.c" \
    "$repo_root/src/network/catalog_source_cache.c" \
    "$repo_root/src/ui/fallback_font.c" \
    "$repo_root/src/core/utf8_util.c" \
    "$repo_root/src/core/db_log.c" \
    "$run_dir/path_cache.o" -pthread -Wl,--gc-sections \
    -o "$run_dir/metadata_catalog_order_spec"

cd "$run_dir"
./metadata_catalog_order_spec

# Exercise same-generation v5/v6 -> v7 derived-index upgrades. The test binary
# seeds a current fixture; these conversions rename it into the historical
# filename schemes and rewrite the matching version headers.
mkdir -p "$run_dir/migration-v6" "$run_dir/migration-v5" "$run_dir/readonly"
(cd "$run_dir/migration-v6" && ../metadata_catalog_order_spec --seed-legacy)
cp -a "$run_dir/migration-v6/." "$run_dir/migration-v5/"
python3 - "$run_dir/migration-v6" "$run_dir/migration-v5" <<'PY'
import pathlib, struct, sys

def downgrade(root, target_version):
    generation = int((root / ".compas/tagcache.gen").read_text().strip())
    parent = root / ".compas"
    base = parent / f"database_idx.tcd.g{generation}.i"
    for path in list(parent.glob(base.name + "*.v7")):
        suffix = path.name[len(base.name):]
        if target_version == 6:
            target_suffix = suffix.replace(".v7", ".v6")
        elif suffix in ("title.v7", "rank0.v7", "query.v7"):
            target_suffix = suffix.replace(".v7", ".v5")
        else:
            target_suffix = suffix[:-3]
        path.rename(parent / (base.name + target_suffix))

    meta_path = parent / f"database_idx.tcd.g{generation}.imeta"
    meta = bytearray(meta_path.read_bytes())
    struct.pack_into("=I", meta, 4, target_version)
    meta_path.write_bytes(meta)

    if target_version == 6:
        album_meta = parent / f"database_idx.tcd.g{generation}.ialbum_meta.v4"
        album_meta.rename(parent / f"database_idx.tcd.g{generation}.ialbum_meta.v3")
        data = bytearray((parent / f"database_idx.tcd.g{generation}.ialbum_meta.v3").read_bytes())
        struct.pack_into("=I", data, 4, 3)
        (parent / f"database_idx.tcd.g{generation}.ialbum_meta.v3").write_bytes(data)
        for path in list(parent.glob(f"database_idx.tcd.g{generation}.ialbum_*.v4")):
            path.rename(path.with_name(path.name[:-3] + ".v3"))
        old_revision = 3
        rank_suffix = ".v6"
    else:
        album_meta = parent / f"database_idx.tcd.g{generation}.ialbum_meta.v4"
        album_meta.rename(parent / f"database_idx.tcd.g{generation}.ialbum_meta")
        data = bytearray((parent / f"database_idx.tcd.g{generation}.ialbum_meta").read_bytes())
        struct.pack_into("=I", data, 4, 2)
        (parent / f"database_idx.tcd.g{generation}.ialbum_meta").write_bytes(data)
        for path in list(parent.glob(f"database_idx.tcd.g{generation}.ialbum_*.v4")):
            path.rename(path.with_name(path.name[:-3]))
        old_revision = 2
        rank_suffix = ".v5"

    # Keep title/rank inverse consistent while forcing the legacy first two
    # rows to disagree with the new title order.
    title_path = parent / f"database_idx.tcd.g{generation}.ititle{rank_suffix}"
    rank_path = parent / f"database_idx.tcd.g{generation}.irank0{rank_suffix}"
    titles = list(struct.unpack("=" + "i" * (title_path.stat().st_size // 4), title_path.read_bytes()))
    ranks = list(struct.unpack("=" + "i" * (rank_path.stat().st_size // 4), rank_path.read_bytes()))
    assert len(titles) >= 2
    first, second = titles[0], titles[1]
    titles[0], titles[1] = second, first
    ranks[first], ranks[second] = 2, 1
    title_path.write_bytes(struct.pack("=" + "i" * len(titles), *titles))
    rank_path.write_bytes(struct.pack("=" + "i" * len(ranks), *ranks))

    group_suffix = ".v6" if target_version == 6 else ""
    desired_slots = (0, 1, 2) if target_version == 6 else (1, 2, 0)
    for kind in (1, 2):
        group_path = parent / f"database_idx.tcd.g{generation}.igroup{kind}{group_suffix}"
        groups = bytearray(group_path.read_bytes())
        assert len(groups) == 60
        records = [groups[offset:offset + 20] for offset in range(0, 60, 20)]
        by_representative = {struct.unpack_from("=i", record)[0]: record for record in records}
        assert set(by_representative) == {0, 1, 2}
        group_path.write_bytes(b"".join(by_representative[slot] for slot in desired_slots))

    revision_path = root / "old_revision.txt"
    revision = revision_path.read_text().strip().rsplit(":", 1)[0]
    revision_path.write_text(f"{revision}:{old_revision}\n")

downgrade(pathlib.Path(sys.argv[1]), 6)
downgrade(pathlib.Path(sys.argv[2]), 5)
PY
cp -a "$run_dir/migration-v6/." "$run_dir/readonly/"
(cd "$run_dir/migration-v6" && ../metadata_catalog_order_spec --verify-migration)
(cd "$run_dir/migration-v5" && ../metadata_catalog_order_spec --verify-migration)
chmod -R u-w "$run_dir/readonly"
(cd "$run_dir/readonly" && ../metadata_catalog_order_spec --verify-readonly-legacy)
