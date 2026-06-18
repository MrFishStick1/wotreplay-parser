#include "animation_writer.h"
#include "fstream_ioctx.h"
#include "game.h"
#include "gd.h"
#include "gd_io.h"
#include "gdfontl.h"
#include "gdfontmb.h"
#include "gdfonts.h"
#include "gdfontt.h"
#include "logger.h"
#include "packet.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <optional>
#include <ranges>
#include <string>

#include <boost/filesystem.hpp>

using namespace wotreplay;

#ifdef _WIN32
#define WOT_POPEN _popen
#define WOT_PCLOSE _pclose
#define WOT_POPEN_MODE "wb"
#else
#define WOT_POPEN popen
#define WOT_PCLOSE pclose
#define WOT_POPEN_MODE "w"
#endif

const int TURRET_LINE_LENGTH = 20;
const float HIT_VISIBILITY_TIMEOUT = 2.5;

int animation_writer_t::update_model(const game_t &game, float window_start, float window_size, int packet_start) {
    int ix = packet_start;
    const auto &packets = game.get_packets();
    boost::container::flat_map<int, packet_t> turrets;

    float window_end = window_start + window_size;

    std::erase_if(hits, [=](const packet_t &p) { return p.clock() + HIT_VISIBILITY_TIMEOUT <= window_start; });

    while (ix < packets.size() && (!packets[ix].has_property(property_t::clock) || packets[ix].clock() <= window_end)) {
        if (packets[ix].has_property(property_t::position)) {
            tracks[packets[ix].player_id()].emplace_back(packets[ix]);
        }

        if (packets[ix].has_property(property_t::turret_orientation)) {
            turrets[packets[ix].player_id()] = packets[ix];
        }

        if (packets[ix].has_property(property_t::health)) {
            current_health[packets[ix].player_id()] = packets[ix];
        }

        if (packets[ix].has_property(property_t::max_health)) {
            max_health[packets[ix].player_id()] = packets[ix];
        }

        if (packets[ix].type() == 0x08 && packets[ix].sub_type() == 0x01 && packets[ix].has_property(property_t::source)) {
            hits.emplace_back(packets[ix]);
        }

        ix += 1;
    }

    for (const auto &[player_id, turret] : turrets) {
        this->turrets[player_id].emplace_back(turret);
    }

    return ix;
}

gdImagePtr animation_writer_t::create_background_frame(const game_t &game) const {
    gdImagePtr result = gdImageCreateTrueColor(this->image_width, this->image_height);

    if (no_basemap) {
        int t = gdImageColorAllocate(result, 0x01, 0x01, 0x01);
        gdImageFill(result, 0, 0, t);
        gdImageColorTransparent(result, t);
    } else {
        auto shape = base.shape();
        for (int i = 0; i < shape[0]; i += 1) {
            for (int j = 0; j < shape[1]; j += 1) {
                int c = gdTrueColor(base[i][j][0], base[i][j][1], base[i][j][2]);
                gdImageSetPixel(result, j, i, c);
            }
        }
    }

    return result;
}

void animation_writer_t::set_max_history(int max_history) { this->max_history = max_history; }

void animation_writer_t::set_show_orientation(bool show_orientation) { this->show_orientation = show_orientation; }
void animation_writer_t::set_use_player_health(bool use_player_health) { this->use_player_health = use_player_health; }

std::optional<packet_t> find_recent_position(const boost::container::flat_map<int, std::vector<packet_t>> &packets, int player_id, float clock) {
    if (!packets.contains(player_id)) {
        return std::nullopt;
    }

    const auto &player_packets = packets.at(player_id);

    const auto result = std::find_if(player_packets.rbegin(), player_packets.rend(), [=](const packet_t &p) { return std::abs(p.clock() - clock) < 1; });

    if (result == player_packets.rend()) {
        return std::nullopt;
    }

    return {*result};
}

// Returns the first sample after `clock`, or end() if there is none. `samples`
// must be sorted ascending by clock(), which is how tracks/turrets accumulate.
static std::vector<packet_t>::const_iterator upper_bound_by_clock(const std::vector<packet_t> &samples, float clock) {
    return std::upper_bound(samples.begin(), samples.end(), clock, [](float c, const packet_t &p) { return c < p.clock(); });
}

