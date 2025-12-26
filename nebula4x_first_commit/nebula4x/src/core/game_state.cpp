#include "nebula4x/core/game_state.h"

namespace nebula4x {

Id allocate_id(GameState& s) {
  const Id id = s.next_id;
  s.next_id++;
  if (s.next_id == kInvalidId) s.next_id++; // avoid 0
  return id;
}

} // namespace nebula4x
