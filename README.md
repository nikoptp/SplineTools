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
  optional spline-spaced decals, and Blueprint-authored roadside HISM meshes.
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

### Roadside Meshes

Road Blueprint class defaults can define reusable roadside mesh profiles under
**Road > Roadside Meshes**. Each profile may assign optional Start, Middle, and
End static meshes, choose the left side, right side, or both sides of the road,
and select Face Road, Face Lane, or Random orientation. Mesh +X is treated as
forward and +Z as up; use the per-profile rotation offset for meshes authored in
a different local orientation.

The profile's distance from the road center, spacing, scale, height offset, and
road-relative rotation can be tuned directly. Distance and height variance are
deterministic per side, while spacing jitter is cumulative and deterministic.
Both sides use the same longitudinal sample distances. For example, edge posts
can use both sides, a 600 cm center distance, 5,000 cm spacing, and zero jitter;
plowing markers can use random orientation and spacing jitter.

Open roads place Start and End meshes at the effective, junction-trimmed
boundaries. Middle meshes stay strictly between boundary meshes. A Middle-only
profile is inset by half its configured spacing at both effective boundaries;
with 5,000 cm spacing its samples begin at 2,500 cm and avoid cluttering road
intersections. Closed loops use only the Middle mesh, apply the same half-spacing
seam inset, and do not duplicate the loop seam. Landscape alignment is optional
per profile and uses the road's existing terrain trace settings to place
instances on and align them to the landscape normal.

Collision is also optional per profile. Enabled profiles use the HISM BlockAll
collision setup and the static mesh's authored collision, which is appropriate
for guardrails and fences. Disabled profiles use NoCollision, which is suitable
for decorative plowing markers.

Roadside HISM components are generated and serialized by **Bake Road Cache**
alongside road chunks and decals. PIE, standalone play, and packaged builds
load the authored roadside cache without rebuilding it or tracing the landscape.

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

Road terrain traces ignore cached procedural roads and junctions, preventing
successive rebuilds from treating generated collision as terrain and creating
ramps. Network rebuilds suppress redundant deferred junction callbacks while
roads are rebuilt. **Rebuild Dirty** remains incremental for roads but always
refreshes every managed junction cache; **Rebuild All** refreshes both complete
sets. Both operations are geometrically idempotent.

Junction surfaces use terrain-sampled radial rings instead of one large center
fan. Each road first finishes rebuilding its trim, then the junction copies the
actual serialized surface-row vertices from that road's endpoint mesh. This
keeps the rendered meshes on exactly the same seam instead of independently
resampling the spline. The center height is extrapolated from the connected road
approaches rather than taken from one terrain trace. Inner rings blend from that
road-supported center to the exact seams and constrain terrain displacement to
**Maximum Interior Terrain Deviation** (50 cm by default), preventing terrain
holes from collapsing the patch. The embedded perimeter skirt also copies each
road's actual side-flap corner vertices, preserves both flap anchors at shared
corners, and uses the dominant connected-road flap material. Terrain-facing skirt spans
are sampled independently at **Ground Blend Sample Spacing** (75 cm by default),
interpolate smoothly between the exact road-flap endpoint offsets, and remain
terrain projected between those anchors. Shared corners retain every connected
road identity, so the skirt does not cross either open road mouth. Terrain
traces ignore procedural roads and other junction meshes so cached geometry
cannot be mistaken for the ground.

Nearby junctions share the available length of any road connecting them, keeping
a configurable minimum road section between their trims. Their perimeter skirts
stop at the junction-center bisector, and managed neighboring junctions rebuild
together when either changes. Tune **Terrain Sample Spacing**, **Ground Blend
Width**, **Ground Blend Embed Depth**, **Nearby Junction Search Radius**, and
**Minimum Road Length Between Junctions** on the junction class defaults. Use
**Ground Blend Sample Spacing** to trade skirt smoothness for vertex count and
**Maximum Interior Terrain Deviation** to control interior ground conformity.
After upgrading an existing map, restart the editor and run **Rebuild All** once
to replace previously cached center-fan junction meshes.

Complex non-convex intersection topology and generated junction markings remain
future extensions.

## Road Painting Editor Mode

Open **Select Mode > Road Painting** and use **Draw** to drag a route directly
over Landscape actors. The brush ignores roads, junctions, buildings, and props.
Brush targeting retries simple and world-static collision, then loaded editor
Landscape heightfield data, so differing streamed-cell collision responses do
not release the mouse drag to normal viewport camera controls.
While dragging, the viewport shows the sampled route, simplified preview, snap
target, and prospective intersections. Generated actors are created only when
the stroke is released.
Preview intersection checks are processed incrementally with a 20-work-item
per-frame budget so large networks do not block the editor while the preview
finishes.

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

Use **Select/Move** to click a graph point or link for single selection. Click
and drag on terrain to marquee-select the visible points and road links inside
the rectangle. Dragging any selected element moves all selected points and the
endpoints of selected links together across the landscape. The Road Painting
toolkit shows the active network, point/link/junction counts, pending rebuild
state, and action buttons for **Delete**,
**Adopt Selected**, **Rebuild Dirty**, **Rebuild All**, **Validate**, **Frame**,
and **Insert Point**. **D** switches to Draw, **S** switches to Select/Move,
**F** frames the current network or selection, and **Escape** cancels the
current interaction. Hovering shows the surface cursor and selection target;
hold **Shift** and use the mouse wheel to adjust the Draw snap radius or Select
selection radius. Authoring operations use editor transactions and support
undo and redo. Undo/redo and explicit rebuilds reconcile all loaded actors with
the network GUID, removing managed road or junction actors whose graph ownership
was restored away by a transaction.

Each procedural road Blueprint can optionally define its own Landscape material
paint profile under **Road > Landscape Paint**. Enable the profile in the road
Blueprint Class Defaults, assign a Landscape Layer Info asset, choose a dedicated
edit-layer name such as `RoadPainting`, and set the full paint width and side
falloff. **Rebuild Dirty** or **Rebuild All** synchronizes editor-only Landscape
brush actors managed by the road network. One brush is created for each loaded
Landscape/edit-layer combination, and changing or deleting roads updates that
non-destructive brush contribution. Mixed road Blueprint classes can paint
different material layers and widths in one network.
Brush synchronization is time-sliced with the same 20-work-item-per-frame budget
and resumes automatically on later editor ticks. Once synchronization finishes,
the network queues one editing-weightmap update; each brush renders its complete
target-layer mask during that Landscape pass.

The target Landscape must have Edit Layers enabled, and the Layer Info name must
exist in its Landscape material. The brush copies Unreal's existing combined
weightmap and composites only the road width and falloff area; it never clears
or replaces the complete material layer. A non-weight-blended Layer Info paired
with an alpha-blended Landscape material layer remains the recommended authoring
setup for road masks. The result is editor-authored Landscape data and must be
saved/cooked with the map; no runtime Landscape mutation is performed.

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
