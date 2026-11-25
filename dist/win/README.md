# Windows releases

Add release notes under `Unreleased` in [`CHANGELOG.md`](../../CHANGELOG.md) and commit them. From the repo root, create a release branch and run:

```sh
git switch -c release/7.2
uv run scripts/bump.py minor
```

The bump script updates the app and installer versions and moves the notes into a dated release entry. Use `major` or `minor`; qView currently uses two-part versions with a single-digit minor. Review the changes and follow the printed Git commands. Pushing the `vMAJOR.MINOR` tag creates the GitHub Release with those notes; GitHub Actions does not build the installer.

Once the GitHub Release exists, run `make push` from the tagged commit. It builds the Windows x64 Release locally, stages the Qt runtime and plugins, compiles `qView.iss`, and uploads the installer to the matching GitHub Release in `metrized-inc/qView`. It refuses to replace an existing asset. The installer also remains in `build/installers/`.

You need Make, PowerShell 7, CMake, Visual Studio C++ tools, Qt 6.8.3 for MSVC x64, Inno Setup 6, and an authenticated `gh` with release write access. Set `QV_QT_ROOT` if Qt is not at `C:\Dev\Qt\6.8.3\msvc2022_64`, or `QV_CMAKE_GENERATOR` if not using Visual Studio 2026. The package does not bundle the Microsoft Visual C++ Redistributable or optional third-party image plugins.

The installer still uses upstream qView’s installer identity and default directory; it is not a side-by-side install.
