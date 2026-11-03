# Announces a published Spidy release in #spidy-updates on Discord, through the channel's webhook
# "Spidy Releases". .github/workflows/release.yml runs it once a release is created; the webhook's address
# is the repository secret DISCORD_SPIDY_RELEASES_WEBHOOK, passed in the environment as DISCORD_WEBHOOK.
# The post pings the opt-in role "Spidy releases" and nobody else, and lists the commits the release notes
# list. Without a webhook address it says so and does nothing.
#   .\tools\announce_release.ps1 -Version 0.3.2 -NotesFile release-notes.md -DryRun   print the post, send nothing
param(
    [Parameter(Mandatory)][string]$Version,
    [string]$Tag = "v$Version",
    [string]$NotesFile,
    [string]$Repository = 'IlyaMez/SpidyVR',
    [string]$RoleId = '1558016092197355620',       # the role "Spidy releases" (opt-in, not mentionable by members)
    [string]$HelpChannel = '1558067917310922814',  # #spidy-bugs (#spidy-help is archived)
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$limit = 1900 # Discord takes 2000 characters

$release = "https://github.com/$Repository/releases/tag/$Tag"
# The changes: the bullet lines under the notes' "## Changes" heading.
$changes = @()
$heading = ''
if ($NotesFile -and (Test-Path $NotesFile)) {
    $inChanges = $false
    foreach ($line in Get-Content $NotesFile -Encoding UTF8) {
        if ($line -match '^## Changes') { $inChanges = $true; $heading = $line.Substring(3); continue }
        if ($inChanges -and $line -match '^- \S') { $changes += $line }
    }
}

$head = @(
    "# Spidy $Version is out"
    "<@&$RoleId>"
    "**Update:** start the launcher and press **Update now**. New here? Download: <$release>"
)
$foot = "-# Something wrong after updating? Report it in <#$HelpChannel>."
$body = @()
if ($changes.Count) {
    $body += ''
    $body += "**$heading**"
    $room = $limit - (($head + $foot) -join "`n").Length - $heading.Length - 80
    $shown = 0
    foreach ($line in $changes) {
        if ($line.Length + 1 -gt $room) { break }
        $body += $line
        $room -= $line.Length + 1
        $shown++
    }
    if ($shown -lt $changes.Count) { $body += "...and $($changes.Count - $shown) more on the release page." }
}
$content = ($head + $body + '' + $foot) -join "`n"

$payload = @{
    content          = $content
    allowed_mentions = @{ parse = @(); roles = @($RoleId) }
    flags            = 4 # no link previews
} | ConvertTo-Json -Depth 5 -Compress

if ($DryRun) {
    Write-Output "$($content.Length) characters:"
    Write-Output $content
    return
}
$webhook = $env:DISCORD_WEBHOOK
if (-not $webhook) {
    Write-Output 'No Discord webhook is configured (the secret DISCORD_SPIDY_RELEASES_WEBHOOK): nothing announced.'
    return
}
if ($webhook -notmatch '^https://(ptb\.|canary\.)?discord(app)?\.com/api/webhooks/\d+/') {
    throw 'DISCORD_WEBHOOK is not a Discord webhook address.'
}
$separator = if ($webhook.Contains('?')) { '&' } else { '?' }
$sent = Invoke-RestMethod -Uri "$webhook${separator}wait=true" -Method Post `
    -ContentType 'application/json; charset=utf-8' -Body ([Text.Encoding]::UTF8.GetBytes($payload))
Write-Output "Announced Spidy $Version on Discord (message $($sent.id))."
