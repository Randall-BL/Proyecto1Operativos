#include "ShipScheduler.h" // API del scheduler.

#include "ShipIO.h" // Logging por Serial.

void ship_scheduler_set_algorithm(ShipScheduler *scheduler, ShipAlgo algo) {
  if (!scheduler) return;
  scheduler->algorithm = algo;
}

ShipAlgo ship_scheduler_get_algorithm(const ShipScheduler *scheduler) {
  if (!scheduler) return ALG_FCFS;
  return scheduler->algorithm;
}

const char *ship_scheduler_get_algorithm_label(const ShipScheduler *scheduler) {
  if (!scheduler) return "?";
  switch (scheduler->algorithm) {
    case ALG_FCFS: return "FCFS";
    case ALG_SJF: return "SJF";
    case ALG_STRN: return "STRN";
    case ALG_EDF: return "EDF";
    case ALG_RR: return "RR";
    case ALG_PRIORITY: return "PRIO";
  }
  return "?";
}

void ship_scheduler_set_round_robin_quantum(ShipScheduler *scheduler, unsigned long quantumMillis) {
  if (!scheduler) return;
  if (quantumMillis < 100) quantumMillis = 100;
  scheduler->rrQuantumMillis = quantumMillis;
}

unsigned long ship_scheduler_get_round_robin_quantum(const ShipScheduler *scheduler) {
  if (!scheduler) return 0;
  return scheduler->rrQuantumMillis;
}

void ship_scheduler_set_flow_mode(ShipScheduler *scheduler, ShipFlowMode mode) {
  if (!scheduler) return;
  scheduler->flowMode = mode;
  scheduler->fairnessPassedInWindow = 0;
  scheduler->signLastSwitchAt = millis();
}

ShipFlowMode ship_scheduler_get_flow_mode(const ShipScheduler *scheduler) {
  if (!scheduler) return FLOW_TICO;
  return scheduler->flowMode;
}

const char *ship_scheduler_get_flow_mode_label(const ShipScheduler *scheduler) {
  if (!scheduler) return "?";
  switch (scheduler->flowMode) {
    case FLOW_TICO: return "TICO";
    case FLOW_FAIRNESS: return "EQUIDAD";
    case FLOW_SIGN: return "LETRERO";
  }
  return "?";
}

void ship_scheduler_set_fairness_window(ShipScheduler *scheduler, uint8_t windowW) {
  if (!scheduler) return;
  if (windowW == 0) windowW = 1;
  scheduler->fairnessWindowW = windowW;
  scheduler->fairnessPassedInWindow = 0;
}

uint8_t ship_scheduler_get_fairness_window(const ShipScheduler *scheduler) {
  if (!scheduler) return 1;
  return scheduler->fairnessWindowW == 0 ? 1 : scheduler->fairnessWindowW;
}

void ship_scheduler_set_sign_direction(ShipScheduler *scheduler, BoatSide side) {
  if (!scheduler) return;
  scheduler->signDirection = side;
  scheduler->signLastSwitchAt = millis();
}

BoatSide ship_scheduler_get_sign_direction(const ShipScheduler *scheduler) {
  if (!scheduler) return SIDE_LEFT;
  return scheduler->signDirection;
}

void ship_scheduler_set_sign_interval(ShipScheduler *scheduler, unsigned long intervalMillis) {
  if (!scheduler) return;
  if (intervalMillis < 1000) intervalMillis = 1000;
  scheduler->signIntervalMillis = intervalMillis;
  scheduler->signLastSwitchAt = millis();
}

unsigned long ship_scheduler_get_sign_interval(const ShipScheduler *scheduler) {
  if (!scheduler) return 0;
  return scheduler->signIntervalMillis;
}

void ship_scheduler_set_max_ready_queue(ShipScheduler *scheduler, uint8_t limit) {
  if (!scheduler) return;
  if (limit == 0) limit = 1;
  if (limit > MAX_BOATS) limit = MAX_BOATS;
  scheduler->maxReadyQueueConfigured = limit;
}

uint8_t ship_scheduler_get_max_ready_queue(const ShipScheduler *scheduler) {
  if (!scheduler) return MAX_BOATS;
  if (scheduler->maxReadyQueueConfigured == 0 || scheduler->maxReadyQueueConfigured > MAX_BOATS) return MAX_BOATS;
  return scheduler->maxReadyQueueConfigured;
}

void ship_scheduler_set_channel_length(ShipScheduler *scheduler, uint16_t meters) {
  if (!scheduler) return;
  if (meters == 0) meters = 1;
  scheduler->channelLengthMeters = meters;
}

uint16_t ship_scheduler_get_channel_length(const ShipScheduler *scheduler) {
  if (!scheduler) return 0;
  return scheduler->channelLengthMeters;
}

void ship_scheduler_set_boat_speed(ShipScheduler *scheduler, uint16_t metersPerSec) {
  if (!scheduler) return;
  if (metersPerSec == 0) metersPerSec = 1;
  scheduler->boatSpeedMetersPerSec = metersPerSec;
}

uint16_t ship_scheduler_get_boat_speed(const ShipScheduler *scheduler) {
  if (!scheduler) return 0;
  return scheduler->boatSpeedMetersPerSec;
}

void ship_scheduler_set_flow_logging(ShipScheduler *scheduler, bool enabled) {
  if (!scheduler) return;
  scheduler->flowLoggingEnabled = enabled;
}

bool ship_scheduler_get_flow_logging(const ShipScheduler *scheduler) {
  if (!scheduler) return false;
  return scheduler->flowLoggingEnabled;
}

void ship_scheduler_set_tico_margin_factor(ShipScheduler *scheduler, BoatType activeType, BoatType candidateType, float factor) {
  if (!scheduler) return;
  if (activeType < 0 || activeType >= 3) return;
  if (candidateType < 0 || candidateType >= 3) return;
  if (factor <= 0.0f) factor = 1.0f;
  scheduler->ticoMarginFactor[activeType][candidateType] = factor;
}

float ship_scheduler_get_tico_margin_factor(const ShipScheduler *scheduler, BoatType activeType, BoatType candidateType) {
  if (!scheduler) return 1.0f;
  if (activeType < 0 || activeType >= 3) return 1.0f;
  if (candidateType < 0 || candidateType >= 3) return 1.0f;
  return scheduler->ticoMarginFactor[activeType][candidateType];
}
