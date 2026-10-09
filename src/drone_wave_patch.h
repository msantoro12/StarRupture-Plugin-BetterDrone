#pragma once

struct IPluginSelf;
struct IPluginHookScanner;

// Resolve the wave-patch AOBs. Callable only from OnPluginLoadHooks — the loader
// refuses scans made anywhere else.
void ResolveWavePatch(IPluginSelf* self, IPluginHookScanner* scanner);

bool InitWavePatch();
void ShutdownWavePatch();
