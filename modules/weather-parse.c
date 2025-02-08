#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weather.h"

#define check_pos_impl(p, op, m) \
    do { \
        if ((p) op (m)) { \
            fprintf(stderr, "failed to parse at %d\n", __LINE__); \
            parse_fail = 1; \
        } \
    } while (0)
#define check_parse(end, pos) check_pos_impl(end, !=, pos)

static int
parse_line(struct weather_info *info, const char* line, size_t len)
{
    const char wind_prefix[] = "Wind: ";
    const char visibility_prefix[] = "Visibility: ";
    const char sky_conditions_prefix[] = "Sky conditions: ";
    const char weather_prefix[] = "Weather: ";
    const char temperature_prefix[] = "Temperature: ";
    const char heat_index_prefix[] = "Heat index: ";
    const char dew_point_prefix[] = "Dew Point: ";
    const char relative_humidity_prefix[] = "Relative Humidity: ";
    const char pressure_prefix[] = "Pressure (altimeter): ";

    const char* lend = line + len;
    int parse_fail = 0;

#define has_prefix(l, s, p) ((sizeof(p) - 1) <= s && !strncmp(l, p, sizeof(p) - 1))

    // Wind: ...
    if (has_prefix(line, len, wind_prefix)) {
        line += sizeof(wind_prefix) - 1;
        len -= sizeof(wind_prefix) - 1;

        const char calm_indicator[] = "Calm:0";
        const char variable_indicator[] = "Variable at ";
        const char normal_indicator[] = "from the ";

        // Wind: Calm:0
        if (has_prefix(line, len, calm_indicator)) {
            info->wind_direction = strdup("μ");
            info->wind_azimuth = -1;
            info->wind_mph = 0;
            info->wind_knots = 0;
            info->wind_kmph = 0;
            info->wind_mps = 0;
        // Wind: Variable at N MPH (N KT)
        } else if (has_prefix(line, len, variable_indicator)) {
            line += sizeof(variable_indicator) - 1;
            len -= sizeof(variable_indicator) - 1;

            info->wind_direction = strdup("μ");
            info->wind_azimuth = -1;

            size_t spn = strcspn(line, " ");
            char* end = NULL;
            long long int_parse = strtoll(line, &end, 10);
            check_parse(end, line + spn);
            line += spn + 1;
            check_pos_impl(line, >=, lend);

            info->wind_mph = int_parse;

            spn = strcspn(line, "(");
            line += spn + 1;
            check_pos_impl(line, >=, lend);

            int_parse = strtoll(line, &end, 10);
            line = end;
            check_pos_impl(line, >=, lend);

            info->wind_knots = int_parse;
            info->wind_kmph = int_parse * 1.852;
            info->wind_mps = int_parse * 0.514;
        // Wind: from the DIR (AZI degrees) at N MPH (N KT) ...
        } else if (has_prefix(line, len, normal_indicator)) {
            line += sizeof(normal_indicator) - 1;
            len -= sizeof(normal_indicator) - 1;

            size_t spn = strcspn(line, " ");
            check_pos_impl(line + spn + 2, >=, lend);
            info->wind_direction = strndup(line, spn);

            line += spn + 2;
            check_pos_impl(line, >=, lend);

            spn = strcspn(line, " ");
            char* end = NULL;
            long long int_parse = strtoll(line, &end, 10);
            check_parse(end, line + spn);

            info->wind_azimuth = int_parse;

            spn = strcspn(line, "t");
            line += spn + 2;
            check_pos_impl(line, >=, lend);

            spn = strcspn(line, " ");
            int_parse = strtoll(line, &end, 10);
            check_parse(end, line + spn);

            info->wind_mph = int_parse;

            spn = strcspn(line, "(");
            line += spn + 1;
            check_pos_impl(line, >=, lend);

            spn = strcspn(line, " ");
            int_parse = strtoll(line, &end, 10);
            check_parse(end, line + spn);

            info->wind_knots = int_parse;
            info->wind_kmph = int_parse * 1.852;
            info->wind_mps = int_parse * 0.514;
        } else {
            fprintf(stderr, "Unknown 'Wind' format: '%.*s'\n", (int)len, line);
            parse_fail = 1;
        }
    // Visibility: VIS
    } else if (has_prefix(line, len, visibility_prefix)) {
        line += sizeof(visibility_prefix) - 1;
        len -= sizeof(visibility_prefix) - 1;

        size_t spn = strcspn(line, ":");
        check_pos_impl(line + spn, >=, lend);

        free(info->visibility);
        info->visibility = strndup(line, spn);
    // Sky conditions: SKY
    } else if (has_prefix(line, len, sky_conditions_prefix)) {
        line += sizeof(sky_conditions_prefix) - 1;
        len -= sizeof(sky_conditions_prefix) - 1;

        size_t spn = strcspn(line, "\n");
        check_pos_impl(line + spn, >, lend);

        free(info->sky_condition);
        info->sky_condition = strndup(line, spn);
    // Weather: WEATHER
    } else if (has_prefix(line, len, weather_prefix)) {
        line += sizeof(weather_prefix) - 1;
        len -= sizeof(weather_prefix) - 1;

        size_t spn = strcspn(line, "\n");
        check_pos_impl(line + spn, >, lend);

        free(info->weather);
        info->weather = strndup(line, spn);
    // Temperature: T F (T C)
    } else if (has_prefix(line, len, temperature_prefix)) {
        line += sizeof(temperature_prefix) - 1;
        len -= sizeof(temperature_prefix) - 1;

        size_t spn = strcspn(line, " ");
        char* end = NULL;
        double double_parse = strtod(line, &end);
        check_parse(end, line + spn);
        line += spn + 1;
        check_pos_impl(line, >=, lend);

        info->temp_f = double_parse;

        spn = strcspn(line, "(");
        line += spn + 1;
        check_pos_impl(line, >=, lend);

        spn = strcspn(line, " ");
        double_parse = strtod(line, &end);
        check_parse(end, line + spn);

        info->temp_c = double_parse;
    // Heat index: T F (T C):N
    } else if (has_prefix(line, len, heat_index_prefix)) {
        line += sizeof(heat_index_prefix) - 1;
        len -= sizeof(heat_index_prefix) - 1;

        size_t spn = strcspn(line, " ");
        char* end = NULL;
        double double_parse = strtod(line, &end);
        check_parse(end, line + spn);
        line += spn + 1;
        check_pos_impl(line, >=, lend);

        info->heat_index_f = double_parse;

        spn = strcspn(line, "(");
        line += spn + 1;
        check_pos_impl(line, >=, lend);

        spn = strcspn(line, " ");
        double_parse = strtod(line, &end);
        check_parse(end, line + spn);

        info->heat_index_c = double_parse;
    // Dew Point: T F (T C)
    } else if (has_prefix(line, len, dew_point_prefix)) {
        line += sizeof(dew_point_prefix) - 1;
        len -= sizeof(dew_point_prefix) - 1;

        size_t spn = strcspn(line, " ");
        char* end = NULL;
        double double_parse = strtod(line, &end);
        check_parse(end, line + spn);
        line += spn + 1;
        check_pos_impl(line, >=, lend);

        info->dew_point_f = double_parse;

        spn = strcspn(line, "(");
        line += spn + 1;
        check_pos_impl(line, >=, lend);

        spn = strcspn(line, " ");
        double_parse = strtod(line, &end);
        check_parse(end, line + spn);

        info->dew_point_c = double_parse;
    // Relative Humidity: H%
    } else if (has_prefix(line, len, relative_humidity_prefix)) {
        line += sizeof(relative_humidity_prefix) - 1;
        len -= sizeof(relative_humidity_prefix) - 1;

        size_t spn = strcspn(line, "%");
        char* end = NULL;
        long long int_parse = strtoll(line, &end, 10);
        check_parse(end, line + spn);
        line += spn + 1;
        check_pos_impl(line, >, lend);

        info->humidity = int_parse;
    // Pressure (altimeter): X.Y in. Hg (N hPa)
    } else if (has_prefix(line, len, pressure_prefix)) {
        line += sizeof(pressure_prefix) - 1;
        len -= sizeof(pressure_prefix) - 1;

        size_t spn = strcspn(line, " ");
        char* end = NULL;
        double double_parse = strtod(line, &end);
        check_parse(end, line + spn);
        line += spn + 1;
        check_pos_impl(line, >, lend);

        info->pressure_mmhg = double_parse;

        spn = strcspn(line, "(");
        line += spn + 1;
        check_pos_impl(line, >=, lend);

        spn = strcspn(line, " ");
        long long int_parse = strtoll(line, &end, 10);
        check_parse(end, line + spn);

        info->pressure_hpa = int_parse;
    }

    return parse_fail;
}