// Quantize an interpolation fraction so motion shows `steps` evenly-spaced
// intermediate points between the two surrounding packets (steps+1 equal segments,
// snapping to k/(steps+1)). A larger steps value is smoother; a very large value is
// effectively continuous. steps <= 0 leaves the fraction untouched.
static float quantize_fraction(float t, int steps) {
    if (steps > 0) {
        const float segments = (float)(steps + 1);
        t = std::round(t * segments) / segments;
    }
    return t;
}

// Linearly interpolate a player's position at exactly `clock`, between the two
// surrounding position packets. This smooths motion so it tracks the video frame
// rate instead of stepping at the (much lower) packet rate. `steps` controls how
// many intermediate points are placed between packets.
static std::tuple<float, float, float> interpolate_position(const std::vector<packet_t> &samples, float clock, int steps) {
    auto hi = upper_bound_by_clock(samples, clock);
    if (hi == samples.begin()) {
        return hi->position();
    }
    if (hi == samples.end()) {
        return samples.back().position();
    }

    const packet_t &lo = *(hi - 1);
    const float span = hi->clock() - lo.clock();
    const float t = quantize_fraction(span > 0.f ? (clock - lo.clock()) / span : 0.f, steps);

    const auto [x0, y0, z0] = lo.position();
    const auto [x1, y1, z1] = hi->position();
    return {x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, z0 + (z1 - z0) * t};
}

// Interpolate an angle accessor (hull/turret orientation) at `clock`, taking the
// shortest path around the circle so it never spins the wrong way across the seam.
static float interpolate_angle(const std::vector<packet_t> &samples, float clock, float (packet_t::*getter)() const, int steps) {
    auto hi = upper_bound_by_clock(samples, clock);
    if (hi == samples.begin()) {
        return ((*hi).*getter)();
    }
    if (hi == samples.end()) {
        return (samples.back().*getter)();
    }

    const packet_t &lo = *(hi - 1);
    const float span = hi->clock() - lo.clock();
    const float t = quantize_fraction(span > 0.f ? (clock - lo.clock()) / span : 0.f, steps);

    const float a0 = (lo.*getter)();
    const float a1 = ((*hi).*getter)();
    const float two_pi = 2.f * std::numbers::pi_v<float>;

    float d = std::fmod(a1 - a0 + std::numbers::pi_v<float>, two_pi);
    if (d < 0.f) {
        d += two_pi;
    }
    d -= std::numbers::pi_v<float>;

    return a0 + d * t;
}

