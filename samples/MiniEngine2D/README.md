# MiniEngine2D Docs

This sample keeps its design notes next to the code.

- [uml_class.md](./uml_class.md): class-level UML for the engine systems.
- [diagram_engine.md](./diagram_engine.md): frame flow and runtime sequence.
- [DesignGameEngine_Frame.md](./DesignGameEngine_Frame.md): startup and initialization UML.

Current architectural note:

- Rendering is exposed as a single engine-wide service through `GameEngine::GetRender()`.
- Gameplay builds `RenderFrame` objects; the render thread consumes them from `RenderQueue`.
- Actors never render directly. They only emit `SpriteSceneProxy` data.
