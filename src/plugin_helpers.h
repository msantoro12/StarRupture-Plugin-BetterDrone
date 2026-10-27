#pragma once
#include <plugin_interface.h>

IPluginSelf* GetSelf();

// Shutdown also runs at process exit and, on loaders older than 1.22.0, from
// the render thread. Game objects are only touched where OnGameThread() is
// true: the thread the last engine tick ran on (NoteGameThread, called at the
// top of every tick). False until the first tick.
void NoteGameThread();
bool OnGameThread();

#define LOG_TRACE(format, ...) if (auto s = GetSelf()) s->logger->Trace(s, format, ##__VA_ARGS__)
#define LOG_DEBUG(format, ...) if (auto s = GetSelf()) s->logger->Debug(s, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...)  if (auto s = GetSelf()) s->logger->Info (s, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...)  if (auto s = GetSelf()) s->logger->Warn (s, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) if (auto s = GetSelf()) s->logger->Error(s, format, ##__VA_ARGS__)