gdImagePtr animation_writer_t::create_frame(const game_t &game, gdImagePtr background, float clock) const {
    gdImagePtr frame = gdImageCreateTrueColor(gdImageSX(background), gdImageSY(background));

    gdImageCopy(frame, background, 0, 0, 0, 0, gdImageSX(frame), gdImageSY(frame));

    int r = gdTrueColor(0xFF, 0x00, 0x00);
    int g = gdTrueColor(0x00, 0xFF, 0x00);
    int w = gdTrueColor(0xFF, 0xFF, 0xFF);
    int b = gdTrueColor(0x00, 0xFF, 0xFF);

    gdImageString(frame, gdFontLarge, 10, 10, (uint8_t *)std::format("{}", clock).c_str(), b);

    int recorder_team = game.get_team_id(game.get_recorder_id());
    int recorder_id = game.get_recorder_id();

    float f = image_width / (float)512;

    for (const auto &[player_id, player_info] : game.players) {
        if (!tracks.contains(player_id)) {
            continue;
        }

        const auto &positions = tracks.at(player_id);

        int player_team = game.get_team_id(player_id);
        if (player_team == -1) {
            continue;
        }

        int c;

        if (recorder_id == player_id) {
            c = b;
        } else if (player_team == -1) {
            c = w;
        } else if (player_team == recorder_team) {
            c = g;
        } else {
            c = r;
        }

        const auto player_position = (interpolate && interp_positions.contains(player_id))
                                         ? interpolate_position(interp_positions.at(player_id), clock, interpolate_steps)
                                         : positions.back().position();
        auto [player_x, player_y] = get_2d_coord(player_position, this->arena.bounding_box, this->image_width, this->image_height);

        bool is_visible = positions.back().clock() - clock > -5.f;
        bool is_alive = current_health.contains(player_id) && current_health.at(player_id).health() > 0;
        bool is_hit = std::find_if(hits.begin(), hits.end(), [&](const packet_t &p) { return p.player_id() == player_id; }) != hits.end();

        // tracks
        if (!use_player_health || is_alive) {
            gdImageAlphaBlending(frame, gdEffectAlphaBlend);
            int history_pos = 0;
            for (const auto &packet : positions | std::views::reverse) {
                if (max_history != -1 && history_pos >= max_history) {
                    break;
                }

                auto [x, y] = get_2d_coord(packet.position(), this->arena.bounding_box, this->image_width, this->image_height);

                float p = ((float)(history_pos) / (float)max_history);
                int blend = gdTrueColorAlpha(gdTrueColorGetRed(c), gdTrueColorGetGreen(c), gdTrueColorGetBlue(c), (int)(128 * (p * p * p)));

                gdImageSetPixel(frame, x, y, blend);

                history_pos += 1;
            }
            gdImageAlphaBlending(frame, gdEffectReplace);
        }

        // render player name
        if (!use_player_health || is_alive) {
            gdFontPtr nameFont;

            if (image_width >= 1024) {
                nameFont = gdFontMediumBold;
            } else if (image_height >= 512) {
                nameFont = gdFontSmall;
            } else {
                nameFont = gdFontTiny;
            }

            int char_size = 6;
            const auto player_display_name = player_info.name;
            int left_offset = player_x - 10 - player_display_name.length() * char_size;

            gdImageAlphaBlending(frame, gdEffectAlphaBlend);
            gdImageFilledRectangle(frame, left_offset - 2, player_y + 2, player_x - 10, player_y + 12, gdTrueColorAlpha(0x00, 0x00, 0x00, 0x40));
            gdImageAlphaBlending(frame, gdEffectReplace);
            gdImageString(frame, nameFont, left_offset, player_y, (uint8_t *)player_display_name.c_str(), c);
        }

        // render player health bar
        if (is_alive && current_health.contains(player_id) && max_health.contains(player_id)) {
            float f = ((float)current_health.at(player_id).health()) / ((float)max_health.at(player_id).max_health());

            gdImageFilledRectangle(frame, player_x - 42, player_y - 3, player_x - 12, player_y + 0, r);

            if (is_hit) {
                gdImageFilledRectangle(frame, player_x - 42, player_y - 3, player_x - 42 + 30 * f, player_y + 0, gdTrueColor(0xFF, 0xFF, 0x00));
            } else if (f > 0) {
                gdImageFilledRectangle(frame, player_x - 42, player_y - 3, player_x - 42 + 30 * f, player_y + 0, g);
            }

            gdImageRectangle(frame, player_x - 42, player_y - 3, player_x - 12, player_y + 0, gdTrueColor(0x00, 0x00, 0x00));
        }

        // render turrets
        if ((!use_player_health || is_alive) && show_turrets && turrets.contains(player_id)) {
            const auto &turret_samples = turrets.at(player_id);
            const auto turret_angle = (interpolate && interp_turrets.contains(player_id))
                                          ? interpolate_angle(interp_turrets.at(player_id), clock, &packet_t::turret_orientation, interpolate_steps)
                                          : turret_samples.back().turret_orientation();
            const auto hull_angle = (interpolate && interp_positions.contains(player_id))
                                        ? interpolate_angle(interp_positions.at(player_id), clock, &packet_t::hull_orientation, interpolate_steps)
                                        : positions.back().hull_orientation();
            const auto t = turret_angle + hull_angle;

            const std::array<std::tuple<float, int>, 4> turret_lines = {
                std::make_tuple(0.0f, r),
                std::make_tuple(1.0f, w),
                std::make_tuple(2.0f, g),
                std::make_tuple(3.0f, b),
            };

            for (const auto [r, c] : turret_lines) {
                if (debug) {
                    gdImageLine(frame, player_x, player_y, std::round(player_x + f * TURRET_LINE_LENGTH * std::cos(t + r * std::numbers::pi / 2)),
                                std::round(player_y + f * TURRET_LINE_LENGTH * std::sin(t + r * std::numbers::pi / 2)), c);
                } else if (c == w) {
                    gdImageAlphaBlending(frame, gdEffectAlphaBlend);
                    gdImageSetAntiAliased(frame, int gdTrueColorAlpha(0xFF, 0xFF, 0xFF, 0x40));
                    gdImageFilledArc(frame, player_x, player_y, f * TURRET_LINE_LENGTH * 2, f * TURRET_LINE_LENGTH * 2,
                                     (t + r * std::numbers::pi / 2) * 180.f / std::numbers::pi - 30.f,
                                     (t + r * std::numbers::pi / 2) * 180.f / std::numbers::pi + 30.f, gdAntiAliased, gdArc);
                    gdImageSetAntiAliased(frame, int gdTrueColor(0xFF, 0xFF, 0xFF));
                    gdImageFilledArc(frame, player_x, player_y, f * TURRET_LINE_LENGTH * 2, f * TURRET_LINE_LENGTH * 2,
                                     (t + r * std::numbers::pi / 2) * 180.f / std::numbers::pi - 30.f,
                                     (t + r * std::numbers::pi / 2) * 180.f / std::numbers::pi + 30.f, gdAntiAliased, gdEdged | gdNoFill);
                    gdImageAlphaBlending(frame, gdEffectReplace);
                }
            }
        }

        // render tank
        if (show_orientation) {
            const auto o = (interpolate && interp_positions.contains(player_id))
                               ? interpolate_angle(interp_positions.at(player_id), clock, &packet_t::hull_orientation, interpolate_steps)
                               : positions.back().hull_orientation();

            if (debug) {
                gdImageLine(frame, player_x, player_y, player_x + f * TURRET_LINE_LENGTH * std::cos(o - std::numbers::pi / 2),
                            player_y + f * TURRET_LINE_LENGTH * std::sin(o - std::numbers::pi / 2), b);
            } else {
                gdImageSetAntiAliased(frame, c);
                gdImageFilledArc(frame, player_x + f * TURRET_LINE_LENGTH / 4 * std::cos(o - std::numbers::pi / 2),
                                 player_y + f * TURRET_LINE_LENGTH / 4 * std::sin(o - std::numbers::pi / 2), f * TURRET_LINE_LENGTH / 1.5, f * TURRET_LINE_LENGTH / 1.5,
                                 (o - 3 * std::numbers::pi / 2) * 180.f / std::numbers::pi - 22.5f,
                                 (o - 3 * std::numbers::pi / 2) * 180.f / std::numbers::pi + 22.5f, gdAntiAliased, gdChord);
                gdImageSetAntiAliased(frame, gdTrueColor(0x00, 0x00, 0x00));
                gdImageFilledArc(frame, player_x + f * TURRET_LINE_LENGTH / 4 * std::cos(o - std::numbers::pi / 2),
                                 player_y + f * TURRET_LINE_LENGTH / 4 * std::sin(o - std::numbers::pi / 2), f * TURRET_LINE_LENGTH / 1.5, f * TURRET_LINE_LENGTH / 1.5,
                                 (o - 3 * std::numbers::pi / 2) * 180.f / std::numbers::pi - 22.5f,
                                 (o - 3 * std::numbers::pi / 2) * 180.f / std::numbers::pi + 22.5f, gdAntiAliased, gdEdged | gdNoFill);
            }
        } else {
            gdImageFilledRectangle(frame, player_x - 2, player_y - 2, player_x + 2, player_y + 2, c);
            gdImageRectangle(frame, player_x - 2, player_y - 2, player_x + 2, player_y + 2, gdTrueColor(0x00, 0x00, 0x00));
        }

        // render hits
        for (const auto &hit : hits) {
            const auto &player_position = find_recent_position(tracks, hit.player_id(), hit.clock());

            if (!player_position.has_value()) {
                logger.writef(log_level_t::warning, "[animation_writer] unable to locate player_id=%1% data=%2%\n", hit.player_id(), hit);
                continue;
            }

            const auto &source_position = find_recent_position(tracks, hit.source(), hit.clock());
            if (!source_position.has_value()) {
                logger.writef(log_level_t::warning, "[animation_writer] unable to locate source=%1% data=%2%\n", hit.source(), hit);
                continue;
            }

            auto [target_x, target_y] = get_2d_coord(player_position->position(), this->arena.bounding_box, this->image_width, this->image_height);

            auto [source_x, source_y] = get_2d_coord(source_position->position(), this->arena.bounding_box, this->image_width, this->image_height);
            gdImageLine(frame, target_x, target_y, source_x, source_y, gdTrueColor(0xFF, 0xFF, 0x00));
        }
    }

    if (no_basemap) {
        gdImageColorTransparent(frame, gdImageGetTransparent(background));
    }

    if (debug) {
        static int debug_frame_nr = 0;
        static std::set<int> rendered_hits;

        for (const auto &hit : hits) {
            const int hit_id = (int)hit.clock() * 1000;
            if (rendered_hits.contains(hit_id)) {
                continue;
            }

            gdImagePtr debug_frame = gdImageCreateTrueColor(gdImageSX(background), gdImageSY(background));
            gdImageCopy(debug_frame, background, 0, 0, 0, 0, gdImageSX(debug_frame), gdImageSY(debug_frame));

            gdImageString(debug_frame, gdFontLarge, 10, 10, (uint8_t *)std::format("[{}] {}", debug_frame_nr, clock).c_str(), b);

            const auto &player_position = find_recent_position(tracks, hit.player_id(), hit.clock());

            if (!player_position.has_value()) {
                logger.writef(log_level_t::warning, "[animation_writer] unable to locate player_id=%1% data=%2%\n", hit.player_id(), hit);
                continue;
            }

            const auto &source_position = find_recent_position(tracks, hit.source(), hit.clock());
            if (!source_position.has_value()) {
                logger.writef(log_level_t::warning, "[animation_writer] unable to locate source=%1% data=%2%\n", hit.source(), hit);
                continue;
            }

            if (!tracks.contains(hit.source())) {
                logger.writef(log_level_t::warning, "[animation_writer] unable to locate source=%1% data=%2%\n", hit.source(), hit);
                continue;
            }

            if (!turrets.contains(hit.source())) {
                logger.writef(log_level_t::warning, "[animation_writer] unable to locate source=%1% data=%2%\n", hit.source(), hit);
                continue;
            }

            auto filtered = game.get_packets() | std::views::filter([=](const packet_t &p) { return p.has_property(property_t::turret_orientation); }) |
                            std::views::filter([=](const packet_t &p) { return p.player_id() == hit.source(); }) | std::views::common;

            auto turret = std::min_element(filtered.begin(), filtered.end(), [=](const packet_t &left, const packet_t &right) {
                return std::abs(left.clock() - hit.clock()) < std::abs(right.clock() - hit.clock());
            });

            auto [target_x, target_y] = get_2d_coord(player_position->position(), this->arena.bounding_box, this->image_width, this->image_height);

            auto [source_x, source_y] = get_2d_coord(source_position->position(), this->arena.bounding_box, this->image_width, this->image_height);
            gdImageLine(debug_frame, target_x, target_y, source_x, source_y, gdTrueColor(0xFF, 0xFF, 0x00));

            const auto o = tracks.at(hit.source()).back().hull_orientation();
            gdImageLine(debug_frame, source_x, source_y, source_x + f * TURRET_LINE_LENGTH * std::cos(o - std::numbers::pi / 2),
                        source_y + f * TURRET_LINE_LENGTH * std::sin(o - std::numbers::pi / 2), b);

            const auto t = turrets.at(hit.source()).back().turret_orientation() + tracks.at(hit.source()).back().hull_orientation();

            const std::array<std::tuple<float, int>, 4> turret_lines = {
                std::make_tuple(1.0f, r),
                std::make_tuple(0.0f, w),
                std::make_tuple(2.0f, g),
                std::make_tuple(3.0f, b),
            };

            for (const auto [r, c] : turret_lines) {
                gdImageLine(debug_frame, source_x, source_y, std::round(source_x + f * TURRET_LINE_LENGTH * std::cos(t + r * std::numbers::pi / 2)),
                            std::round(source_y + f * TURRET_LINE_LENGTH * std::sin(t + r * std::numbers::pi / 2)), c);
            }

            const auto file_name = std::format("{}/debug-{:010}.png", this->raw_images_path.length() == 0 ? "." : this->raw_images_path, debug_frame_nr);
            std::ofstream of(file_name, std::ios::binary | std::ios::out);
            OfstreamIOCtx ctx(of);
            gdImagePngCtx(debug_frame, (gdIOCtxPtr)&ctx);

            debug_frame_nr += 1;
            rendered_hits.emplace(hit_id);
            gdImageDestroy(debug_frame);
        }
    }

    return frame;
}

