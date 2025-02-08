#include "weather.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARSE_SUCCEED(name, marker, info, lit) \
    do { \
        if (0) { \
            puts("string literal assertion: " lit); \
        } \
        const struct weather_curl_buffer buf = { \
            .buffer = lit, \
            .capacity = 0, \
            .size = sizeof(lit), \
        }; \
        fprintf(stderr, "========================================\n"); \
        fprintf(stderr, "Testing " name "\n"); \
        int res = parse_weather_info(&buf, &info); \
        if (res) { \
            fprintf(stderr, "unexpected failure to parse test '" name "'\n"); \
            marker = 0; \
            overall = EXIT_FAILURE; \
        } else { \
            fprintf(stderr, "succeeded in parsing\n"); \
        } \
    } while (0)

#define PARSE_FAIL(name, lit) \
    do { \
        if (0) { \
            puts("string literal assertion: " lit); \
        } \
        const struct weather_curl_buffer buf = { \
            .buffer = lit, \
            .capacity = 0, \
            .size = sizeof(lit), \
        }; \
        struct weather_info info = {0}; \
        fprintf(stderr, "========================================\n"); \
        fprintf(stderr, "Testing " name " (expected to fail)\n"); \
        int res = parse_weather_info(&buf, &info); \
        if (!res) { \
            fprintf(stderr, "unexpected successful parsing of '" name "'\n"); \
            overall = EXIT_FAILURE; \
        } else { \
            fprintf(stderr, "succeeded in detecting bad parsing\n"); \
        } \
    } while (0)

