#pragma once
#include <stddef.h>

struct weather_info
{
    char* station_town;
    char* station_state;
    int year;
    int month;
    int day;
    int hour;
    int minute;
    char* wind_direction;
    int wind_azimuth;
    int wind_mph;
    int wind_knots;
    int wind_kmph;
    int wind_mps;
    char* visibility;
    char* sky_condition;
    char* weather;
    double temp_c;
    double temp_f;
    double heat_index_c;
    double heat_index_f;
    double dew_point_c;
    double dew_point_f;
    int humidity;
    double pressure_mmhg;
    int pressure_hpa;
};

struct weather_curl_buffer
{
    char *buffer;
    size_t size;
    size_t capacity;
};

int
parse_weather_info(const struct weather_curl_buffer *buf, struct weather_info *info);