void animation_writer_t::write(std::ostream &os) {
    if (mp4_output) {
        // ffmpeg already wrote the mp4 directly to output_path during update();
        // there is nothing to stream to os.
        return;
    }

    // The GIF has already been streamed to gif_file through ctx during update().
    // Release the GD context (this does not close the FILE), then flush, rewind
    // and copy the file contents to the output stream.
    if (ctx != nullptr) {
        ctx->gd_free(ctx);
        ctx = nullptr;
    }

    std::fflush(gif_file);
    std::rewind(gif_file);

    char buffer[64 * 1024];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), gif_file)) > 0) {
        os.write(buffer, n);
    }
    os.flush();
}

void animation_writer_t::set_model_update_rate(int model_update_rate) { this->model_update_rate = model_update_rate; }

void animation_writer_t::set_frame_rate(int frame_rate) { this->frame_rate = frame_rate; }

void animation_writer_t::set_show_turrets(bool show_turrets) { this->show_turrets = show_turrets; }

void animation_writer_t::set_skip(double skip) { this->skip = skip; }

void animation_writer_t::set_debug(bool debug) { this->debug = debug; }

void animation_writer_t::set_real_time(bool real_time) { this->real_time = real_time; }

void animation_writer_t::set_interpolate(bool interpolate) { this->interpolate = interpolate; }