#define CHECK_STR(info, field, expect) \
    do { \
        if (!info.field) { \
            fprintf(stderr, "Unexpected `" #field "`: NULL\n"); \
        } else if (strcmp(info.field, expect)) { \
            fprintf(stderr, "Unexpected `" #field "`: '%s', expected '" expect "'\n", info.field); \
            overall = EXIT_FAILURE; \
        } \
    } while (0)
#define CHECK_DOUBLE(info, field, expect) \
    do { \
        if (info.field != expect) { \
            fprintf(stderr, "Unexpected `" #field "`: '%f', expected '" #expect "'\n", info.field); \
            overall = EXIT_FAILURE; \
        } \
    } while (0)
#define CHECK_INT(info, field, expect) \
    do { \
        if (info.field != expect) { \
            fprintf(stderr, "Unexpected `" #field "`: '%d', expected '" #expect "'\n", info.field); \
            overall = EXIT_FAILURE; \
        } \
    } while (0)

#define DEFAULT_STATION \
    "Tirstrup, Denmark (EKAH) 56-18N 010-37E 25M\n"
#define CHECK_DEFAULT_STATION(info) \
    do { \
        CHECK_STR(info, station_town, "Tirstrup"); \
        CHECK_STR(info, station_state, "Denmark"); \
    } while (0)
#define DEFAULT_TIME \
    "Dec 22, 2024 - 04:20 PM EST / 2024.12.22 2120 UTC\n"
#define CHECK_DEFAULT_TIME(info) \
    do { \
        CHECK_INT(info, year, 2024); \
        CHECK_INT(info, month, 12); \
        CHECK_INT(info, day, 22); \
        CHECK_INT(info, hour, 21); \
        CHECK_INT(info, minute, 20); \
    } while (0)
#define DEFAULT_WIND \
    "Wind: from the SSW (200 degrees) at 5 MPH (4 KT) (direction variable):0\n"
#define CHECK_DEFAULT_WIND(info) \
    do { \
        CHECK_STR(info, wind_direction, "SSW"); \
        CHECK_INT(info, wind_azimuth, 200); \
        CHECK_INT(info, wind_mph, 5); \
        CHECK_INT(info, wind_knots, 4); \
        CHECK_INT(info, wind_kmph, 7); \
        CHECK_INT(info, wind_mps, 2); \
    } while (0)
#define DEFAULT_VISIBILITY \
    "Visibility: greater than 7 mile(s):0\n"
#define CHECK_DEFAULT_VISIBILITY(info) \
    do { \
        CHECK_STR(info, visibility, "greater than 7 mile(s)"); \
    } while (0)
#define DEFAULT_SKY_CONDITIONS \
    "Sky conditions: mostly clear\n"
#define CHECK_DEFAULT_SKY_CONDITIONS(info) \
    do { \
        CHECK_STR(info, sky_condition, "mostly clear"); \
    } while (0)
#define DEFAULT_WEATHER \
    "Weather: rain\n"
#define CHECK_DEFAULT_WEATHER(info) \
    do { \
        CHECK_STR(info, weather, "rain"); \
    } while (0)
#define DEFAULT_TEMPERATURE \
    "Temperature: 37 F (3 C)\n"
#define CHECK_DEFAULT_TEMPERATURE(info) \
    do { \
        CHECK_DOUBLE(info, temp_f, 37); \
        CHECK_DOUBLE(info, temp_c, 3); \
    } while (0)
#define DEFAULT_HEAT_INDEX \
    "Heat index: 35 F (2 C):0\n"
#define CHECK_DEFAULT_HEAT_INDEX(info) \
    do { \
        CHECK_DOUBLE(info, heat_index_f, 35); \
        CHECK_DOUBLE(info, heat_index_c, 2); \
    } while (0)
#define DEFAULT_DEW_POINT \
    "Dew Point: 35 F (2 C)\n"
#define CHECK_DEFAULT_DEW_POINT(info) \
    do { \
        CHECK_DOUBLE(info, dew_point_f, 35); \
        CHECK_DOUBLE(info, dew_point_c, 2); \
    } while (0)
#define DEFAULT_RELATIVE_HUMIDITY \
    "Relative Humidity: 93%\n"
#define CHECK_DEFAULT_RELATIVE_HUMIDITY(info) \
    do { \
        CHECK_INT(info, humidity, 93); \
    } while (0)
#define DEFAULT_PRESSURE \
    "Pressure (altimeter): 29.26 in. Hg (0991 hPa)\n"
#define CHECK_DEFAULT_PRESSURE(info) \
    do { \
        CHECK_DOUBLE(info, pressure_mmhg, 29.26); \
        CHECK_INT(info, pressure_hpa, 991); \
    } while (0)
#define DEFAULT_FOOTER \
    "ob: EKAH 222120Z AUTO 20004KT 150V250 9999 FEW140/// 03/02 Q0991\n" \
    "cycle: 21\n"

int main(int argc, char* argv[])
{
    int overall = EXIT_SUCCESS;

    PARSE_FAIL("empty string", "");

    // Default contents parsing
    {
        int parse_ok = 1;
        struct weather_info info = {0};
        PARSE_SUCCEED("default values",
            parse_ok, info,
            DEFAULT_STATION
            DEFAULT_TIME
            DEFAULT_WIND
            DEFAULT_VISIBILITY
            DEFAULT_SKY_CONDITIONS
            DEFAULT_WEATHER
            DEFAULT_TEMPERATURE
            DEFAULT_HEAT_INDEX
            DEFAULT_DEW_POINT
            DEFAULT_RELATIVE_HUMIDITY
            DEFAULT_PRESSURE
            DEFAULT_FOOTER);
        if (parse_ok) {
            CHECK_DEFAULT_STATION(info);
            CHECK_DEFAULT_TIME(info);
            CHECK_DEFAULT_WIND(info);
            CHECK_DEFAULT_VISIBILITY(info);
            CHECK_DEFAULT_WEATHER(info);
            CHECK_DEFAULT_SKY_CONDITIONS(info);
            CHECK_DEFAULT_TEMPERATURE(info);
            CHECK_DEFAULT_HEAT_INDEX(info);
            CHECK_DEFAULT_DEW_POINT(info);
            CHECK_DEFAULT_RELATIVE_HUMIDITY(info);
            CHECK_DEFAULT_PRESSURE(info);
        }
    }

    // Unknown station parsing
    {
        int parse_ok = 1;
        struct weather_info info = {0};
        PARSE_SUCCEED("unknown station",
            parse_ok, info,
            "Station name not available\n"
            DEFAULT_TIME
            DEFAULT_WIND
            DEFAULT_VISIBILITY
            DEFAULT_SKY_CONDITIONS
            DEFAULT_WEATHER
            DEFAULT_TEMPERATURE
            DEFAULT_HEAT_INDEX
            DEFAULT_DEW_POINT
            DEFAULT_RELATIVE_HUMIDITY
            DEFAULT_PRESSURE
            DEFAULT_FOOTER);
        if (parse_ok) {
            CHECK_STR(info, station_town, "?");
            CHECK_STR(info, station_state, "?");
            CHECK_DEFAULT_TIME(info);
            CHECK_DEFAULT_WIND(info);
            CHECK_DEFAULT_VISIBILITY(info);
            CHECK_DEFAULT_WEATHER(info);
            CHECK_DEFAULT_SKY_CONDITIONS(info);
            CHECK_DEFAULT_TEMPERATURE(info);
            CHECK_DEFAULT_HEAT_INDEX(info);
            CHECK_DEFAULT_DEW_POINT(info);
            CHECK_DEFAULT_RELATIVE_HUMIDITY(info);
            CHECK_DEFAULT_PRESSURE(info);
        }
    }

    // Missing weather parsing.
    {
        int parse_ok = 1;
        struct weather_info info = {0};
        PARSE_SUCCEED("missing_weather",
            parse_ok, info,
            DEFAULT_STATION
            DEFAULT_TIME
            DEFAULT_WIND
            DEFAULT_VISIBILITY
            DEFAULT_SKY_CONDITIONS
            DEFAULT_TEMPERATURE
            DEFAULT_HEAT_INDEX
            DEFAULT_DEW_POINT
            DEFAULT_RELATIVE_HUMIDITY
            DEFAULT_PRESSURE
            DEFAULT_FOOTER);
        if (parse_ok) {
            CHECK_DEFAULT_STATION(info);
            CHECK_DEFAULT_TIME(info);
            CHECK_DEFAULT_WIND(info);
            CHECK_DEFAULT_VISIBILITY(info);
            CHECK_STR(info, weather, "<unknown>");
            CHECK_DEFAULT_SKY_CONDITIONS(info);
            CHECK_DEFAULT_TEMPERATURE(info);
            CHECK_DEFAULT_HEAT_INDEX(info);
            CHECK_DEFAULT_DEW_POINT(info);
            CHECK_DEFAULT_RELATIVE_HUMIDITY(info);
            CHECK_DEFAULT_PRESSURE(info);
        }
    }

    // Calm wind parsing
    {
        int parse_ok = 1;
        struct weather_info info = {0};
        PARSE_SUCCEED("calm wind",
            parse_ok, info,
            DEFAULT_STATION
            DEFAULT_TIME
            "Wind: Calm:0\n"
            DEFAULT_VISIBILITY
            DEFAULT_SKY_CONDITIONS
            DEFAULT_WEATHER
            DEFAULT_TEMPERATURE
            DEFAULT_HEAT_INDEX
            DEFAULT_DEW_POINT
            DEFAULT_RELATIVE_HUMIDITY
            DEFAULT_PRESSURE
            DEFAULT_FOOTER);
        if (parse_ok) {
            CHECK_DEFAULT_STATION(info);
            CHECK_DEFAULT_TIME(info);
            CHECK_STR(info, wind_direction, "μ");
            CHECK_INT(info, wind_azimuth, -1);
            CHECK_INT(info, wind_mph, 0);
            CHECK_INT(info, wind_knots, 0);
            CHECK_INT(info, wind_kmph, 0);
            CHECK_INT(info, wind_mps, 0);
            CHECK_DEFAULT_VISIBILITY(info);
            CHECK_DEFAULT_WEATHER(info);
            CHECK_DEFAULT_SKY_CONDITIONS(info);
            CHECK_DEFAULT_TEMPERATURE(info);
            CHECK_DEFAULT_HEAT_INDEX(info);
            CHECK_DEFAULT_DEW_POINT(info);
            CHECK_DEFAULT_RELATIVE_HUMIDITY(info);
            CHECK_DEFAULT_PRESSURE(info);
        }
    }

    // Variable wind parsing
    {
        int parse_ok = 1;
        struct weather_info info = {0};
        PARSE_SUCCEED("variable wind",
            parse_ok, info,
            DEFAULT_STATION
            DEFAULT_TIME
            "Wind: Variable at 5 MPH (4 KT)\n"
            DEFAULT_VISIBILITY
            DEFAULT_SKY_CONDITIONS
            DEFAULT_WEATHER
            DEFAULT_TEMPERATURE
            DEFAULT_HEAT_INDEX
            DEFAULT_DEW_POINT
            DEFAULT_RELATIVE_HUMIDITY
            DEFAULT_PRESSURE
            DEFAULT_FOOTER);
        if (parse_ok) {
            CHECK_DEFAULT_STATION(info);
            CHECK_DEFAULT_TIME(info);
            CHECK_STR(info, wind_direction, "μ");
            CHECK_INT(info, wind_azimuth, -1);
            CHECK_INT(info, wind_mph, 5);
            CHECK_INT(info, wind_knots, 4);
            CHECK_INT(info, wind_kmph, 7);
            CHECK_INT(info, wind_mps, 2);
            CHECK_DEFAULT_VISIBILITY(info);
            CHECK_DEFAULT_WEATHER(info);
            CHECK_DEFAULT_SKY_CONDITIONS(info);
            CHECK_DEFAULT_TEMPERATURE(info);
            CHECK_DEFAULT_HEAT_INDEX(info);
            CHECK_DEFAULT_DEW_POINT(info);
            CHECK_DEFAULT_RELATIVE_HUMIDITY(info);
            CHECK_DEFAULT_PRESSURE(info);
        }
    }

    return overall;
}
