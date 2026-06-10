# Seamless Instancing

A UE 5.7+ plugin to handle instancing of Static Mesh Actors seamlessly.

- De-selecting Static Mesh Actors automatically converts them to instances.
- Selecting instances automatically converts them back to Static Mesh Actors for easy manipulation.

No need for manual conversions, extra editor modes or complex tools.
Just install, enable Seamless Instancing from the toolbar and forget it exists.

Enjoy lightweight levels with instancing, with the proper UX of manipulating regular actors. Seamlessly.

### Supported Features
- Processes only StaticMeshComponents of StaticMeshActors with Mobility is set to Static, SpatiallyLoaded set to True, and not HiddenInEditor
- Maintains most StaticMeshComponents' properties (i.e. Material override, collision options, cast shadows, etc) by making different Instanced Components when a different property is needed
- Supports World Partition levels
  - Instances are separated per WP tile
  - Instances are separated per each DataLayers combination
- Works on perspective and ortho viewports
- Works with pick-based selection and selection box
- Converts CustomPrimitiveData into PerInstanceCustomData and back

### Caveats and Considerations
- Makes Actor Groups unusable. Needs some thinking on how they could be made to work
- One File Per Actor granularity is greatly reduced since components are now merged

### TO DO
- World Partition levels: different grids
- Non World Partition levels, sublevels
- More stuff
