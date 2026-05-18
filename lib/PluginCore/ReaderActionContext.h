#pragma once

#include <Epub.h>

#include <memory>
#include <optional>
#include <string>

class Activity;
class GfxRenderer;
class MappedInputManager;

// ReaderActionContext: the surface a plugin sees inside a PluginReaderMenuAction.
//
// Why an interface and not raw access to the reader: the reader owns ~65 KB of
// Epub state. Network plugins (KOReader sync, OPDS push) cannot share that
// memory with a TLS handshake. `releaseEpubAndReplace` encapsulates the "snapshot
// position → free Epub → switch activity" sequence so plugins don't reimplement
// it (and don't get it subtly wrong, like forgetting to persist progress first).
class ReaderActionContext {
 public:
  virtual ~ReaderActionContext() = default;

  // The reader holds the Epub via std::shared_ptr; plugins receive a shared
  // copy. The pointer becomes the only owner after releaseEpubAndReplace clears
  // the reader's reference, so plugins should normally drop their copy before
  // initiating a handoff (or rely on the new activity to take it from here).
  virtual std::shared_ptr<Epub> getEpub() = 0;
  virtual std::string getEpubPath() const = 0;

  // Pass-throughs so plugins can construct the activity passed to
  // releaseEpubAndReplace. The reader is mid-tear-down by then but the renderer
  // and input manager outlive any single activity.
  virtual GfxRenderer& getRenderer() = 0;
  virtual MappedInputManager& getInput() = 0;

  virtual int getCurrentSpineIndex() const = 0;
  virtual int getCurrentPage() const = 0;
  virtual int getTotalPages() const = 0;
  virtual std::optional<uint16_t> getCurrentParagraphIndex() const = 0;

  // Persist the reader's current position to disk. Returns false on write failure;
  // plugins should abort their handoff in that case so the user resumes where they
  // think they are.
  virtual bool saveProgress() = 0;

  // Snapshot the current position, free the Epub (~65 KB), and replace the reader
  // with `newActivity`. Plugins that need significant heap for their handoff
  // (anything doing TLS) should route through here.
  virtual void releaseEpubAndReplace(std::unique_ptr<Activity> newActivity) = 0;

  // Arm a "save failed" popup on the next reader render. Useful when a plugin
  // aborts its handoff because saveProgress() returned false — the popup
  // surfaces the failure once the reader regains focus.
  virtual void flashSaveErrorPopup() = 0;
};
