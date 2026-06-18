#pragma once
#include "packet.h"
#include <boost/container/flat_map.hpp>
#include <vector>
#ifndef wotreplay_animation_writer_h
#define wotreplay_animation_writer_h

#include "image_writer.h"

#include <deque>
#include <gd.h>

namespace wotreplay {
class animation_writer_t : public image_writer_t {
  public:
    virtual void write(std::ostream &os);
    virtual void update(const game_t &game);
    virtual void finish();
    virtual void init(const arena_t &arena, const std::string &mode);
    virtual ~animation_writer_t();
    int update_model(const game_t &game, float window_start, float window_size, int packet_start);
    gdImagePtr create_frame(const game_t &game, gdImagePtr background, float clock) const;
    gdImagePtr create_background_frame(const game_t &game) const;
    virtual void set_frame_rate(int frame_rate);
    virtual void set_model_update_rate(int model_update_rate);
    void set_max_history(int max_history);
    void set_raw_images_path(const std::string &raw_images_path);
    void set_show_turrets(bool show_turrets);
    void set_show_orientation(bool show_orientation);
    void set_use_player_health(bool use_player_health);
    void set_skip(double skip);
    void set_debug(bool debug);
    void set_real_time(bool real_time);
    void set_interpolate(bool interpolate);
    void set_interpolate_steps(int interpolate_steps);
    void set_mp4(bool mp4);
    void set_output_path(const std::string &output_path);
    void set_ffmpeg_path(const std::string &ffmpeg_path);

  private:
    gdIOCtx *ctx = nullptr;
    FILE *gif_file = nullptr;
    std::string gif_file_path;
    boost::container::flat_map<int, std::vector<packet_t>> turrets;
    boost::container::flat_map<int, std::vector<packet_t>> tracks;
    // Complete per-player timelines (all packets, built once up front) used only for
    // interpolation, which needs samples ahead of the current frame -- the incremental
    // tracks/turrets above never reach past the frame being drawn.
    boost::container::flat_map<int, std::vector<packet_t>> interp_positions;
    boost::container::flat_map<int, std::vector<packet_t>> interp_turrets;
    boost::container::flat_map<int, packet_t> current_health;
    boost::container::flat_map<int, packet_t> max_health;
    std::vector<packet_t> hits;
    int frame_rate, model_update_rate;
    int max_history;
    std::string raw_images_path;
    bool show_turrets;
    bool show_orientation;
    bool use_player_health;
    double skip;
    bool debug;
    bool real_time = false;
    bool interpolate = false;
    int interpolate_steps = 0;
    bool mp4_output = false;
    std::string output_path;
    std::string ffmpeg_path = "ffmpeg";
    FILE *ffmpeg_pipe = nullptr;
};
} // namespace wotreplay

#endif
