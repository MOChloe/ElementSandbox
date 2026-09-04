# ElementSandbox

ElementSandbox is a UE 5.6 sandbox architecture and performance demo focused on large-scale persistent world data, streaming, spatial queries, and destruction.

## Highlights

- Independent ECS-oriented storage and processing paths for buildings, world objects, and elemental simulation.
- Chunk-based persistent storage, streaming, and on-demand synchronization.
- BVH, Dynamic AABB Tree, and Cell-based spatial indexing selected for different entity behaviors.
- Custom Raycast/Overlap queries, on-demand collision proxies, event-driven scheduling, and `ParallelFor` processing.
- Client/server process separation even in local single-player mode.
- HISM/WPO-driven meteor debris presentation with precomputed trajectories, avoiding per-fragment Actors and per-frame CPU Transform updates.

## Included world seed

`WorldSeeds/MillionSettlement` contains the chunked million-scale building dataset used by the demo. The seed is intentionally included so the large-world streaming and destruction scenarios can be reproduced after cloning the repository.

## Reference benchmark

Tested at native 1920x1080 on an Intel Core i9-13900K and NVIDIA RTX 3080:

- World loading: 200+ FPS
- Loaded world: approximately 300 FPS
- Meteor scenario with approximately 120,000 flying and landing `WorldObject` fragments: approximately 240 FPS during flight and after landing

Performance varies with hardware, build configuration, and runtime settings.

## Requirements

- Unreal Engine 5.6
- A C++ toolchain supported by UE 5.6

## Running the demo

1. Generate project files and compile the project for UE 5.6.
2. Run `Launch_SingleClient.bat` for a local server and one client, or `Launch_TwoClients.bat` for the multiplayer synchronization demonstration.
3. Use `Reset_WorldSave.bat` to restore a clean writable world from the bundled source seed.

The launch scripts use native 1920x1080 output and start the local server in the background.
