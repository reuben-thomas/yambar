yambar-modules-pulse(5)

# NAME
pulse - Monitors one pulseaudio source and sink for volume and mute/unmute changes

# TAGS

[[ *Name*
:[ *Type*
:[ *Description*
|  online
:  bool
:  True when the Pulseaudio connection has successfully been opened
|  sink_online
:  bool
:  True when the Pulseaudio sink (output) exists and is usable
|  source_online
:  bool
:  True when the Pulseaudio source (input) exists and is usable
|  sink_percent
:  range
:  Sink (output) volume level in percentage, with min and max as start and end range values
|  source_percent
:  range
:  Source (input) volume level in percentage, with min and max as start and end range values
|  sink_muted
:  bool
:  True if sink is muted, otherwise false
|  source_muted
:  bool
:  True if source is muted, otherwise false


# CONFIGURATION

[[ *Name*
:[ *Type*
:[ *Req*
:[ *Description*
|  sink
:  string
:  no
:  Sink name to monitor. Defaults to _@DEFAULT\_SINK@_
|  source
:  string
:  no
:  Source name to monitor. Defaults to _@DEFAULT\_SOURCE@_


# EXAMPLES

```
bar:
  left:
    - pulse:
        content: {string: {text: "{sink_percent} %"}}
```

# SEE ALSO

*yambar-modules*(5), *yambar-particles*(5), *yambar-tags*(5), *yambar-decorations*(5)

