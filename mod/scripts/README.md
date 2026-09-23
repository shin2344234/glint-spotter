# Release scripts

The scripts in this folder are synced from a shared release process used
across several mods. Do not edit them here; a sync overwrites local changes.

`ReleaseConfig.ps1` and `release_config.py` read the mod's name, paths and
ids from `release.json` at the repo root, so the other scripts here do not
hard-code them.

- `sign.ps1` signs the built plugin.
- `package.ps1` runs the project's checks, builds the release archives, and
  records their checksums.
- `release-docs.py` drafts, fills in, and checks the release notes kept
  outside this repo.
- `vtscan.py` submits the archive for a virus scan and prints the results.
- `publish-github.ps1` tags the release and publishes it on GitHub.
- `publish-nexus.ps1` calls `Publish-NexusModUpdate.ps1` to put the files on
  Nexus Mods.
- `nexus-ids.py` reads back the Nexus ids a release needs.
- `announce-discord.py` posts the release announcement.

Keys for these scripts live in `keys.local.env` beside them, which is
gitignored. `keys.local.env.example` says which ones and where to get them.
