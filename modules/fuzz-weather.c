#include "weather.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char* argv[])
{
    if (argc != 2) {
        return EXIT_FAILURE;
    }

    struct weather_curl_buffer buf;
#define BUF_INIT_SIZE ((size_t)16384)
    buf.buffer = (char*)malloc(BUF_INIT_SIZE * sizeof(char));
    if (!buf.buffer) {
        printf("failed to reallocate %zu bytes\n", BUF_INIT_SIZE);
        free(buf.buffer);
        return EXIT_FAILURE;
    }
    buf.size = 0;
    buf.capacity = BUF_INIT_SIZE;

    int fin = open(argv[1], O_CLOEXEC);
    if (fin < 0) {
        printf("failed to open input %s: %s\n", argv[1], strerror(errno));
        free(buf.buffer);
        return EXIT_FAILURE;
    }

    size_t bytes_read;
    do {
        bytes_read = read(fin, buf.buffer + buf.size, buf.capacity - buf.size);
        if (bytes_read < 0) {
            printf("failed to read input: %s\n", strerror(errno));
            close(fin);
            return EXIT_FAILURE;
        }

        buf.size += bytes_read;
        if (buf.size == buf.capacity) {
            size_t newcap = 2 * buf.capacity;
            char* newbuf = (char*)realloc(buf.buffer, newcap * sizeof(char));
            if (!newbuf) {
                printf("failed to reallocate %zu bytes\n", newcap);
                free(buf.buffer);
                close(fin);
                return EXIT_FAILURE;
            }

            buf.buffer = newbuf;
            buf.capacity = newcap;
        }
    } while (bytes_read);
    buf.buffer[buf.size] = '\0';

    close(fin);

    struct weather_info info;
    memset(&info, 0, sizeof(info));
    int ret = parse_weather_info(&buf, &info);

    free(buf.buffer);

    if (!ret) {
#define print_field(s, f, t) \
    printf(#f ": " t "\n", s.f)

        print_field(info, station_town, "%s");
        print_field(info, station_state, "%s");
        print_field(info, year, "%d");
        print_field(info, month, "%d");
        print_field(info, day, "%d");
        print_field(info, hour, "%d");
        print_field(info, minute, "%d");
        print_field(info, wind_direction, "%s");
        print_field(info, wind_azimuth, "%d");
        print_field(info, wind_mph, "%d");
        print_field(info, wind_kmph, "%d");
        print_field(info, wind_mps, "%d");
        print_field(info, visibility, "%s");
        print_field(info, sky_condition, "%s");
        print_field(info, weather, "%s");
        print_field(info, temp_c, "%f");
        print_field(info, temp_f, "%f");
        print_field(info, heat_index_c, "%f");
        print_field(info, heat_index_f, "%f");
        print_field(info, dew_point_c, "%f");
        print_field(info, dew_point_f, "%f");
        print_field(info, humidity, "%d");
        print_field(info, pressure_mmhg, "%f");
        print_field(info, pressure_hpa, "%d");
    }

    free(info.station_town);
    free(info.station_state);
    free(info.wind_direction);
    free(info.visibility);
    free(info.sky_condition);
    free(info.weather);

    return ret;
}
