# Release scripts

Adapted from Master Looter's, so a fix to either can be carried over by reading
one diff. Everything here reports by default and changes nothing without
`--apply` or `-Apply`.

Keys live in `keys.local.env` beside these scripts, which is gitignored.
`keys.local.env.example` says which ones and where to get them. Copying that
file over from Master Looter works: the three keys are the same account.

## The order

```
py -3 prose_check.py <docs>   check the release prose before any of it goes out
mod\build.bat                 build the plugin into mod\dist
py -3 package.py              regenerate the ini, zip the archive, print hashes
py -3 vtscan.py --upload      submit the archive and the plugin, wait for a verdict
py -3 vtscan.py --prose       print the numbers and the report links for the page
gh release create v<version> mod\dist\GlintSpotter-<version>.zip
.\publish-nexus.ps1           report what it would send to Nexus
.\publish-nexus.ps1 -Apply    send it
py -3 announce-discord.py --apply
```

`prose_check.py` is ported from Master Looter and checks anything written in
Seth's voice for AI hallmarks: the mechanical bans, a list of recurring tics, and
any sentence that appears in two documents of the same release. Pass the post,
the changelog and the Discord text together, because the cross-document check
only sees the files it is given. `--all` sweeps `private/nexus` and
`private/discord`. It exits non-zero on a hard hit. When one project catches a
new tic, add it to the other copy too, or the two drift apart.

`package.py` rebuilds `GlintSpotter.ini` from the plugin's own `WriteDefaults`
before zipping, so the ini in the archive cannot disagree with the one the
plugin writes on first run. That happened twice before this existed.

## Before the first Nexus publish

`publish-nexus.ps1` needs two ids that only exist once the mod page does, and
they are blank in the file. Put the number from the mod page URL into `PAGE_ID`
in `nexus-ids.py`, run it, and paste the two ids it prints into the bottom of
`publish-nexus.ps1`. The file id is the one worth being careful with: point a
release at the wrong one and it attaches as a version of some other file.

`announce-discord.py` needs the Nexus files URL in `NEXUS_FILES`, also blank
for the same reason. It reads the release text from
`private/discord/release-<version>.txt` and refuses to post without it, or to
post the same version twice.

## What is not here

No signing. Master Looter signs its plugin and refuses to package an unsigned
one; this mod does not, so that check was dropped rather than left to fail.
`mod/scripts/sign.ps1` in that project is the pattern if it is ever wanted.

No second archive. Master Looter ships a plugin-only zip for Definitive Mod
Manager alongside the full one. This ships a single archive, which the manager
handles because the plugin is the only file that has to be deployed.
