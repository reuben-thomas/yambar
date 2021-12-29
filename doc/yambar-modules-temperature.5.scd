yambar-modules-temperature(5)

# NAME
temperature - This module displays the temperature of the thermal zones available on
the system

# TAGS

[[ *Name*
:[ *Type*
:[ *Description*
|  temperature
:  string
:  Current temperature of a given thermal zone

# CONFIGURATION

[[ *Name*
:[ *Type*
:[ *Req*
:[ *Description*
|  interval
:  int
:  no
:  Refresh interval of the temperature in ms (default=500). Cannot be less then 500 ms
|  unit
:  string
:  no
:  Unit of the temperature. It can be Celsius (C or Celsius) or Fahrenheit (F or
Fahrenheit) (default=C)

|  thermal_zone
:  int
:  yes
:  Identifier of the thermal zone to read

# EXAMPLES

```
bar:
  left:
    - temperature:
        interval: 1000
        thermal_zone: 0
        content:
          string: {text: "{temperature}℃ (DEGREE CELSIUS)"}
```

# SEE ALSO

*yambar-modules*(5), *yambar-particles*(5), *yambar-tags*(5), *yambar-decorations*(5)
