# Build and upload the Windows x64 installer for an existing release.
param(
    [string]$QtRoot = $(if ($env:QV_QT_ROOT) { $env:QV_QT_ROOT } else { 'C:\Dev\Qt\6.8.3\msvc2022_64' })
)

$ErrorActionPreference = 'Stop'
$repo = 'metrized-inc/qView'

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed ($LASTEXITCODE)." }
}

Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    if (Invoke-Checked git @('status', '--porcelain', '--untracked-files=no')) {
        throw 'Commit or stash tracked changes before building a release.'
    }

    $cmake = Get-Content CMakeLists.txt -Raw
    if ($cmake -notmatch '(?m)^project\(qView VERSION ([0-9]+\.[0-9]) LANGUAGES CXX\)\r?$') {
        throw 'Could not read the app version from CMakeLists.txt.'
    }
    $version = $Matches[1]
    $installer = Get-Content dist/win/qView.iss -Raw
    if ($installer -notmatch '(?m)^#define MyAppVersion "([0-9]+\.[0-9])"\r?$' -or $Matches[1] -ne $version) {
        throw 'App and installer versions do not match.'
    }
    $tag = "v$version"
    $release = Invoke-Checked gh @('release', 'view', $tag, '-R', $repo, '--json', 'isDraft,assets') | ConvertFrom-Json
    if ($release.isDraft) { throw "Publish the $tag release before uploading." }
    $filename = "qView-$version-win64.exe"
    if ($release.assets.name -contains $filename) { throw "$filename is already on $tag; refusing to overwrite it." }
    $commit = Invoke-Checked gh @('api', "repos/$repo/commits/$tag", '--jq', '.sha')
    if ($commit -ne (Invoke-Checked git @('rev-parse', 'HEAD'))) {
        throw "Check out the commit tagged $tag on $repo before building."
    }

    $deploy = Join-Path $QtRoot 'bin/windeployqt.exe'
    if (-not (Test-Path -LiteralPath $deploy -PathType Leaf)) {
        throw "Cannot find $deploy. Set QV_QT_ROOT to the Qt kit directory."
    }
    $iscc = (Get-Command ISCC.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1).Source
    if (-not $iscc) {
        $iscc = @("${env:ProgramFiles(x86)}/Inno Setup 6/ISCC.exe", "$env:LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe") |
            Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    }
    if (-not $iscc) { throw 'Install Inno Setup 6 or put ISCC.exe on PATH.' }

    $generator = if ($env:QV_CMAKE_GENERATOR) { $env:QV_CMAKE_GENERATOR } else { 'Visual Studio 18 2026' }
    Invoke-Checked cmake @('-S', '.', '-B', 'build/release-x64', '-G', $generator, '-A', 'x64', "-DCMAKE_PREFIX_PATH=$QtRoot", '-DCMAKE_CXX_FLAGS=-D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR')
    Invoke-Checked cmake @('--build', 'build/release-x64', '--config', 'Release', '--parallel')

    $output = (New-Item -ItemType Directory -Force 'build/installers').FullName
    $stage = (New-Item -ItemType Directory "build/installer-stage-$([Guid]::NewGuid())").FullName
    try {
        Copy-Item 'build/release-x64/Release/qView.exe' $stage
        Invoke-Checked $deploy @('--release', '--no-compiler-runtime', (Join-Path $stage 'qView.exe'))
        Invoke-Checked $iscc @("/DMyStageDir=$stage", "/O$output", 'dist/win/qView.iss')
        $asset = Join-Path $output $filename
        if (-not (Test-Path -LiteralPath $asset -PathType Leaf)) { throw "Installer not found: $asset" }
        Invoke-Checked gh @('release', 'upload', $tag, $asset, '-R', $repo)
    } finally {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
} finally {
    Pop-Location
}
