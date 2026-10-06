#include <cstdio>

#include "libs/Kernel.h"
#include "libs/PublicDataRequest.h"
#include "modules/utils/player/PlayerPublicAccess.h"

namespace {

int checks;
int failures;
const char* current_test;

#define CHECK(condition)                                                      \
  do {                                                                        \
    ++checks;                                                                 \
    if (!(condition)) {                                                       \
      ++failures;                                                             \
      std::printf("  FAIL (%s:%d) %s\n", current_test, __LINE__, #condition); \
    }                                                                         \
  } while (false)

#define TEST(name)     \
  current_test = name; \
  std::printf("%s\n", name)

// What the Player module would report, and whether it is loaded at all.
bool player_loaded = true;
bool player_playing_file = false;
bool player_suspending = false;

// The is_playing and is_suspended answer from Player::on_get_public_data(),
// unchanged: Player hands back a pointer to its own static bool rather than
// writing into the caller's storage.
void player_on_get_public_data(void* argument) {
  PublicDataRequest* pdr = static_cast<PublicDataRequest*>(argument);

  if (!pdr->starts_with(player_checksum)) return;

  if (pdr->second_element_is(is_playing_checksum) || pdr->second_element_is(is_suspended_checksum)) {
    static bool bool_data;
    bool_data = pdr->second_element_is(is_playing_checksum) ? player_playing_file : player_suspending;
    pdr->set_data_ptr(&bool_data);
    pdr->set_taken();
  }
}

Kernel kernel;

}  // namespace

Kernel* Kernel::instance = &kernel;

void Kernel::call_event(_EVENT_ENUM id_event, void* argument) {
  if (id_event == ON_GET_PUBLIC_DATA && player_loaded) player_on_get_public_data(argument);
}

int main() {
  {
    TEST("a file playing reads as playing");
    player_loaded = true;
    player_playing_file = true;
    CHECK(player_is_playing());
  }
  {
    TEST("no file playing reads as not playing");
    player_loaded = true;
    player_playing_file = false;
    CHECK(!player_is_playing());
  }
  {
    TEST("each call reads the current value");
    player_loaded = true;
    player_playing_file = false;
    CHECK(!player_is_playing());
    player_playing_file = true;
    CHECK(player_is_playing());
    player_playing_file = false;
    CHECK(!player_is_playing());
  }
  {
    TEST("a suspended machine with no file playing reads as not playing");
    player_loaded = true;
    player_playing_file = false;
    player_suspending = true;
    CHECK(!player_is_playing());
    player_suspending = false;
  }
  {
    TEST("no Player module loaded reads as not playing");
    player_loaded = false;
    player_playing_file = true;
    CHECK(!player_is_playing());
    player_loaded = true;
  }

  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
