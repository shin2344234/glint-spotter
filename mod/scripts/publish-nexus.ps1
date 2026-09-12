<#
.SYNOPSIS
    Publish the current release to the Glint Spotter mod page.

.DESCRIPTION
    A wrapper around Publish-NexusModUpdate.ps1 that fills in the three things
    that never change and the two that follow from the version, so a release
    does not depend on remembering an id.

        Mod id  <fill in>        the v3 id, not the number in the page URL
        File id <fill in>        the active "GlintSpotter" file entry

    Neither is known until the mod page exists. Get both from

        py -3 nexus-ids.py

    with PAGE_ID set in that script, then paste them into the two places at the
    bottom of this file. They are stable for the life of the mod page. The file id is the one worth being careful about: point a
    release at the wrong one and it attaches as a version of some other file.
    Check it with

        py -3 nexus-ids.py

    if the page is ever restructured.

    The version comes from mod/src/version.h, and the archive and changelog are
    derived from it, so this only ever publishes what package.py built.

    Report only unless you pass -Apply, same as the script it wraps.

    The previous version is left listed, never archived. Archiving hides it,
    and people who need an older build (kfen72 asked, 9 September 2026) then
    have nothing to download. What it should become is an Old files entry,
    and the v3 API cannot do that: updateModFile changes the name and nothing
    else, and the category enum for a new file has no old_version value. So
    after -Apply, open Manage Files on the mod page and move the previous
    version to Old files by hand. -ArchivePrevious is there for the one case
    where an old build must be pulled outright.

.EXAMPLE
    .\publish-nexus.ps1
    .\publish-nexus.ps1 -Apply
#>
[CmdletBinding()]
param(
    # Nothing is sent to Nexus without this.
    [switch] $Apply,

    # Override the version read from version.h.
    [string] $Version,

    # Archive the previous version. Off by default: archiving hides it, and an
    # older build should stay downloadable under Old files, which is a manual
    # move on the site because the API cannot set that category.
    [switch] $ArchivePrevious
)

$ErrorActionPreference = 'Stop'

$here = $PSScriptRoot
$mod  = Split-Path $here -Parent
$repo = Split-Path $mod -Parent

if ([string]::IsNullOrWhiteSpace($Version)) {
    $header = Join-Path $mod 'src\version.h'
    $match  = Select-String -LiteralPath $header -Pattern '#define\s+GS_VERSION_STRING\s+"([^"]+)"'
    if (-not $match) { throw "No GS_VERSION_STRING in $header" }
    $Version = $match.Matches[0].Groups[1].Value
}

$archive   = Join-Path $mod  ("dist\GlintSpotter-{0}.zip" -f $Version)
$changelog = Join-Path $repo ("private\nexus\nexus-changelog-{0}.txt" -f $Version)

if (-not (Test-Path -LiteralPath $archive)) {
    throw "No archive at $archive. Run package.py first."
}
if (-not (Test-Path -LiteralPath $changelog)) {
    throw "No changelog at $changelog. Write it before publishing."
}

Write-Host ("Version $Version, from version.h") -ForegroundColor Cyan

# The changelog endpoint appends rather than replaces, so a second run for one
# version posts the text twice. Check what is already up there and refuse
# rather than leave a duplicated page to clean up by hand.

# The two ids from nexus-ids.py, in one place. The check below reached for
# $ids.FileId while the only copy lived in the hashtable at the bottom, so the
# check asked for /mod-files//versions, got a 404, and took the publish down
# with it.
$ids = @{
    FileId = '7953159'          # the active GlintSpotter file entry
    ModId  = '38521561681296'   # the v3 mod id, not the number in the page URL
}

if ($Apply) {
    $key = $env:NEXUS_API_KEY
    if ([string]::IsNullOrWhiteSpace($key)) {
        $keyFile = Join-Path $here 'keys.local.env'
        if (Test-Path -LiteralPath $keyFile) {
            foreach ($line in Get-Content -LiteralPath $keyFile) {
                $t = $line.Trim()
                if ($t -match '^\s*(#|$)') { continue }
                $n, $v = $t -split '=', 2
                if ($n.Trim() -eq 'NEXUS_API_KEY') { $key = $v.Trim().Trim('"').Trim("'"); break }
            }
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($key)) {
        try {
            $existing = Invoke-RestMethod -Uri "https://api.nexusmods.com/v3/mod-files/$($ids.FileId)/versions" `
                                          -Headers @{ 'apikey' = $key } -Method Get
            $already = $existing.data.versions | Where-Object { $_.version -eq $Version }
            if ($already) {
                Write-Host ""
                Write-Host ("Version {0} is already on the mod page, uploaded {1}." -f $Version, $already[0].uploaded_at) -ForegroundColor Red
                Write-Host "Publishing it again would list a second copy and post the changelog twice." -ForegroundColor Red
                Write-Host "Nothing was sent. Bump version.h and rebuild, or pass -Version for a different one." -ForegroundColor Red
                exit 1
            }
        } catch {
            Write-Warning ("Could not check what is already published ({0}); continuing." -f $_.Exception.Message)
        }
    }
}

$args = @{
    FilePath                  = $archive
    FileId                    = $ids.FileId
    ModId                     = $ids.ModId
    Version                   = $Version
    DisplayName               = ("GlintSpotter {0}" -f $Version)
    ChangelogPath             = $changelog
    Category                  = 'main'
    UpdateModVersion          = $true
    PrimaryModManagerDownload = $true
}
if ($ArchivePrevious) { $args['ArchiveExistingFile'] = $true }
if ($Apply)             { $args['Apply']               = $true }

& (Join-Path $here 'Publish-NexusModUpdate.ps1') @args

if ($Apply) {
    Write-Host ""
    Write-Host "Still manual, because the v3 API has no endpoint for either:" -ForegroundColor Yellow
    Write-Host "  the page description  -> private\nexus\nexus-description.bbcode"
    Write-Host ("  the update post       -> private\nexus\nexus-post-{0}.txt" -f $Version)
    Write-Host "  the previous version  -> Manage Files, change its category to Old files (it is still listed as Main)"
}
