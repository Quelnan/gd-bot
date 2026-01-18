# GD Pathfinder

An automatic level pathfinder mod for Geometry Dash that uses physics simulation to find solutions.

## Features

- **Physics Simulation**: 1:1 recreation of GD physics for all gamemodes
- **Beam Search Pathfinding**: Efficient search algorithm to find solutions
- **All Gamemodes**: Cube, Ship, Ball, UFO, Wave, Robot, Spider, Swing
- **Automatic Replay**: Found paths are automatically replayed

## Usage

1. Go to any level's info page
2. Click the Pathfinder button (PF)
3. Click "Start" to begin pathfinding
4. Once a path is found, click "Start" again to replay it

## Limitations

- Very long or complex levels may take longer to solve
- Frame-perfect sections may not be solved perfectly
- Some triggers and mechanics are not fully supported

## Logs

Logs are saved to: `geode/mods/quelnan.gdpathfinder/pathfinder.log`
