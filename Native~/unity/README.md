# Unity PluginAPI headers

`basis_unity_plugin.cpp` includes Unity's native plugin interface headers. They
are **not** redistributed here (they ship with the editor). Copy these files from
your Unity install into this folder, or add the source folder to the compiler
include path via the CMake cache var `UNITY_PLUGIN_API_DIR`:

```
<UnityEditor>/Editor/Data/PluginAPI/IUnityInterface.h
<UnityEditor>/Editor/Data/PluginAPI/IUnityGraphics.h
<UnityEditor>/Editor/Data/PluginAPI/IUnityGraphicsD3D11.h
<UnityEditor>/Editor/Data/PluginAPI/IUnityGraphicsD3D12.h
<UnityEditor>/Editor/Data/PluginAPI/IUnityGraphicsVulkan.h
```

- Windows build needs: IUnityInterface, IUnityGraphics, IUnityGraphicsD3D11, IUnityGraphicsD3D12
- Android build needs: IUnityInterface, IUnityGraphics, IUnityGraphicsVulkan

Typical locations:
- Windows: `C:\Program Files\Unity\Hub\Editor\<ver>\Editor\Data\PluginAPI`
- macOS:   `/Applications/Unity/Hub/Editor/<ver>/Unity.app/Contents/PluginAPI`

These headers are licensed under the Unity Companion License; keep them with your
local build only (the `.gitignore` in Native~ excludes them from the package).
