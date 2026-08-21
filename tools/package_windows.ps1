param(
    [Parameter(Mandatory = $false)]
    [string]$Version = "0.2.8",

    [Parameter(Mandatory = $false)]
    [string]$Configuration = "Release",

    [Parameter(Mandatory = $false)]
    [string]$ExpectedQtVersion = "6.10.3"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot ".."))
$normalizedVersion = $Version.Trim()
if ($normalizedVersion.StartsWith("v", [System.StringComparison]::OrdinalIgnoreCase)) {
    $normalizedVersion = $normalizedVersion.Substring(1)
}
if ($normalizedVersion -notmatch '^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$') {
    throw "Version must look like 1.2.3 or v1.2.3."
}

$buildDirectory = Join-Path $repositoryRoot "out\build\windows-release"
$cmakeCachePath = Join-Path $buildDirectory "CMakeCache.txt"
$outputDirectory = Join-Path $repositoryRoot "out\packages"
$packageName = "Ambilight-by-Levurgi4-v$normalizedVersion-windows-x64"
$packageDirectory = Join-Path $outputDirectory $packageName
$archivePath = Join-Path $outputDirectory "$packageName.zip"
$checksumPath = "$archivePath.sha256"

if (-not (Test-Path -LiteralPath $cmakeCachePath)) {
    throw "Configure the windows-release preset before packaging."
}
$qtDirectoryEntry = Select-String -LiteralPath $cmakeCachePath `
    -Pattern '^Qt6_DIR:PATH=(.+)$' | Select-Object -First 1
if ($null -eq $qtDirectoryEntry) {
    throw "Qt6_DIR was not found in the windows-release CMake cache."
}
$qtConfigVersionPath = Join-Path `
    $qtDirectoryEntry.Matches[0].Groups[1].Value `
    "Qt6ConfigVersionImpl.cmake"
$qtInstallRoot = [System.IO.Path]::GetFullPath((Join-Path `
    $qtDirectoryEntry.Matches[0].Groups[1].Value "..\..\.."))
if (-not (Test-Path -LiteralPath $qtConfigVersionPath)) {
    throw "Qt version metadata was not found at $qtConfigVersionPath."
}
$qtVersionMatch = Select-String -LiteralPath $qtConfigVersionPath `
    -Pattern '^set\(PACKAGE_VERSION "([^"]+)"\)' | Select-Object -First 1
if ($null -eq $qtVersionMatch) {
    throw "Qt version could not be read from $qtConfigVersionPath."
}
$actualQtVersion = $qtVersionMatch.Matches[0].Groups[1].Value
if ($actualQtVersion -ne $ExpectedQtVersion) {
    throw "This package must use Qt $ExpectedQtVersion for matching notices and source; the configured build uses Qt $actualQtVersion."
}

$allowedOutputRoot = [System.IO.Path]::GetFullPath($outputDirectory)
$allowedPrefix = $allowedOutputRoot.TrimEnd(
    [System.IO.Path]::DirectorySeparatorChar,
    [System.IO.Path]::AltDirectorySeparatorChar) +
    [System.IO.Path]::DirectorySeparatorChar
$resolvedPackageDirectory = [System.IO.Path]::GetFullPath($packageDirectory)
if (-not $resolvedPackageDirectory.StartsWith(
        $allowedPrefix,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to clean a package directory outside out\packages."
}

New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
if (Test-Path -LiteralPath $packageDirectory) {
    Remove-Item -LiteralPath $packageDirectory -Recurse -Force
}
foreach ($artifact in @($archivePath, $checksumPath)) {
    if (Test-Path -LiteralPath $artifact) {
        Remove-Item -LiteralPath $artifact -Force
    }
}

cmake --install $buildDirectory `
    --config $Configuration `
    --prefix $packageDirectory
if ($LASTEXITCODE -ne 0) {
    throw "cmake --install failed with exit code $LASTEXITCODE."
}

# Windows 10/11 provides this ICU compatibility forwarder as a system DLL.
# windeployqt copies it even though distributing the operating-system copy is
# unnecessary and would add another third-party binary to the portable ZIP.
$systemIcuForwarder = Join-Path $packageDirectory "bin\icuuc.dll"
if (Test-Path -LiteralPath $systemIcuForwarder) {
    Remove-Item -LiteralPath $systemIcuForwarder -Force
}

Copy-Item -LiteralPath (Join-Path $repositoryRoot "docs\installation-windows.md") `
    -Destination (Join-Path $packageDirectory "FIRST-RUN-RU.md")
Copy-Item -LiteralPath (Join-Path $repositoryRoot "README_PRODUCT.md") `
    -Destination (Join-Path $packageDirectory "README.md")

$qtNoticeDirectory = New-Item -ItemType Directory -Force `
    -Path (Join-Path $packageDirectory "LICENSES\Qt")
foreach ($module in @("qtbase", "qttranslations")) {
    $sbomName = "$module-$ExpectedQtVersion.spdx"
    $sbomPath = Join-Path $qtInstallRoot "sbom\$sbomName"
    if (-not (Test-Path -LiteralPath $sbomPath)) {
        throw "Qt SBOM is missing: $sbomPath"
    }
    Copy-Item -LiteralPath $sbomPath -Destination $qtNoticeDirectory
}

$requiredNotices = @(
    "LICENSE",
    "THIRD_PARTY_NOTICES.md",
    "LICENSES\LGPL-3.0-only.txt",
    "LICENSES\GPL-3.0-only.txt",
    "LICENSES\Qt\qtbase-$ExpectedQtVersion.spdx",
    "LICENSES\Qt\qttranslations-$ExpectedQtVersion.spdx"
)
foreach ($relativePath in $requiredNotices) {
    if (-not (Test-Path -LiteralPath (Join-Path $packageDirectory $relativePath))) {
        throw "Required package notice is missing: $relativePath"
    }
}

Compress-Archive -LiteralPath $packageDirectory `
    -DestinationPath $archivePath `
    -CompressionLevel Optimal

$archiveHash = Get-FileHash -LiteralPath $archivePath -Algorithm SHA256
$checksumLine = "{0} *{1}" -f $archiveHash.Hash.ToLowerInvariant(), `
    [System.IO.Path]::GetFileName($archivePath)
Set-Content -LiteralPath $checksumPath -Value $checksumLine -Encoding ascii

Write-Host "Portable package: $archivePath"
Write-Host "SHA-256: $($archiveHash.Hash.ToLowerInvariant())"
