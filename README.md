# Prism-24.20

A game server for Fortnite v24.20 (`++Fortnite+Release-24.20-CL-25156858`).

## Replication Graph

Includes a recreated Replication Graph plus Fortnite's own game-specific version built on top of it (`FortReplicationGraph`). Not 1:1 with Epic's, but still cool.

Source:
- [ReplicationGraph.cpp](Prism-24.20/Engine/Plugins/Runtime/ReplicationGraph/Source/Private/ReplicationGraph.cpp)
- [FortReplicationGraph.cpp](Prism-24.20/FortniteGame/Source/FortniteGame/Private/Online/FortReplicationGraph.cpp)

## Status

Prism is no longer being worked on. It is open source for show, and there are still bugs. Contributions are welcome.

## Building

1. Install Visual Studio with the "Desktop development with C++" workload.
2. Clone the repository.
3. Open `Prism-24.20.slnx`.
4. Set the configuration to `Release` and the platform to `x64`.
5. Build the `Prism-24.20` project.

The output is a DLL. The Debug configuration does not build.
