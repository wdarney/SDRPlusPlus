# Vendored Dear ImGui Metal backend

`backend.mm` includes `imgui/imgui_impl_metal.h` and links against the matching
`.mm`. Drop the upstream files here from
<https://github.com/ocornut/imgui/tree/master/backends>:

- `imgui_impl_metal.h`
- `imgui_impl_metal.mm`

`core/CMakeLists.txt`'s GLOB picks them up automatically.

We don't vendor them in the fork by default so the imgui version stays in
lockstep with whatever `core/src/imgui` is shipping. Pinning a divergent copy
here would cause drift on the next imgui bump.
