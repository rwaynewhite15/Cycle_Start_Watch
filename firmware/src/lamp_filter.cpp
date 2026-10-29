#include "lamp_filter.h"

void LampFilter::configure(const FilterConfig& cfg) {
  if (cfg.invert != cfg_.invert) {
    // Start over so the flip does not look like a lamp edge. The state under
    // the new meaning is reported again after the normal settle time.
    started_ = false;
    reported_ = LAMP_UNKNOWN;
  }
  cfg_ = cfg;
  if (cfg_.blink_edges < 2) cfg_.blink_edges = 2;
  if (cfg_.blink_edges > kMaxEdges) cfg_.blink_edges = kMaxEdges;
}

void LampFilter::addEdge(uint32_t t) {
  if (edge_count_ == kMaxEdges) {
    for (uint8_t i = 1; i < kMaxEdges; i++) edges_[i - 1] = edges_[i];
    edge_count_--;
  }
  edges_[edge_count_++] = t;
  last_edge_ms_ = t;
}

uint8_t LampFilter::edgesWithin(uint32_t now_ms, uint32_t window_ms, uint32_t* first) const {
  uint8_t n = 0;
  for (int i = edge_count_ - 1; i >= 0; i--) {
    if (now_ms - edges_[i] > window_ms) break;
    n++;
    *first = edges_[i];
  }
  return n;
}

bool LampFilter::update(bool active, uint32_t now_ms, StateChange* out) {
  // Stage 1: pulse stretch on the electrical signal.
  if (active) {
    last_active_ms_ = now_ms;
    seen_active_ = true;
  }
  bool level = seen_active_ && (now_ms - last_active_ms_ < cfg_.stretch_ms);
  if (cfg_.invert) level = !level;

  if (!started_) {
    started_ = true;
    candidate_ = stable_ = level;
    candidate_since_ = now_ms;
    last_edge_ms_ = now_ms;  // start of observation counts as the "edge"
    edge_count_ = 0;
  }

  // Stage 2: debounce.
  if (level != candidate_) {
    candidate_ = level;
    candidate_since_ = now_ms;
  }
  if (candidate_ != stable_ && now_ms - candidate_since_ >= cfg_.debounce_ms) {
    stable_ = candidate_;
    addEdge(candidate_since_);  // the edge happened when the level first changed
  }

  // Stage 3: blink detection and settle.
  uint32_t first_edge = 0;
  uint8_t recent = edgesWithin(now_ms, cfg_.blink_window_ms, &first_edge);
  LampState next = reported_;
  uint32_t since = 0;

  if (reported_ != LAMP_BLINK && recent >= cfg_.blink_edges) {
    next = LAMP_BLINK;
    since = first_edge;
  } else if (now_ms - last_edge_ms_ >= cfg_.settle_ms) {
    next = stable_ ? LAMP_RUN : LAMP_STOP;
    since = last_edge_ms_;
    // A steady level ends any flashing: only edges after this count toward
    // the next blink detection.
    edge_count_ = 0;
  }

  if (next != reported_) {
    reported_ = next;
    if (out) {
      out->state = next;
      out->since_ms = since;
    }
    return true;
  }
  return false;
}