void animation_writer_t::set_interpolate_steps(int interpolate_steps) { this->interpolate_steps = interpolate_steps; }

void animation_writer_t::set_mp4(bool mp4) { this->mp4_output = mp4; }

void animation_writer_t::set_output_path(const std::string &output_path) { this->output_path = output_path; }

void animation_writer_t::set_ffmpeg_path(const std::string &ffmpeg_path) { this->ffmpeg_path = ffmpeg_path; }

void animation_writer_t::update(const game_t &game) {
    draw_basemap();

    if (interpolate) {
        // Build complete per-player position/turret timelines so motion can be
        // interpolated toward future samples. (tracks/turrets only reach the
        // current frame, which is too late to interpolate against.)
        for (const auto &p : game.get_packets()) {
            if (p.has_property(property_t::position)) {
                interp_positions[p.player_id()].emplace_back(p);
            }
            if (p.has_property(property_t::turret_orientation)) {
                interp_turrets[p.player_id()].emplace_back(p);
            }
        }
    }

    gdImagePtr previous = NULL, background = create_background_frame(game), frame = create_frame(game, background, 0.f);

    float window_start = 0.f;

    if (mp4_output) {
        // Stream frames straight into ffmpeg so no large intermediate file (gif
        // or png sequence) is ever produced; ffmpeg writes the final mp4 itself.
        // The pad filter rounds odd dimensions up to even, which yuv420p/x264 require.
        // ffmpeg_path is quoted so a full path containing spaces works; launching
        // ffmpeg by full path also lets Windows find its sibling DLLs (shared builds).
        // Force the mp4 muxer (-f mp4) so the container is always mp4 regardless of
        // the output filename's extension -- otherwise ffmpeg infers the muxer from
        // the extension and e.g. a ".gif" output name makes it reject the h264 stream.
        const auto inner = std::format("\"{}\" -y -f image2pipe -vcodec bmp -framerate {} -i - -c:v libx264 -crf 22 -pix_fmt yuv420p "
                                       "-vf \"pad=ceil(iw/2)*2:ceil(ih/2)*2\" -f mp4 \"{}\"",
                                       ffmpeg_path, frame_rate, output_path);
#ifdef _WIN32
        // _popen runs this through `cmd.exe /c`; when the command starts with a quote,
        // cmd strips the first and last quote, which would break a quoted path with
        // spaces. Wrapping the whole command in an extra outer pair makes cmd strip
        // that pair instead, leaving the inner quotes intact.
        const auto cmd = "\"" + inner + "\"";
#else
        const auto cmd = inner;
#endif
        ffmpeg_pipe = WOT_POPEN(cmd.c_str(), WOT_POPEN_MODE);
        if (ffmpeg_pipe == nullptr) {
            throw std::runtime_error("unable to launch ffmpeg for mp4 output (is ffmpeg on PATH?)");
        }
    } else {
        gdImageGifAnimBeginCtx(background, ctx, 1, 0);
    }

    const auto &packets = game.get_packets();

    int ix = 0, total_packets = packets.size();

    int frame_count = 0;

    float df = 1.f / frame_rate;
    float dm = (float)model_update_rate / frame_rate;

    int frame_nr = 0;
    int rendered_frame_nr = 0;
    while (ix < total_packets) {
        frame_nr += 1;

        for (float ds = 0; ds < df && ix < total_packets; ds += dm) {
            if (real_time) {
                // Advance the clock by exactly dm so each frame represents a
                // fixed slice of in-game time. This plays back at true real time
                // (for model-update-rate 1) but renders idle gaps instead of
                // skipping them.
                ix = this->update_model(game, window_start, dm, ix);
                window_start += dm;
            } else {
                // Snap to the next packet's timestamp. Skips idle gaps, but
                // overshoots dm slightly each frame, so playback runs a touch
                // faster than real time.
                window_start = packets[ix].clock();
                ix = this->update_model(game, window_start, dm, ix);
            }
        }

        if (window_start < skip) {
            continue;
        }

        rendered_frame_nr += 1;
        frame = create_frame(game, background, window_start);

        logger.writef(log_level_t::info, "generating gif frame frame_nr=%1% rendered_frame_nr=%2% window_start=%3%\n", frame_nr, rendered_frame_nr,
                      window_start);

        if (!raw_images_path.empty()) {
            const auto file_name = std::format("{}/frame-{:010}.png", raw_images_path, frame_nr);
            std::ofstream of(file_name, std::ios::binary | std::ios::out);
            OfstreamIOCtx ctx(of);
            gdImagePngCtx(frame, (gdIOCtxPtr)&ctx);
        }

        if (mp4_output) {
            // Hand the frame to ffmpeg as an uncompressed BMP, then free it right
            // away. BMP avoids the zlib compress (here) and decompress (in ffmpeg)
            // that PNG would cost on both ends of the pipe -- that compression was
            // the dominant per-frame cost and the reason the pipe lagged. Each
            // frame is still "used and discarded" so nothing piles up.
            int bmp_size = 0;
            void *bmp_data = gdImageBmpPtr(frame, &bmp_size, 0);
            if (bmp_data != nullptr) {
                std::fwrite(bmp_data, 1, bmp_size, ffmpeg_pipe);
                gdFree(bmp_data);
            }
            gdImageDestroy(frame);
            frame = nullptr;
        } else {
            gdImageTrueColorToPalette(frame, 1, 255);
            gdImageGifAnimAddCtx(frame, ctx, 1, 0, 0, (int)(df * 100), gdDisposalNone, previous);

            if (previous) {
                gdImageDestroy(previous);
            }

            previous = frame;
        }
    }

    if (mp4_output) {
        if (ffmpeg_pipe != nullptr) {
            WOT_PCLOSE(ffmpeg_pipe);
            ffmpeg_pipe = nullptr;
        }
    } else {
        if (frame) {
            gdImageDestroy(frame);
        }
        gdImageGifAnimEndCtx(ctx);
    }

    gdImageDestroy(background);
}

