#include "OpdsServerStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstring>

#include "CrossPointSettings.h"

OpdsServerStore OpdsServerStore::instance;

namespace {
constexpr char OPDS_FILE_JSON[] = "/.crosspoint/opds.json";

// Serialize the in-memory server list to JSON on SD. Passwords are XOR-obfuscated
// with the device MAC and base64-encoded under "password_obf"; the friend-based
// JsonSettingsIO::saveOpds helper used to live in core but moved here when OPDS
// became a plugin, since core can't depend on a plugin-owned type.
bool writeOpdsJson(const std::vector<OpdsServer>& servers, const char* path) {
  JsonDocument doc;
  JsonArray arr = doc["servers"].to<JsonArray>();
  for (const auto& server : servers) {
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = server.name;
    obj["url"] = server.url;
    obj["username"] = server.username;
    obj["password_obf"] = obfuscation::obfuscateToBase64(server.password);
  }
  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

// Returns true on parse success. Sets *needsResave when any password came from
// the legacy plaintext "password" key, so the caller can rewrite the file with
// obfuscation.
bool readOpdsJson(std::vector<OpdsServer>& servers, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("OPS", "JSON parse error: %s", error.c_str());
    return false;
  }
  servers.clear();
  JsonArray arr = doc["servers"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (servers.size() >= OpdsServerStore::MAX_SERVERS) break;
    OpdsServer server;
    server.name = obj["name"] | std::string("");
    server.url = obj["url"] | std::string("");
    server.username = obj["username"] | std::string("");
    bool ok = false;
    server.password = obfuscation::deobfuscateFromBase64(obj["password_obf"] | "", &ok);
    if (!ok || server.password.empty()) {
      server.password = obj["password"] | std::string("");
      if (!server.password.empty() && needsResave) *needsResave = true;
    }
    servers.push_back(std::move(server));
  }
  LOG_DBG("OPS", "Loaded %zu OPDS servers from file", servers.size());
  return true;
}
}  // namespace

bool OpdsServerStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return writeOpdsJson(servers, OPDS_FILE_JSON);
}

bool OpdsServerStore::loadFromFile() {
  if (Storage.exists(OPDS_FILE_JSON)) {
    String json = Storage.readFile(OPDS_FILE_JSON);
    if (!json.isEmpty()) {
      bool resave = false;
      bool result = readOpdsJson(servers, json.c_str(), &resave);
      if (result && resave) {
        LOG_DBG("OPS", "Resaving JSON with obfuscated passwords");
        saveToFile();
      }
      return result;
    }
  }

  // No opds.json found — attempt one-time migration from the legacy single-server
  // fields in CrossPointSettings (opdsServerUrl/opdsUsername/opdsPassword).
  if (migrateFromSettings()) {
    LOG_DBG("OPS", "Migrated legacy OPDS settings");
    return true;
  }

  return false;
}

bool OpdsServerStore::migrateFromSettings() {
  if (strlen(SETTINGS.opdsServerUrl) == 0) {
    return false;
  }

  OpdsServer server;
  server.name = "OPDS Server";
  server.url = SETTINGS.opdsServerUrl;
  server.username = SETTINGS.opdsUsername;
  server.password = SETTINGS.opdsPassword;
  servers.push_back(std::move(server));

  if (saveToFile()) {
    // Clear legacy fields so migration won't run again on next boot
    SETTINGS.opdsServerUrl[0] = '\0';
    SETTINGS.opdsUsername[0] = '\0';
    SETTINGS.opdsPassword[0] = '\0';
    SETTINGS.saveToFile();
    LOG_DBG("OPS", "Migrated single-server OPDS config to opds.json");
    return true;
  }

  // Save failed — roll back in-memory state so we don't have a partial migration
  servers.clear();
  return false;
}

bool OpdsServerStore::addServer(const OpdsServer& server) {
  if (servers.size() >= MAX_SERVERS) {
    LOG_DBG("OPS", "Cannot add more servers, limit of %zu reached", MAX_SERVERS);
    return false;
  }

  servers.push_back(server);
  LOG_DBG("OPS", "Added server: %s", server.name.c_str());
  return saveToFile();
}

bool OpdsServerStore::updateServer(size_t index, const OpdsServer& server) {
  if (index >= servers.size()) {
    return false;
  }

  servers[index] = server;
  LOG_DBG("OPS", "Updated server: %s", server.name.c_str());
  return saveToFile();
}

bool OpdsServerStore::removeServer(size_t index) {
  if (index >= servers.size()) {
    return false;
  }

  LOG_DBG("OPS", "Removed server: %s", servers[index].name.c_str());
  servers.erase(servers.begin() + static_cast<ptrdiff_t>(index));
  return saveToFile();
}

const OpdsServer* OpdsServerStore::getServer(size_t index) const {
  if (index >= servers.size()) {
    return nullptr;
  }
  return &servers[index];
}
