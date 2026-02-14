#include "nebula4x/core/enum_strings.h"

namespace nebula4x {

std::string body_type_to_string(BodyType t) {
  switch (t) {
    case BodyType::Star: return "star";
    case BodyType::Planet: return "planet";
    case BodyType::Moon: return "moon";
    case BodyType::Asteroid: return "asteroid";
    case BodyType::Comet: return "comet";
    case BodyType::GasGiant: return "gas_giant";
  }
  // Safe default for unknown/invalid values.
  return "planet";
}

BodyType body_type_from_string(const std::string& s) {
  if (s == "star") return BodyType::Star;
  if (s == "planet") return BodyType::Planet;
  if (s == "moon") return BodyType::Moon;
  if (s == "asteroid") return BodyType::Asteroid;
  if (s == "comet") return BodyType::Comet;
  if (s == "gas_giant") return BodyType::GasGiant;
  // Safe default for unknown strings.
  return BodyType::Planet;
}


std::string wreck_kind_to_string(WreckKind k) {
  switch (k) {
    case WreckKind::Ship: return "ship";
    case WreckKind::Cache: return "cache";
    case WreckKind::Debris: return "debris";
  }
  return "ship";
}

WreckKind wreck_kind_from_string(const std::string& s) {
  if (s == "ship" || s == "hull") return WreckKind::Ship;
  if (s == "cache" || s == "salvage_cache" || s == "mineral_cache") return WreckKind::Cache;
  if (s == "debris" || s == "field") return WreckKind::Debris;
  // Safe default for unknown strings.
  return WreckKind::Ship;
}

std::string anomaly_kind_to_string(AnomalyKind k) {
  switch (k) {
    case AnomalyKind::Unknown: return "unknown";
    case AnomalyKind::Signal: return "signal";
    case AnomalyKind::Distress: return "distress";
    case AnomalyKind::Ruins: return "ruins";
    case AnomalyKind::Artifact: return "artifact";
    case AnomalyKind::Phenomenon: return "phenomenon";
    case AnomalyKind::Distortion: return "distortion";
    case AnomalyKind::Xenoarchaeology: return "xenoarchaeology";
    case AnomalyKind::CodexEcho: return "codex_echo";
    case AnomalyKind::Echo: return "echo";
    case AnomalyKind::Cache: return "cache";
    case AnomalyKind::Generic: return "anomaly";
  }
  return "unknown";
}

AnomalyKind anomaly_kind_from_string(const std::string& s) {
  if (s == "signal" || s == "beacon") return AnomalyKind::Signal;
  if (s == "distress" || s == "mayday" || s == "sos") return AnomalyKind::Distress;
  if (s == "ruins") return AnomalyKind::Ruins;
  if (s == "artifact") return AnomalyKind::Artifact;
  if (s == "phenomenon" || s == "phenomena") return AnomalyKind::Phenomenon;
  if (s == "distortion") return AnomalyKind::Distortion;
  if (s == "xenoarchaeology" || s == "xeno") return AnomalyKind::Xenoarchaeology;
  if (s == "codex_echo" || s == "codex") return AnomalyKind::CodexEcho;
  if (s == "echo") return AnomalyKind::Echo;
  if (s == "cache") return AnomalyKind::Cache;
  if (s == "anomaly" || s == "generic") return AnomalyKind::Generic;
  return AnomalyKind::Unknown;
}

const char* anomaly_kind_label(AnomalyKind k) {
  switch (k) {
    case AnomalyKind::Unknown: return "Unknown";
    case AnomalyKind::Signal: return "Signal";
    case AnomalyKind::Distress: return "Distress";
    case AnomalyKind::Ruins: return "Ruins";
    case AnomalyKind::Artifact: return "Artifact";
    case AnomalyKind::Phenomenon: return "Phenomenon";
    case AnomalyKind::Distortion: return "Distortion";
    case AnomalyKind::Xenoarchaeology: return "Xenoarchaeology";
    case AnomalyKind::CodexEcho: return "Codex Echo";
    case AnomalyKind::Echo: return "Echo";
    case AnomalyKind::Cache: return "Cache";
    case AnomalyKind::Generic: return "Anomaly";
  }
  return "Unknown";
}

} // namespace nebula4x