void animation_writer_t::set_raw_images_path(const std::string &raw_images_path) { this->raw_images_path = raw_images_path; }

void animation_writer_t::init(const arena_t &arena, const std::string &mode) {
    image_writer_t::init(arena, mode);

    if (!mp4_output) {
        // Stream the GIF to a temporary file rather than an in-memory buffer. Large
        // animations can exceed any fixed buffer size and overflow, so we let the GD
        // context write straight to disk and copy the file out in write(). The mp4
        // path pipes frames to ffmpeg instead and needs no GD context at all.
        gif_file_path = (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("wotreplay-%%%%-%%%%-%%%%-%%%%.gif")).string();
        gif_file = std::fopen(gif_file_path.c_str(), "wb+");
        if (gif_file == nullptr) {
            throw std::runtime_error("unable to open temporary file for animation output: " + gif_file_path);
        }
        ctx = gdNewFileCtx(gif_file);
    }

    if (!raw_images_path.empty() && !boost::filesystem::exists(raw_images_path)) {
        logger.writef(log_level_t::info, "create raw images directory: %1%\n", raw_images_path);
        boost::filesystem::create_directory(raw_images_path);
    }
}

void animation_writer_t::finish() {}

animation_writer_t::~animation_writer_t() {
    if (ffmpeg_pipe != nullptr) {
        WOT_PCLOSE(ffmpeg_pipe);
        ffmpeg_pipe = nullptr;
    }

    if (ctx != nullptr) {
        ctx->gd_free(ctx);
        ctx = nullptr;
    }

    if (gif_file != nullptr) {
        std::fclose(gif_file);
        gif_file = nullptr;
    }

    if (!gif_file_path.empty()) {
        boost::system::error_code ec;
        boost::filesystem::remove(gif_file_path, ec);
    }
}
