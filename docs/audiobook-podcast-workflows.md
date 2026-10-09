# Audiobook and podcast workflows

The canonical plugins are maintained in the companion `compas-plugins`
repository. This update is Audiobooks 3.5.0 and Podcasts 1.4.0.

## Audiobooks

The Authors and Series shelves use folder names, with natural ordering of titles:

```text
Audiobooks/
  Author/
    Book/01.mp3
    Series/
      01 - First book/01.mp3
      02 - Second book/CD1/01.mp3
  Standalone book/CD1/01.mp3
  Loose book.m4b
```

Disc/part folders stay chapters of their book. Covers and other sidecars do not
prevent an author folder from being recognized. Series names are qualified by
author so identically named series remain separate. Large shelves split into
bounded ranges and book pages, leaving a native list slot available for chapters.
Browse folders remains available for other layouts or libraries beyond scan limits.
Embedded author/series tags are not indexed by these shelves.

Progress keys remain relative paths. Earlier releases sometimes treated an entire
series folder as one book. A saved collection remains available in Continue
listening and its shelf, with its original resume point, bookmarks and history.
Playing it continues to save to that collection; newly discovered individual
books have independent state. Existing records are not automatically split or
rewritten.

## Podcasts

Settings offers saved 0.5–2x speed for downloaded episodes when the firmware
supports it. Each plugin restores its own speed when playback moves between
Audiobooks and Podcasts, after resume seeking settles. Changing a preference
while another source is playing does not change that source's speed. Streams keep
the existing MP3-only, no-resume behavior.

Manage downloads offers All, Not played and Played views. Deleting played
files requires a second tap within eight seconds; the current episode and all
unplayed downloads are retained. Individual deletion also protects the current
file. Files are never pruned automatically.

If a resume seek cannot be confirmed, the saved point is protected from an
incorrect early position. A listener who continues past it can advance progress,
and playback speed remains available. Resume can explicitly retry, with the
same protection even if native playback refuses to restart.

## Validation

From the companion repository, run:

```sh
lua tests/workflow_shelves_test.lua plugins/Audiobooks/Audiobooks.lua plugins/Podcasts/Podcasts.lua
lua tests/podcasts_speed_test.lua plugins/Podcasts/Podcasts.lua plugins/Audiobooks/Audiobooks.lua
python3 tools/build_index.py --check
```

These tests execute the actual plugin scripts, covering legacy collection
ownership after playback, bookmarks, natural order, large shelf navigation,
active/unplayed download protection, refused/failed resume and speed handoff.
