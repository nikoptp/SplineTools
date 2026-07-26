# SplineTools

Runtime C++ actors for constructing reusable, spline-driven world geometry in
Unreal Engine.

Developed in an Unreal Engine 5.4 project.

## Features

- `ASplineToolActorBase` provides construction-time rebuilding, spline sampling,
  generated-content cleanup, and HISM helpers.
- `ACastleWallActor` places randomized wall meshes and towers along open or
  closed splines.
- Castle walls support corner-tower detection, evenly distributed towers, and
  optional terrain snapping.
- `ARopeBridge` creates physics-enabled bridge sections and constraints along a
  spline.
- Tools rebuild during construction and expose call-in-editor rebuild actions.

## Requirements

- Unreal Engine 5.4 or a compatible later version.
- Runtime module dependencies: `Core`, `CoreUObject`, `Engine`, and
  `PhysicsCore`.
- The plugin is code-only and requires a C++ toolchain.
- Meshes are supplied by the host project.

## Installation

1. Copy this directory to `<Project>/Plugins/SplineTools`.
2. Regenerate project files.
3. Build the project and open Unreal Editor.
4. Enable **SplineTools** in **Edit > Plugins** if needed.

Do not move generated `Binaries/` or `Intermediate/` directories between
projects.

## Castle Wall Quick Start

1. Place `ACastleWallActor` in a level.
2. Edit its spline points.
3. Assign wall and optional tower meshes.
4. Configure spacing, scale, offsets, seed, closed-loop behavior, and terrain
   snapping.
5. Use **Rebuild Spline Tool** when a manual rebuild is needed.

Generated wall and tower meshes use hierarchical instancing.

## Rope Bridge Quick Start

1. Place `ARopeBridge` in a level.
2. Edit its bridge spline.
3. Assign one or more bridge-part meshes.
4. Configure spacing, mass, damping, anchoring, and angular constraints.
5. Use **Rebuild Bridge** after changing settings when needed.

The first and last parts can be anchored while intermediate sections simulate
physics.

## Extending the Plugin

Derive a new actor from `ASplineToolActorBase` and override:

- `UpdateSplineSettings`
- `ResetGeneratedContent`
- `GenerateSplineContent`
- `FinalizeGeneratedContent`

Use the base transform and HISM helpers when the generated pieces can be
instanced.

## Repository Contents

- `SplineTools.uplugin`: plugin manifest.
- `Source/SplineTools/`: runtime module and spline actor implementations.

## Portability Notes

- Keep the directory, manifest, and module names as `SplineTools`.
- No content is bundled (`CanContainContent` is false); destination projects
  must assign their own meshes.
- Host modules that include plugin headers should add `SplineTools` to their
  `.Build.cs` dependencies.
- Commit source and the manifest. Exclude generated `Binaries/`,
  `Intermediate/`, `DerivedDataCache/`, and IDE files.
- No standalone license has been declared in this directory yet. Add a
  `LICENSE` file before distributing the extracted repository.
