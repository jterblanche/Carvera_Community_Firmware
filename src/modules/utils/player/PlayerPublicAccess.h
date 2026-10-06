#ifndef PLAYERPUBLICACCESS_H
#define PLAYERPUBLICACCESS_H

#include <cstdint>
#include <string>

#include "libs/PublicData.h"
#include "libs/checksumm.h"

#define player_checksum           CHECKSUM("player")
#define is_playing_checksum       CHECKSUM("is_playing")
#define is_suspended_checksum     CHECKSUM("is_suspended")
#define abort_play_checksum       CHECKSUM("abort_play")
#define get_progress_checksum     CHECKSUM("progress")
#define inner_playing_checksum    CHECKSUM("inner_playing")
#define restart_job_checksum    CHECKSUM("restart_job")
#define suspend_play_checksum   CHECKSUM("suspend_play")
#define resume_play_checksum    CHECKSUM("resume_play")
#define link_packet_checksum    CHECKSUM("link_packet")

struct player_link_packet {
    uint8_t type;
    const uint8_t* data;
    uint16_t data_length;
};

struct pad_progress {
    unsigned int percent_complete;
    unsigned long played_lines;
    unsigned long elapsed_secs;
    std::string filename;
    bool is_playing;  // true only while file is actively playing (not paused, not finished)
    unsigned long parsed_lines;
};

// Whether a file is playing. Player answers is_playing with a pointer to its
// own bool, so PublicData::get_value() is given a pointer to fill in, not a
// bool. False when no Player answers.
inline bool player_is_playing()
{
    void *returned = nullptr;
    if (!PublicData::get_value(player_checksum, is_playing_checksum, &returned) || returned == nullptr) return false;
    return *static_cast<bool *>(returned);
}
#endif
