#pragma once
#include "config.h"

inline AppConfig sampleConfig() {
  AppConfig c;
  c.mountRoot = "/mnt/test";
  Server server;
  server.id = "test-server";
  server.name = "Test server";
  server.hostname = "nas.example";
  server.fallbackIp = "192.0.2.10";
  server.protocol = Protocol::Nfs;
  c.servers.append(server);
  c.shares.append({"test-share", "test-server", "Media", "/exports/media",
                   "/mnt/test/Media", "", AutomountMode::Disabled, true});
  return c;
}
