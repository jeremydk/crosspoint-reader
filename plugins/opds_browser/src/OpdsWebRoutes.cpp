// OPDS web routes. Previously these three handlers lived in
// CrossPointWebServer (core) and reached into OpdsServerStore directly,
// which forced the File Transfer plugin to declare a dependency on this one.
// They now live next to the store and the web server picks them up via
// PluginManifest::webRoutes, so the cross-plugin coupling is gone.

#include <ArduinoJson.h>
#include <Logging.h>
#include <PluginManifest.h>
#include <WebServer.h>

#include <cstddef>
#include <string>

#include "OpdsServerStore.h"

namespace {

void handleGetOpdsServers(WebServer& server) {
  const auto& servers = OPDS_STORE.getServers();

  // Stream JSON incrementally so the response doesn't all live in heap at once.
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  for (size_t i = 0; i < servers.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["name"] = servers[i].name;
    doc["url"] = servers[i].url;
    doc["username"] = servers[i].username;
    // Never expose passwords over the API; just flag whether one is set.
    doc["hasPassword"] = !servers[i].password.empty();

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    if (i > 0) server.sendContent(",");
    server.sendContent(output);
  }

  server.sendContent("]");
  server.sendContent("");
  LOG_DBG("WEB", "Served OPDS servers API (%zu servers)", servers.size());
}

void handlePostOpdsServer(WebServer& server) {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server.arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  OpdsServer opdsServer;
  opdsServer.name = doc["name"] | std::string("");
  opdsServer.url = doc["url"] | std::string("");
  opdsServer.username = doc["username"] | std::string("");

  // Password is optional. Absent (vs. present-but-empty) means "preserve the
  // current password" — the web UI omits the field when the user hasn't
  // touched it.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
      server.send(400, "text/plain", "Invalid server index");
      return;
    }
    if (!hasPasswordField) {
      const auto* existing = OPDS_STORE.getServer(static_cast<size_t>(idx));
      if (existing) password = existing->password;
    }
    opdsServer.password = password;
    OPDS_STORE.updateServer(static_cast<size_t>(idx), opdsServer);
    LOG_DBG("WEB", "Updated OPDS server at index %d", idx);
  } else {
    opdsServer.password = password;
    if (!OPDS_STORE.addServer(opdsServer)) {
      server.send(400, "text/plain", "Cannot add server (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added new OPDS server: %s", opdsServer.name.c_str());
  }

  server.send(200, "text/plain", "OK");
}

// POST not DELETE: ESP32 WebServer doesn't surface a request body on DELETE.
void handleDeleteOpdsServer(WebServer& server) {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server.arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server.send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
    server.send(400, "text/plain", "Invalid server index");
    return;
  }

  OPDS_STORE.removeServer(static_cast<size_t>(idx));
  LOG_DBG("WEB", "Deleted OPDS server at index %d", idx);
  server.send(200, "text/plain", "OK");
}

}  // namespace

extern const PluginWebRoute kOpdsWebRoutes[] = {
    {"/api/opds", HTTP_GET, &handleGetOpdsServers},
    {"/api/opds", HTTP_POST, &handlePostOpdsServer},
    {"/api/opds/delete", HTTP_POST, &handleDeleteOpdsServer},
};

extern const uint8_t kOpdsWebRouteCount = 3;