int
parse_weather_info(const struct weather_curl_buffer *buf, struct weather_info *info)
{
    // Extract station information.
    const char* pos = buf->buffer;
    int parse_fail = 0;
#define check_pos() check_pos_impl(pos, >, buf->buffer + buf->size)

    // Fill in invalid data.
    free(info->station_town);
    info->station_town = strdup("<unknown>");
    free(info->station_state);
    info->station_state = strdup("<unknown>");
    info->year = 0;
    info->month = 0;
    info->day = 0;
    info->hour = -1;
    info->minute = -1;
    free(info->wind_direction);
    info->wind_direction = strdup("<unknown>");
    info->wind_azimuth = -1;
    info->wind_mph = -1;
    info->wind_knots = -1;
    info->wind_kmph = -1;
    info->wind_mps = -1;
    free(info->visibility);
    info->visibility = strdup("<unknown>");
    free(info->sky_condition);
    info->sky_condition = strdup("<unknown>");
    free(info->weather);
    info->weather = strdup("<unknown>");
    info->temp_c = -1000;
    info->temp_f = -1000;
    info->heat_index_c = -1000;
    info->heat_index_f = -1000;
    info->dew_point_c = -1000;
    info->dew_point_f = -1000;
    info->humidity = -1;
    info->pressure_mmhg = -1;
    info->pressure_hpa = -1;

    // TOWN, STATE (...
    if (!parse_fail) {
        size_t spn;

        const char *unknown_station = "Station name not available";

        if (!strncmp(pos, unknown_station, sizeof(unknown_station) - 1)) {
            free(info->station_town);
            info->station_town = strdup("?");
            free(info->station_state);
            info->station_state = strdup("?");
        } else {
            spn = strcspn(pos, ",");
            free(info->station_town);
            info->station_town = strndup(pos, spn);
            pos += spn + 1;
            check_pos();

            if (!parse_fail) {
                spn = strcspn(pos, "(");
                // Trim trailing space
                while (isspace(pos[spn - 1])) {
                    --spn;
                }
                // Skip leading space
                while (isspace(*pos)) {
                    ++pos;
                    --spn;
                }
                free(info->station_state);
                info->station_state = strndup(pos, spn);
            }
        }

        // Move to the end of the line.
        spn = strcspn(pos, "\n");
        pos += spn + 1;
        check_pos();
    }
    // Dec 22, 2024 - 04:20 PM EST / 2024.12.22 2120 UTC
    if (!parse_fail) {
        size_t spn;
        const char *lpos = pos;
        char *end;
        long int_parse;

        // Work on the entire line.
        spn = strcspn(pos, "\n");
        pos += spn + 1;
        check_pos();

        spn = strcspn(lpos, "/");
        lpos += spn + 2;
        check_pos_impl(lpos, >=, pos);

        int_parse = strtoll(lpos, &end, 10);
        check_parse(end, lpos + 4);
        lpos = end + 1;
        check_pos_impl(lpos, >=, pos);

        info->year = int_parse;

        int_parse = strtoll(lpos, &end, 10);
        check_parse(end, lpos + 2);
        lpos = end + 1;
        check_pos_impl(lpos, >=, pos);

        info->month = int_parse;

        int_parse = strtoll(lpos, &end, 10);
        check_parse(end, lpos + 2);
        lpos = end + 1;
        check_pos_impl(lpos, >=, pos);

        info->day = int_parse;

        info->hour = (*lpos - '0') * 10 + lpos[1] - '0';
        lpos += 2;
        check_pos_impl(lpos, >=, pos);
        info->minute = (*lpos - '0') * 10 + lpos[1] - '0';
    }

    while (*pos && pos < buf->buffer + buf->size) {
        size_t spn;
        const char *lpos = pos;

        // Work on the entire line.
        spn = strcspn(pos, "\n");
        pos += spn;
        // Handle contents without newlines at the end of the file.
        if (*pos == '\n') {
            ++pos;
        }

        if (parse_line(info, lpos, spn)) {
            fprintf(stderr, "failed to parse line '%.*s'\n", (int)spn, lpos);
            parse_fail = 1;
        }
    }

    return parse_fail;
}
