# SplineTools

Runtime C++ actors and editor tools for constructing reusable, spline-driven
world geometry in Unreal Engine.

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
- `AProceduralRoadActor` generates terrain-conforming road surfaces with
  world-uniform UVs, open bottoms, tucked terrain side flaps, convex collision,
  and optional spline-spaced decals.
- The **Road Painting** editor mode turns landscape strokes into connected road
  splines and explicit junction actors while preserving ordinary actor editing.
- Tools rebuild during construction and expose call-in-editor rebuild actions.

## Requirements

- Unreal Engine 5.4 or a compatible later version.
- Runtime module dependencies: `Core`, `CoreUObject`, `Engine`,
  `ProceduralMeshComponent`, and `PhysicsCore`.
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

## Procedural Road Quick Start

1. Place `AProceduralRoadActor` in a level and edit its spline.
2. Assign a road material and set road width, segment length, and width
   subdivisions.
3. Keep terrain alignment enabled to project every cross-section sample onto
   the landscape. Tune the trace range and surface offset if needed.
4. Tune side-flap width and embed depth so the open-bottom mesh meets uneven
   terrain without exposing gaps. Open spline ends can generate matching
   tangent-aligned terrain flaps with independent length and embed depth.
5. Enable simple collision to create grouped convex prisms along the surface.
6. Optionally assign a decal material and enable decal generation for spline-
   aligned markings or wear.

The road rebuilds after spline and property edits. UVs are based on world
distance across and along the road, so material density remains uniform when
segment lengths change. Road geometry is divided into moderately long,
configurable chunks. Editor rebuilds hash the sampled geometry and only replace
chunks affected by the spline edit; unchanged mesh and collision components are
retained.

Generated road chunks, collision, decals, source hashes, and junction patches
are serialized into the placed actors. PIE, standalone play, and packaged builds
load that authored cache and never trace the landscape or regenerate road
geometry. Use **Bake Road Cache** after a manual setup change and save the level;
`HasCachedRoadData` can be used by editor validation or Blueprint tooling to
detect an unbaked road.

Road-surface UV channel 1 is reserved for material-driven markings:

- `U` is normalized from `0` at the left road edge to `1` at the right edge.
- `V` is continuous spline distance divided by `MarkingUVWorldLength`.

A road material can use `TextureCoordinate` index 1 to create center lines,
edge lines, and repeating dash masks without decal components.

For simpler authoring, assign line materials and enable center and/or side lines
on the actor. Side lines are continuous and use mesh section/material slot 2.
The center line uses slot 3 and has independent material, width, surface offset,
UV scale, and optional world-distance dash/gap lengths. If no center material is
assigned, it falls back to the side-line material. The road surface material
needs no marking logic. Unreal decal components cannot be instanced through
ISM/HISM; keep optional decals for sparse wear or unique details.

## Road Intersections

Place an `AProceduralRoadJunctionActor` at the intersection center and set its
endpoint search radius and automatic trim distance. It discovers nearby road
spline start/end points, claims the available endpoints, trims the affected road
ends, and builds a terrain-conforming patch from the resulting road-edge pairs.
The patch uses world-space UVs and simple convex collision. A road endpoint can
be owned by only one junction at a time, preventing competing trims.
If no junction material is assigned, the patch uses the road material occurring
most often among its connected roads.

Junction and road-spline edits trigger debounced rescans and rebuilds in the
editor. When too few endpoints are available to produce a mesh, an orange
wireframe sphere shows the discovery area. Disable automatic endpoint discovery
to configure the connection list manually.

The initial center-fan patch works well for ordinary T-junctions, crossroads,
and other simple star-shaped layouts. Complex non-convex intersection topology
and generated junction markings remain future extensions.

## Road Painting Editor Mode

Open **Select Mode > Road Painting** and use **Draw** to drag a route directly
over Landscape actors. The brush ignores roads, junctions, buildings, and props.
While dragging, the viewport shows the sampled route, simplified preview, snap
target, and prospective intersections. Generated actors are created only when
the stroke is released.

Choose the road Blueprint class per stroke. Each resulting graph link retains
that class. Compatible degree-two links are grouped into one maximal spline;
branches and road-class transitions generate junction actors with exact managed
endpoint connections. Crossings whose interpolated heights differ by more than
the configured threshold remain disconnected, allowing overpasses.

The default authoring tolerances are:

- 200 cm stroke sampling.
- 100 cm route simplification.
- 300 cm endpoint and path snapping.
- 100 cm curved-path crossing sampling.
- 100 cm duplicate-intersection merging.
- 200 cm maximum junction height difference.

Use **Select/Move** to select graph points or links and drag points across the
landscape. **Delete** removes the selected graph element, **Rebuild Dirty** and
**Rebuild All** refresh managed output, and **Validate** reports graph or actor
reference problems. Authoring operations use editor transactions and support
undo and redo.

**Adopt Selected Roads** imports only the explicitly selected procedural road
actors and optional selected junctions. Adoption preserves each road actor,
Blueprint class, closed-loop state, spline point types, and custom tangents
where possible. If a selected junction references an unselected road, adoption
stops and reports the roads that must also be selected.

The mode stores its graph in one non-spatial `ARoadNetworkActor`. The network,
point, link, run, and junction GUIDs make actor ownership explicit; a network
will never modify or remove actors managed by another network. Managed roads
and junctions remain spatial World Partition external actors and retain their
serialized geometry caches. The network actor and the `SplineToolsEditor`
module are editor-only and are excluded from cooked builds.

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
- `Source/SplineToolsEditor/`: road graph, editor mode, interactive tools, and
  automation tests.

## Portability Notes

- Keep the directory, manifest, and module names as `SplineTools`.
- No content is bundled (`CanContainContent` is false); destination projects
  must assign their own meshes.
- Host modules that include plugin headers should add `SplineTools` to their
  `.Build.cs` dependencies.
- Commit source and the manifest. Exclude generated `Binaries/`,
  `Intermediate/`, `DerivedDataCache/`, and IDE files.
- Licensed under the MIT License; see `LICENSE`.
