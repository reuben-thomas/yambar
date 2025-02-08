yambar-modules-weather(5)

# NAME
weather - This module monitors weather station reports

# DESCRIPTION

This module monitors weather reports in NOAA's format from weather
stations around the world. It instantiates the provided _content_
particle for the station specified.

# TAGS

[[ *Name*
:[ *Type*
:< *Description*
|  station_town
:  string
:  Town name of the station.
|  station_state
:  string
:  Region name of the station.
|  year
:  int
:  Year of the weather report.
|  month
:  int
:  Month of the weather report.
|  day
:  int
:  Day of the weather report.
|  hour
:  int
:  Hour of the weather report.
|  minute
:  int
:  Minute of the weather report.
|  wind_direction
:  string
:  Direction of the wind or *μ* if calm or variable.
|  wind_azimuth
:  int
:  The azimuth of the wind or *-1* if calm or variable.
|  wind_mph
:  int
:  The speed of the wind in miles per hour (m/h).
|  wind_knots
:  int
:  The speed of the wind in nautical knots.
|  wind_kmph
:  int
:  The speed of the wind in kilometers per hour (km/h).
|  wind_mps
:  int
:  The speed of the wind in meters per second (m/s).
|  visibility
:  string
:  Visibility report.
|  sky_condition
:  string
:  Description of the sky condition (e.g., cloudy).
|  weather
:  string
:  Description of the weather (not always available).
|  temp_c
:  float
:  Temperature in Celsius.
|  temp_f
:  float
:  Temperature in Fahrenheit.
|  heat_index_c
:  float
:  Heat index in Celsius.
|  heat_index_f
:  float
:  Heat index in Fahrenheit.
|  dew_point_c
:  float
:  Dew point in Celsius.
|  dew_point_f
:  float
:  Dew point in Fahrenheit.
|  humidity
:  int
:  Relative humidity (0-100).
|  pressure_mmhg
:  float
:  Pressure in millimeters of Mercury (mmHg).
|  pressure_hpa
:  int
:  Pressure in hectapascals (hPa).


# CONFIGURATION

[[ *Name*
:[ *Type*
:[ *Req*
:< *Description*
|  station
:  string
:  yes
:  Name of the station to report weather.
|  host
:  string
:  no
:  Website to fetch the weather report from. Defaults to NOAA's
   website which uses ICAO names for stations.


# EXAMPLES

Display weather report for KIAD (Washington Dulles Airport).

```
bar:
  left:
    - weather:
        station: KIAD
        content:
          map:
			TODO
```

# SEE ALSO

*yambar-modules*(5), *yambar-particles*(5), *yambar-tags*(5), *yambar-decorations*(5)

